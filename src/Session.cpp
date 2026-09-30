#include "Session.h"

#include "Json.h"
#include "Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <climits>
#include <cmath>

using nlohmann::json;

namespace {

std::optional<json> ReadJson(const std::wstring& path) {
    auto text = ReadFileText(path);
    if (!text) return std::nullopt;
    json parsed = json::parse(*text, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    return parsed;
}

double Round1(double value) {
    return std::round(value * 10) / 10;
}

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// "C:\...\2026-09-30_140215.json" -> "2026-09-30_140215"
std::wstring StemOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L'\\');
    std::wstring name = slash == std::wstring::npos ? path : path.substr(slash + 1);
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

}  // namespace

Session Session::Create(const std::wstring& directory, bool sensitive, std::vector<std::wstring> tags,
                        const std::wstring& serverUrl) {
    Session session;
    session.serverUrl = serverUrl;
    session.tags = std::move(tags);
    GetSystemTime(&session.startedUtc);
    // Stamps are local time to the second, so a clock change or the repeated
    // hour when daylight saving ends can produce one that's taken. The audio
    // file is opened with "wb": never let it truncate an older recording.
    std::wstring stamp = FormatFileStamp(session.startedUtc);
    session.id = stamp;
    for (int n = 2; n < 1000 && (Exists(directory + L"\\" + session.id + L".opus") ||
                                 Exists(directory + L"\\" + session.id + L".json"));
         ++n) {
        session.id = stamp + L"_" + std::to_wstring(n);
    }
    session.audioPath = directory + L"\\" + session.id + L".opus";
    session.metaPath = directory + L"\\" + session.id + L".json";
    session.title = L"Recording " + FormatFriendlyLocal(session.startedUtc);
    session.status = "recording";
    session.SetSensitive(sensitive);
    return session;
}

std::optional<Session> Session::Load(const std::wstring& metaPath) {
    auto doc = ReadJson(metaPath);
    if (!doc) return std::nullopt;
    const json& j = *doc;

    Session s;
    s.metaPath = metaPath;
    s.id = FromUtf8(JsonGet(j, "id", ""));
    if (s.id.empty()) return std::nullopt;
    std::wstring directory = metaPath.substr(0, metaPath.find_last_of(L'\\'));
    // The uploader opens this path, so it must stay inside the sessions folder.
    std::wstring audioFile = FromUtf8(JsonGet(j, "audio_file", ""));
    if (!IsPlainFileName(audioFile)) audioFile = s.id + L".opus";
    if (!IsPlainFileName(audioFile)) audioFile = StemOf(metaPath) + L".opus";
    s.audioPath = directory + L"\\" + audioFile;
    s.title = FromUtf8(JsonGet(j, "title", ""));
    ParseIsoUtc(FromUtf8(JsonGet(j, "started_utc", "")), s.startedUtc);
    ParseIsoUtc(FromUtf8(JsonGet(j, "ended_utc", "")), s.endedUtc);
    s.durationSeconds = JsonGet(j, "duration_seconds", 0.0);
    s.sensitive = JsonGet(j, "sensitive", false);
    s.status = JsonGet(j, "status", "");
    auto markers = j.find("markers");
    if (markers != j.end() && markers->is_array()) {
        for (const auto& m : *markers) {
            if (!m.is_object()) continue;
            s.markers.push_back({JsonGet(m, "offset_seconds", 0.0), FromUtf8(JsonGet(m, "label", ""))});
        }
    }
    auto tags = j.find("tags");
    if (tags != j.end() && tags->is_array()) {
        s.tags.emplace();
        for (const auto& tag : *tags) {
            if (tag.is_string()) s.tags->push_back(FromUtf8(tag.get<std::string>()));
        }
    }
    // A server_url that isn't a string can't be trusted to name this
    // recording's server, so it counts as "none" and the recording is held.
    auto server = j.find("server_url");
    if (server != j.end()) s.serverUrl = server->is_string() ? FromUtf8(server->get<std::string>()) : std::wstring();
    s.uploadState = JsonGet(j, "upload_state", std::string(s.sensitive ? UploadState::kLocalOnly : UploadState::kPending));
    s.uploadError = FromUtf8(JsonGet(j, "upload_error", ""));
    double speakrId = JsonGet(j, "speakr_id", 0.0);
    s.speakrId = std::isnan(speakrId) ? 0 : static_cast<int>(std::clamp(speakrId, 0.0, static_cast<double>(INT_MAX)));
    s.uploadedUtc = FromUtf8(JsonGet(j, "uploaded_utc", ""));
    s.finishedUtc = FromUtf8(JsonGet(j, "finished_utc", ""));
    s.uploadAttemptUtc = FromUtf8(JsonGet(j, "upload_attempt_utc", ""));
    s.uploadAfterUtc = FromUtf8(JsonGet(j, "upload_after_utc", ""));
    s.audioDeleted = JsonGet(j, "audio_deleted", false);
    return s;
}

void Session::Finish(double recordedSeconds) {
    GetSystemTime(&endedUtc);
    durationSeconds = recordedSeconds;
    status = "recorded";
}

void Session::SetSensitive(bool value) {
    sensitive = value;
    uploadState = value ? UploadState::kLocalOnly : UploadState::kPending;
}

bool Session::Save() const {
    json markerList = json::array();
    for (const auto& m : markers) {
        markerList.push_back({{"offset_seconds", Round1(m.offsetSeconds)}, {"label", ToUtf8(m.label)}});
    }
    json j = {
        {"app", "CallRecorder"},
        {"format", 1},
        {"id", ToUtf8(id)},
        {"title", ToUtf8(title)},
        {"audio_file", ToUtf8(id) + ".opus"},
        {"status", status},
        {"sensitive", sensitive},
        {"started_utc", ToUtf8(FormatIsoUtc(startedUtc))},
        {"duration_seconds", Round1(durationSeconds)},
        {"markers", markerList},
        {"upload_state", uploadState},
    };
    if (tags) {
        json tagList = json::array();
        for (const auto& tag : *tags) tagList.push_back(ToUtf8(tag));
        j["tags"] = tagList;
    }
    if (serverUrl) j["server_url"] = ToUtf8(*serverUrl);
    if (endedUtc.wYear != 0) j["ended_utc"] = ToUtf8(FormatIsoUtc(endedUtc));
    if (!uploadError.empty()) j["upload_error"] = ToUtf8(uploadError);
    if (speakrId) j["speakr_id"] = speakrId;
    if (!uploadedUtc.empty()) j["uploaded_utc"] = ToUtf8(uploadedUtc);
    if (!finishedUtc.empty()) j["finished_utc"] = ToUtf8(finishedUtc);
    if (!uploadAttemptUtc.empty()) j["upload_attempt_utc"] = ToUtf8(uploadAttemptUtc);
    if (!uploadAfterUtc.empty()) j["upload_after_utc"] = ToUtf8(uploadAfterUtc);
    if (audioDeleted) j["audio_deleted"] = true;
    return WriteFileAtomically(metaPath, j.dump(2) + "\n");
}

std::mutex& Session::FileMutex() {
    static std::mutex mutex;
    return mutex;
}

void Session::Discard() const {
    DeleteFileW(audioPath.c_str());
    DeleteFileW(metaPath.c_str());
}

std::vector<std::wstring> Session::ListMetaFiles(const std::wstring& directory) {
    std::vector<std::wstring> paths;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((directory + L"\\*.json").c_str(), &found);
    if (search == INVALID_HANDLE_VALUE) return paths;
    do {
        // The search pattern can also match .json.bad and .json.tmp through
        // short file names; only real sidecars end in exactly ".json".
        std::wstring name = found.cFileName;
        if (name.size() > 5 && name.ends_with(L".json")) paths.push_back(directory + L"\\" + name);
    } while (FindNextFileW(search, &found));
    FindClose(search);
    std::sort(paths.begin(), paths.end());  // file stamps sort oldest first
    return paths;
}

int Session::RecoverInterrupted(const std::wstring& directory) {
    int recovered = 0;
    for (const auto& path : ListMetaFiles(directory)) {
        auto session = Load(path);
        if (!session || session->status != "recording") continue;
        session->status = "interrupted";
        // The sidecar's duration was never filled in; the Ogg file knows the
        // real length, but Speakr works it out from the audio anyway.
        if (session->Save()) ++recovered;
    }

    // A recording whose sidecar is missing or unreadable (a power cut before
    // the first write reached the disk) would otherwise be invisible forever.
    std::vector<std::wstring> stems;
    WIN32_FIND_DATAW found;
    HANDLE search = FindFirstFileW((directory + L"\\*.opus").c_str(), &found);
    if (search != INVALID_HANDLE_VALUE) {
        do {
            std::wstring name = found.cFileName;
            if (name.size() > 5 && name.ends_with(L".opus") && !(found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) {
                stems.push_back(name.substr(0, name.size() - 5));
            }
        } while (FindNextFileW(search, &found));
        FindClose(search);
    }
    for (const auto& stem : stems) {
        std::wstring metaPath = directory + L"\\" + stem + L".json";
        std::wstring audioPath = directory + L"\\" + stem + L".opus";
        if (Load(metaPath)) continue;
        if (Exists(metaPath) &&
            !MoveFileExW(metaPath.c_str(), (metaPath + L".bad").c_str(), MOVEFILE_REPLACE_EXISTING)) {
            continue;  // can't set the damaged file aside, so don't overwrite it
        }

        Session s;
        s.id = stem;
        s.audioPath = audioPath;
        s.metaPath = metaPath;
        WIN32_FILE_ATTRIBUTE_DATA info{};
        SYSTEMTIME created{};
        if (GetFileAttributesExW(audioPath.c_str(), GetFileExInfoStandard, &info) &&
            FileTimeToSystemTime(&info.ftCreationTime, &created)) {
            s.startedUtc = created;
        } else {
            GetSystemTime(&s.startedUtc);
        }
        s.title = L"Recording " + FormatFriendlyLocal(s.startedUtc);
        s.status = "interrupted";
        // Whether it was a sensitive recording is lost with the sidecar, so it
        // must not upload unless the user says so.
        s.SetSensitive(true);
        s.uploadError = L"Recovered after its details were lost. Kept on this PC; upload it from the Recordings window if you want it in Speakr.";
        if (s.Save()) ++recovered;
    }
    return recovered;
}
