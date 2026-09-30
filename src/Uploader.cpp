#include "Uploader.h"

#include "HttpClient.h"
#include "Session.h"
#include "Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
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
    if (parsed.contains("id") && parsed["id"].is_number_integer()) return parsed["id"].get<int>();
    for (const char* wrapper : {"recording", "tag"}) {
        if (parsed.contains(wrapper) && parsed[wrapper].is_object()) {
            const auto& inner = parsed[wrapper];
            if (inner.contains("id") && inner["id"].is_number_integer()) return inner["id"].get<int>();
        }
    }
    return 0;
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
        if (config.serverUrl.empty() || token.empty()) {
            SetBlocker(L"Speakr isn't set up yet. Open Settings from the tray menu.");
        } else {
            http = std::make_unique<HttpClient>(config.serverUrl, token);
            std::lock_guard lock(mutex_);
            activeClient_ = http.get();
        }
        if (http) RefreshTags(*http);

        bool reachable = http != nullptr;
        Status counts;
        for (const auto& path : Session::ListMetaFiles(sessionsDir_)) {
            if (WaitForSingleObject(stopEvent_, 0) == WAIT_OBJECT_0) break;
            auto session = Session::Load(path);
            if (!session || session->status == "recording") continue;

            const std::string& state = session->uploadState;
            if (reachable && state == UploadState::kPending) {
                reachable = Upload(*http, *session, config);
            } else if (reachable && state == UploadState::kUploaded) {
                reachable = Check(*http, *session, config);
            }
            if (state == UploadState::kDone) ApplyRetention(*session, config);

            if (state == UploadState::kPending) ++counts.waiting;
            else if (state == UploadState::kUploaded) ++counts.processing;
            else if (state == UploadState::kRejected || state == UploadState::kFailed) ++counts.problems;
        }

        {
            std::lock_guard lock(mutex_);
            activeClient_ = nullptr;
        }
        if (reachable) {
            consecutiveFailures_ = 0;
            SetBlocker({});
        }
        {
            std::lock_guard lock(mutex_);
            counts.blocker = status_.blocker;
            status_ = counts;
        }
        PostMessageW(notifyWindow_, kMessage, 0, 0);

        if (http && !reachable) {
            wait = std::min<DWORD>(kFirstRetryMs << std::min(consecutiveFailures_, 4), kMaxRetryMs);
        } else if (counts.processing > 0) {
            wait = kProcessingWaitMs;
        } else {
            wait = kIdleWaitMs;
        }
    }
}

bool Uploader::IsGlobalFailure(const HttpResponse& response) {
    if (response.status == 0) {
        ++consecutiveFailures_;
        SetBlocker(L"Can't reach Speakr: " + response.Describe());
        return true;
    }
    if (response.status == 401 || response.status == 403) {
        SetBlocker(L"Speakr rejected the API token. Enter a new one in Settings from the tray menu.");
        return true;
    }
    if (response.status == 429 || response.status >= 500) {
        ++consecutiveFailures_;
        SetBlocker(L"Speakr is busy or failing: " + response.Describe());
        return true;
    }
    return false;
}

void Uploader::Reject(Session& session, const std::wstring& reason) {
    session.uploadState = UploadState::kRejected;
    session.uploadError = reason;
    session.Save();
    Notify({L"Upload refused", session.title + L": " + reason, {}, true});
}

HttpResponse Uploader::RefreshTags(HttpClient& http) {
    HttpResponse list = http.Get(L"/api/v1/tags");
    if (!list.Ok()) return list;
    auto parsed = json::parse(list.body, nullptr, false);
    if (!parsed.is_object() || !parsed.contains("tags") || !parsed["tags"].is_array()) return list;
    std::vector<std::wstring> names;
    for (const auto& tag : parsed["tags"]) {
        if (tag.contains("name") && tag["name"].is_string() && tag.contains("id") && tag["id"].is_number_integer()) {
            std::wstring name = FromUtf8(tag["name"].get<std::string>());
            tagIds_[name] = tag["id"].get<int>();
            names.push_back(std::move(name));
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

bool Uploader::Upload(HttpClient& http, Session& session, const Config& config) {
    if (GetFileAttributesW(session.audioPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        Reject(session, L"The audio file is missing.");
        return true;
    }

    std::vector<int> tagIds;
    if (!ResolveTags(http, session.tags.value_or(config.tags), tagIds)) return false;

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
        if (IsGlobalFailure(response)) return false;
        Reject(session, response.Describe());
        return true;
    }
    int id = JsonId(response.body);
    if (!id) {
        Reject(session, L"Speakr accepted the upload but didn't return a recording id.");
        return true;
    }

    session.speakrId = id;
    session.uploadState = UploadState::kUploaded;
    session.uploadError.clear();
    session.uploadedUtc = NowIsoUtc();
    session.Save();

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

    if (response.status == 404) {  // deleted in Speakr; nothing left to wait for
        session.uploadState = UploadState::kDone;
        session.uploadError = L"Deleted in Speakr";
        session.finishedUtc = NowIsoUtc();
        session.Save();
        return true;
    }
    if (!response.Ok()) return !IsGlobalFailure(response);

    auto parsed = json::parse(response.body, nullptr, false);
    std::string status = parsed.is_object() ? parsed.value("status", "") : "";
    if (status == "COMPLETED") {
        session.uploadState = UploadState::kDone;
        session.finishedUtc = NowIsoUtc();
        session.Save();
        Notify({L"Notes ready", session.title + L"\nClick to open in Speakr.", url});
    } else if (status == "FAILED") {
        std::string message;
        if (parsed.contains("error_message") && parsed["error_message"].is_string()) {
            message = parsed["error_message"].get<std::string>();
        }
        session.uploadState = UploadState::kFailed;
        session.uploadError = message.empty() ? L"Speakr couldn't process the recording" : FromUtf8(message);
        session.finishedUtc = NowIsoUtc();
        session.Save();
        Notify({L"Transcription failed", session.title + L": " + session.uploadError, url, true});
    }
    return true;
}

void Uploader::ApplyRetention(Session& session, const Config& config) {
    if (config.keepAudioDays <= 0 || session.audioDeleted) return;
    double age = DaysSinceIsoUtc(session.finishedUtc);
    if (age < config.keepAudioDays) return;
    if (DeleteFileW(session.audioPath.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
        session.audioDeleted = true;
        session.Save();
    }
}
