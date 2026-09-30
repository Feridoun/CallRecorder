#include "Uploader.h"

#include "HttpClient.h"
#include "Json.h"
#include "Session.h"
#include "UploadLogic.h"
#include "Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <exception>
#include <memory>

using nlohmann::json;

namespace {

constexpr DWORD kIdleWaitMs = 10 * 60 * 1000;      // nothing in flight
constexpr DWORD kProcessingWaitMs = 20 * 1000;     // Speakr is working on something
constexpr DWORD kFirstRetryMs = 60 * 1000;         // after a network/server failure, doubling...
constexpr DWORD kMaxRetryMs = 15 * 60 * 1000;      // ...up to this

std::string BuildNotes(const Session& session, int speakrId) {
    std::string notes;
    if (session.status == "interrupted") {
        notes += "_This recording was cut short by a crash or power loss._\n\n";
    }
    if (!session.markers.empty()) {
        notes += "**Markers**\n\n";
        for (const auto& marker : session.markers) {
            std::string at = ToUtf8(FormatDuration(marker.offsetSeconds));
            if (speakrId) {
                // Relative link: opens the recording in the Speakr UI, seeked to the marker.
                at = "[" + at + "](/recordings/" + std::to_string(speakrId) +
                     "?t=" + std::to_string(static_cast<int>(marker.offsetSeconds)) + ")";
            }
            notes += "- " + at + " " + ToUtf8(marker.label) + "\n";
        }
    }
    return notes;
}

int JsonId(const std::string& body) {
    auto parsed = json::parse(body, nullptr, false);
    if (!parsed.is_object()) return 0;
    if (int id = JsonGet(parsed, "id", 0)) return id;
    for (const char* wrapper : {"recording", "tag"}) {
        auto inner = parsed.find(wrapper);
        if (inner != parsed.end()) {
            if (int id = JsonGet(*inner, "id", 0)) return id;
        }
    }
    return 0;
}

std::string UrlEncode(const std::string& text) {
    static const char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (unsigned char c : text) {
        if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            out += static_cast<char>(c);
        } else {
            out += '%';
            out += kHex[c >> 4];
            out += kHex[c & 15];
        }
    }
    return out;
}

// Load-modify-save of one sidecar under the file lock, so a change made in the
// tray or the Recordings window while we were talking to Speakr isn't lost.
// The session is refreshed from disk first; fn returns false to leave the file
// alone (its state moved on). Returns true if fn ran and the save worked.
template <typename Fn>
bool UpdateSession(Session& session, Fn&& fn) {
    std::lock_guard lock(Session::FileMutex());
    auto fresh = Session::Load(session.metaPath);
    if (!fresh) return false;  // discarded meanwhile; don't bring the file back
    session = std::move(*fresh);
    if (!fn(session)) return false;
    return session.Save();
}

bool IsState(const Session& session, const char* state) {
    return session.uploadState == state;
}

}  // namespace

Uploader::Uploader(HWND notifyWindow, std::wstring sessionsDir)
    : notifyWindow_(notifyWindow),
      sessionsDir_(std::move(sessionsDir)),
      wakeEvent_(CreateEventW(nullptr, FALSE, FALSE, nullptr)),
      stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {
    thread_ = std::thread(&Uploader::Run, this);
}

Uploader::~Uploader() {
    SetEvent(stopEvent_);
    {
        std::lock_guard lock(mutex_);
        if (activeClient_) activeClient_->Abort();  // don't wait out a long upload on exit
    }
    thread_.join();
    CloseHandle(wakeEvent_);
    CloseHandle(stopEvent_);
}

void Uploader::Wake() {
    SetEvent(wakeEvent_);
}

std::vector<Uploader::Event> Uploader::TakeEvents() {
    std::lock_guard lock(mutex_);
    return std::exchange(events_, {});
}

Uploader::Status Uploader::GetStatus() {
    std::lock_guard lock(mutex_);
    return status_;
}

std::wstring Uploader::ServerUrl() {
    std::lock_guard lock(mutex_);
    return serverUrl_;
}

std::vector<std::wstring> Uploader::KnownTags() {
    std::lock_guard lock(mutex_);
    return knownTags_;
}

void Uploader::Notify(Event event) {
    {
        std::lock_guard lock(mutex_);
        events_.push_back(std::move(event));
    }
    PostMessageW(notifyWindow_, kMessage, 0, 0);
}

void Uploader::SetBlocker(std::wstring blocker) {
    bool changed;
    {
        std::lock_guard lock(mutex_);
        changed = status_.blocker != blocker;
        status_.blocker = blocker;
    }
    // Tell the user once when uploads stall, not on every retry.
    if (changed && !blocker.empty()) Notify({L"Uploads paused", blocker, {}, true});
}

void Uploader::Run() {
    DWORD wait = 2000;  // let the tray settle before the first pass
    HANDLE handles[] = {stopEvent_, wakeEvent_};

    while (WaitForMultipleObjects(2, handles, FALSE, wait) != WAIT_OBJECT_0) {
        // Nothing thrown while talking to Speakr or reading files may end this
        // thread: it would take the whole app, and a recording, with it.
        try {
            wait = RunCycle();
        } catch (const std::exception&) {
            wait = kFirstRetryMs;
        } catch (...) {
            wait = kFirstRetryMs;
        }
    }
}

DWORD Uploader::RunCycle() {
    Config config = Config::Load();
    if (config.serverUrl != tagServer_) {  // tag ids belong to one server
        tagIds_.clear();
        tagServer_ = config.serverUrl;
        std::lock_guard lock(mutex_);
        knownTags_.clear();
    }
    {
        std::lock_guard lock(mutex_);
        serverUrl_ = config.serverUrl;
    }

    std::string token = ReadSpeakrToken();
    std::unique_ptr<HttpClient> http;
    // Declared after http so it runs first: activeClient_ must never point at
    // a client that is being destroyed, even if something throws.
    struct ClearActive {
        Uploader* uploader;
        ~ClearActive() {
            std::lock_guard lock(uploader->mutex_);
            uploader->activeClient_ = nullptr;
        }
    } clearActive{this};

    if (config.serverUrl.empty() || token.empty()) {
        SetBlocker(L"Speakr isn't set up yet. Choose Speakr connection in the tray menu.");
    } else {
        http = std::make_unique<HttpClient>(config.serverUrl, token);
        std::lock_guard lock(mutex_);
        activeClient_ = http.get();
    }
    auto stopping = [&] { return WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0; };
    // The destructor may have run Abort() before activeClient_ was set.
    if (stopping()) return kIdleWaitMs;
    if (http) RefreshTags(*http);
    if (stopping()) return kIdleWaitMs;

    Cycle cycle;
    cycle.reachable = http != nullptr;
    for (const auto& path : Session::ListMetaFiles(sessionsDir_)) {
        if (stopping()) return kIdleWaitMs;
        try {
            auto session = Session::Load(path);
            if (!session || session->status == "recording") continue;
            ProcessSession(http.get(), *session, config, cycle);

            const std::string& state = session->uploadState;
            if (state == UploadState::kPending) ++cycle.counts.waiting;
            else if (state == UploadState::kUploaded) ++cycle.counts.processing;
            else if (state == UploadState::kHeld) ++cycle.counts.held;
            else if (state == UploadState::kRejected || state == UploadState::kFailed ||
                     state == UploadState::kMissing || state == UploadState::kStuck) {
                ++cycle.counts.problems;
            }
        } catch (const std::exception&) {  // one bad session: skip it this pass
        } catch (...) {
        }
    }

    if (cycle.reachable) {
        consecutiveFailures_ = 0;
        SetBlocker({});
    }
    {
        std::lock_guard lock(mutex_);
        cycle.counts.blocker = status_.blocker;
        status_ = cycle.counts;
    }
    if (cycle.newlyHeld > 0) {
        Notify({L"Recordings held back",
                std::to_wstring(cycle.newlyHeld) +
                    (cycle.newlyHeld == 1 ? L" recording was" : L" recordings were") +
                    L" made before Speakr was set up or for another server. "
                    L"Choose Upload held recordings in the tray menu to send them.",
                {},
                false});
    } else {
        PostMessageW(notifyWindow_, kMessage, 0, 0);
    }

    if (http && !cycle.reachable) {
        return std::min<DWORD>(kFirstRetryMs << std::min(consecutiveFailures_, 4), kMaxRetryMs);
    }
    DWORD wait = cycle.fastPolling > 0 ? kProcessingWaitMs : kIdleWaitMs;
    if (cycle.nextDueSeconds) {  // wake when the next grace period ends
        int64_t seconds = std::clamp<int64_t>(*cycle.nextDueSeconds, 1, kIdleWaitMs / 1000);
        wait = std::min<DWORD>(wait, static_cast<DWORD>(seconds * 1000));
    }
    return wait;
}

void Uploader::ProcessSession(HttpClient* http, Session& session, const Config& config, Cycle& cycle) {
    // Sidecars from before 1.2 have no server: they belong to the configured
    // one. (If none is set up yet they wait, and are bound once one is.)
    if (!session.serverUrl && !config.serverUrl.empty() &&
        (IsState(session, UploadState::kPending) || IsState(session, UploadState::kUploaded))) {
        UpdateSession(session, [&](Session& s) {
            if (s.serverUrl) return false;
            s.serverUrl = config.serverUrl;
            return true;
        });
    }
    bool sameServer = session.serverUrl && UploadLogic::SameServer(*session.serverUrl, config.serverUrl);
    bool canTalk = http && cycle.reachable;

    if (IsState(session, UploadState::kPending)) {
        if (config.serverUrl.empty() || !session.serverUrl) return;  // Speakr isn't set up yet
        if (!sameServer) {
            // Never send a recording to a server it wasn't made for; the user
            // decides (tray menu / Recordings window).
            bool held = UpdateSession(session, [](Session& s) {
                if (!IsState(s, UploadState::kPending)) return false;
                s.uploadState = UploadState::kHeld;
                return true;
            });
            if (held) ++cycle.newlyHeld;
        } else if (UploadLogic::IsInFuture(session.uploadAfterUtc)) {
            int64_t left = *UploadLogic::SecondsUntil(session.uploadAfterUtc);
            cycle.nextDueSeconds = std::min(cycle.nextDueSeconds.value_or(left), left);
        } else if (canTalk) {
            cycle.reachable = Upload(*http, session, config);
        }
    } else if (IsState(session, UploadState::kUploaded)) {
        // A session bound to another server can't be checked here; it stays
        // "processing" but isn't worth polling for.
        if (sameServer && canTalk) cycle.reachable = Check(*http, session, config);
        if (sameServer && IsState(session, UploadState::kUploaded)) ++cycle.fastPolling;
    } else if (IsState(session, UploadState::kDone)) {
        ApplyRetention(session, config);
    }
}

bool Uploader::IsGlobalFailure(const HttpResponse& response) {
    if (response.status == 0) {
        // Exit aborted the request: not worth telling the user about.
        if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) return true;
        ++consecutiveFailures_;
        SetBlocker(L"Can't reach Speakr: " + response.Describe());
        return true;
    }
    if (response.status == 401 || response.status == 403) {
        SetBlocker(L"Speakr rejected the API token. Enter a new one in Settings from the tray menu.");
        return true;
    }
    if (response.status == 408 || response.status == 429 || response.status >= 500) {  // 408: a proxy gave up waiting
        ++consecutiveFailures_;
        SetBlocker(L"Speakr is busy or failing: " + response.Describe());
        return true;
    }
    return false;
}

void Uploader::Reject(Session& session, const std::wstring& reason, bool clearAttempt) {
    bool rejected = UpdateSession(session, [&](Session& s) {
        if (!IsState(s, UploadState::kPending)) return false;
        s.uploadState = UploadState::kRejected;
        s.uploadError = reason;
        if (clearAttempt) s.uploadAttemptUtc.clear();
        return true;
    });
    if (rejected) Notify({L"Upload refused", session.title + L": " + reason, {}, true});
}

HttpResponse Uploader::RefreshTags(HttpClient& http) {
    HttpResponse list = http.Get(L"/api/v1/tags");
    if (!list.Ok()) return list;
    auto parsed = json::parse(list.body, nullptr, false);
    if (!parsed.is_object() || !parsed.contains("tags") || !parsed["tags"].is_array()) return list;
    std::vector<std::wstring> names;
    for (const auto& tag : parsed["tags"]) {
        std::string name = JsonGet(tag, "name", "");
        int id = JsonGet(tag, "id", 0);
        if (!name.empty() && id) {
            names.push_back(FromUtf8(name));
            tagIds_[names.back()] = id;
        }
    }
    std::lock_guard lock(mutex_);
    knownTags_ = std::move(names);
    return list;
}

bool Uploader::ResolveTags(HttpClient& http, const std::vector<std::wstring>& names, std::vector<int>& ids) {
    bool allKnown = std::all_of(names.begin(), names.end(),
                                [&](const std::wstring& name) { return tagIds_.contains(name); });
    if (!allKnown) {
        HttpResponse list = RefreshTags(http);
        if (!list.Ok()) return !IsGlobalFailure(list);  // tags are optional; upload without them
        for (const auto& name : names) {
            if (tagIds_.contains(name)) continue;
            HttpResponse created = http.SendJson(L"POST", L"/api/v1/tags", json{{"name", ToUtf8(name)}}.dump());
            if (created.Ok()) {
                if (int id = JsonId(created.body)) tagIds_[name] = id;
            } else if (IsGlobalFailure(created)) {
                return false;
            }
        }
    }
    for (const auto& name : names) {
        if (auto found = tagIds_.find(name); found != tagIds_.end()) ids.push_back(found->second);
    }
    return true;
}

bool Uploader::FindEarlierUpload(HttpClient& http, const Session& session, int& id) {
    // GET /api/v1/recordings lists the user's recordings, newest first, with
    // ?q= (case-insensitive substring of title/participants), ?per_page (max
    // 100) and ?page; the response is {"recordings": [{"id", "title",
    // "meeting_date" (naive UTC ISO), ...}], "pagination": {"has_next", ...}}.
    // Source: src/api/api_v1.py, list_recordings(), in murtaza-nasir/speakr.
    // meeting_date is what we sent in the upload, so title + instant identify it.
    id = 0;
    std::string title = ToUtf8(session.title);
    std::wstring meetingDate = FormatIsoUtc(session.startedUtc);
    for (int page = 1; page <= 3; ++page) {  // an upload from moments ago is on the first page
        std::wstring path = L"/api/v1/recordings?per_page=100&sort_by=created_at&sort_order=desc&page=" +
                            std::to_wstring(page);
        if (!title.empty()) path += L"&q=" + FromUtf8(UrlEncode(title));
        HttpResponse response = http.Get(path);
        if (!response.Ok()) {
            if (IsGlobalFailure(response)) return false;
            id = -1;  // no usable list on this Speakr
            return true;
        }
        auto parsed = json::parse(response.body, nullptr, false);
        if (int found = UploadLogic::FindUploadedMatch(parsed, title, meetingDate)) {
            id = found;
            return true;
        }
        if (!UploadLogic::ListHasNext(parsed)) break;
    }
    return true;
}

bool Uploader::Upload(HttpClient& http, Session& session, const Config& config) {
    if (GetFileAttributesW(session.audioPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Reject(session, L"The audio file is missing.");
        return true;
    }

    // An earlier attempt whose outcome we never learned (timeout, network
    // drop, app exit) may have reached Speakr. Look for it before sending the
    // file again, or it would be uploaded, and paid for, twice.
    bool unverified = false;
    if (!session.uploadAttemptUtc.empty()) {
        int existing = 0;
        if (!FindEarlierUpload(http, session, existing)) return false;
        if (existing > 0) {
            UpdateSession(session, [&](Session& s) {
                if (!IsState(s, UploadState::kPending)) return false;
                s.speakrId = existing;
                s.uploadState = UploadState::kUploaded;
                s.uploadError.clear();
                s.uploadedUtc = NowIsoUtc();
                s.uploadAttemptUtc.clear();
                return true;
            });
            return true;
        }
        unverified = existing < 0;
    }

    std::vector<int> tagIds;
    if (!ResolveTags(http, session.tags.value_or(config.tags), tagIds)) return false;

    // Record the attempt before sending, from the file as it is now: the user
    // may have marked the recording "keep on this PC" since this pass began.
    bool proceed = UpdateSession(session, [&](Session& s) {
        if (!IsState(s, UploadState::kPending) || s.sensitive || UploadLogic::IsInFuture(s.uploadAfterUtc) ||
            !s.serverUrl || !UploadLogic::SameServer(*s.serverUrl, config.serverUrl)) {
            return false;
        }
        s.uploadAttemptUtc = NowIsoUtc();
        return true;
    });
    if (!proceed) return true;

    HttpClient::Fields fields = {
        {"title", ToUtf8(session.title)},
        {"meeting_date", ToUtf8(FormatIsoUtc(session.startedUtc))},
    };
    if (std::string notes = BuildNotes(session, 0); !notes.empty()) fields.push_back({"notes", notes});
    if (!config.hotwords.empty()) fields.push_back({"hotwords", ToUtf8(config.hotwords)});
    for (size_t i = 0; i < tagIds.size(); ++i) {
        fields.push_back({"tag_ids[" + std::to_string(i) + "]", std::to_string(tagIds[i])});
    }

    HttpResponse response = http.PostMultipart(L"/api/v1/recordings/upload", fields, "file", session.audioPath,
                                               ToUtf8(session.id) + ".opus", "audio/ogg");
    if (!response.Ok()) {
        // Nothing was sent: the file is unreadable, not the network.
        if (response.status == 0 && (response.error == ERROR_FILE_NOT_FOUND || response.error == ERROR_FILE_TOO_LARGE)) {
            Reject(session, response.Describe());
            return true;
        }
        // A global failure leaves the attempt marked: the outcome is unknown.
        if (IsGlobalFailure(response)) return false;
        Reject(session, response.status == 413
                            ? L"Too large for the Speakr server or a proxy in front of it (HTTP 413)."
                            : response.Describe());
        return true;
    }
    int id = JsonId(response.body);
    if (!id) {
        // It is in Speakr; keep the attempt marked so a retry finds it
        // instead of uploading it again.
        Reject(session, L"Speakr accepted the upload but didn't return a recording id.", false);
        return true;
    }

    UpdateSession(session, [&](Session& s) {
        s.speakrId = id;
        s.uploadState = UploadState::kUploaded;
        s.uploadError.clear();
        s.uploadedUtc = NowIsoUtc();
        s.uploadAttemptUtc.clear();
        return true;
    });
    if (unverified) {
        Notify({L"Uploaded again",
                session.title + L"\nUploaded again after an earlier attempt was interrupted. "
                                L"Check Speakr for a duplicate.",
                {},
                true});
    }

    // Now that the id is known, turn marker times into links that open the
    // recording at that point. Best effort: plain times are already there.
    if (!session.markers.empty()) {
        http.SendJson(L"PUT", L"/api/v1/recordings/" + std::to_wstring(id) + L"/notes",
                      json{{"notes", BuildNotes(session, id)}}.dump());
    }
    return true;
}

bool Uploader::Check(HttpClient& http, Session& session, const Config& config) {
    HttpResponse response = http.Get(L"/api/v1/recordings/" + std::to_wstring(session.speakrId) + L"/status");
    std::wstring url = config.serverUrl + L"/recordings/" + std::to_wstring(session.speakrId);

    if (response.status == 404) {
        // Deleted in Speakr, or this isn't the server it was uploaded to. Not
        // "done": the local audio must stay, so retention never touches it.
        static const wchar_t kMissing[] = L"Speakr no longer has this recording. The audio is kept on this PC.";
        bool changed = UpdateSession(session, [](Session& s) {
            if (!IsState(s, UploadState::kUploaded)) return false;
            s.uploadState = UploadState::kMissing;
            s.uploadError = kMissing;
            s.finishedUtc = NowIsoUtc();
            return true;
        });
        if (changed) Notify({L"Recording missing in Speakr", session.title + L": " + kMissing, url, true});
        return true;
    }
    if (!response.Ok()) return !IsGlobalFailure(response);

    auto parsed = json::parse(response.body, nullptr, false);
    UploadLogic::StatusReport report = UploadLogic::ClassifyStatus(parsed);
    if (report.status == UploadLogic::SpeakrStatus::Completed) {
        bool changed = UpdateSession(session, [](Session& s) {
            if (!IsState(s, UploadState::kUploaded)) return false;
            s.uploadState = UploadState::kDone;
            s.finishedUtc = NowIsoUtc();
            return true;
        });
        if (changed) Notify({L"Notes ready", session.title + L"\nClick to open in Speakr.", url});
    } else if (report.status == UploadLogic::SpeakrStatus::Failed) {
        std::wstring error = report.errorMessage.empty() ? L"Speakr couldn't process the recording"
                                                         : FromUtf8(report.errorMessage);
        bool changed = UpdateSession(session, [&](Session& s) {
            if (!IsState(s, UploadState::kUploaded)) return false;
            s.uploadState = UploadState::kFailed;
            s.uploadError = error;
            s.finishedUtc = NowIsoUtc();
            return true;
        });
        if (changed) Notify({L"Transcription failed", session.title + L": " + error, url, true});
    } else if (UploadLogic::IsOverdue(session.uploadedUtc)) {
        static const wchar_t kStuck[] =
            L"Speakr hadn't finished this recording a day after it was uploaded. Check it in Speakr.";
        bool changed = UpdateSession(session, [](Session& s) {
            if (!IsState(s, UploadState::kUploaded)) return false;
            s.uploadState = UploadState::kStuck;
            s.uploadError = kStuck;
            s.finishedUtc = NowIsoUtc();
            return true;
        });
        if (changed) Notify({L"Recording not finished", session.title + L": " + kStuck, url, true});
    }
    return true;
}

void Uploader::ApplyRetention(Session& session, const Config& config) {
    // Only ever for a recording Speakr finished; missing, stuck and failed
    // ones keep their audio.
    if (!IsState(session, UploadState::kDone) || config.keepAudioDays <= 0 || session.audioDeleted) return;
    double age = DaysSinceIsoUtc(session.finishedUtc);
    if (age < config.keepAudioDays) return;
    UpdateSession(session, [](Session& s) {
        if (!IsState(s, UploadState::kDone) || s.audioDeleted) return false;
        if (!DeleteFileW(s.audioPath.c_str()) && GetLastError() != ERROR_FILE_NOT_FOUND) return false;
        s.audioDeleted = true;
        return true;
    });
}
