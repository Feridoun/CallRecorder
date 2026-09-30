#include "Session.h"

#include "Util.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <fstream>
#include <iterator>

using nlohmann::json;

namespace {

bool WriteFileAtomically(const std::wstring& path, const std::string& contents) {
    std::wstring temp = path + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out.write(contents.data(), static_cast<std::streamsize>(contents.size()))) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

std::optional<json> ReadJson(const std::wstring& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) return std::nullopt;
    json parsed = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (parsed.is_discarded() || !parsed.is_object()) return std::nullopt;
    return parsed;
}

double Round1(double value) {
    return std::round(value * 10) / 10;
}

}  // namespace

Session Session::Create(const std::wstring& directory, bool sensitive, std::vector<std::wstring> tags) {
    Session session;
    session.tags = std::move(tags);
    GetSystemTime(&session.startedUtc);
    session.id = FormatFileStamp(session.startedUtc);
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
    s.id = FromUtf8(j.value("id", ""));
    if (s.id.empty()) return std::nullopt;
    std::wstring directory = metaPath.substr(0, metaPath.find_last_of(L'\\'));
    s.audioPath = directory + L"\\" + FromUtf8(j.value("audio_file", ToUtf8(s.id) + ".opus"));
    s.title = FromUtf8(j.value("title", ""));
    ParseIsoUtc(FromUtf8(j.value("started_utc", "")), s.startedUtc);
    ParseIsoUtc(FromUtf8(j.value("ended_utc", "")), s.endedUtc);
    s.durationSeconds = j.value("duration_seconds", 0.0);
    s.sensitive = j.value("sensitive", false);
    s.status = j.value("status", "");
    for (const auto& m : j.value("markers", json::array())) {
        s.markers.push_back({m.value("offset_seconds", 0.0), FromUtf8(m.value("label", ""))});
    }
    if (j.contains("tags") && j["tags"].is_array()) {
        s.tags.emplace();
        for (const auto& tag : j["tags"]) {
            if (tag.is_string()) s.tags->push_back(FromUtf8(tag.get<std::string>()));
        }
    }
    s.uploadState = j.value("upload_state", s.sensitive ? UploadState::kLocalOnly : UploadState::kPending);
    s.uploadError = FromUtf8(j.value("upload_error", ""));
    s.speakrId = j.value("speakr_id", 0);
    s.uploadedUtc = FromUtf8(j.value("uploaded_utc", ""));
    s.finishedUtc = FromUtf8(j.value("finished_utc", ""));
    s.audioDeleted = j.value("audio_deleted", false);
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
    if (endedUtc.wYear != 0) j["ended_utc"] = ToUtf8(FormatIsoUtc(endedUtc));
    if (!uploadError.empty()) j["upload_error"] = ToUtf8(uploadError);
    if (speakrId) j["speakr_id"] = speakrId;
    if (!uploadedUtc.empty()) j["uploaded_utc"] = ToUtf8(uploadedUtc);
    if (!finishedUtc.empty()) j["finished_utc"] = ToUtf8(finishedUtc);
    if (audioDeleted) j["audio_deleted"] = true;
    return WriteFileAtomically(metaPath, j.dump(2) + "\n");
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
    return recovered;
}
