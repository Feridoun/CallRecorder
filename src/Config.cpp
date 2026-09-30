#include "Config.h"

#include "Json.h"
#include "Util.h"

#include <windows.h>
#include <shlobj.h>
#include <wincred.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>

using nlohmann::json;
using nlohmann::ordered_json;

namespace {

bool IsSpace(wchar_t c) {
    return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n';
}

// Reads a whole number that may be out of range, or written as 14.0 or "14"
// by hand, without ever overflowing.
int GetClampedInt(const json& j, const char* key, int fallback, int low, int high) {
    double value = JsonGet<double>(j, key, fallback);
    if (std::isnan(value)) return fallback;
    return static_cast<int>(std::clamp(value, static_cast<double>(low), static_cast<double>(high)));
}

constexpr const char* kCallDetectionNames[] = {"off", "ask", "auto"};  // indexed by CallDetection

// Fills `j` with every setting, in the order the defaults file uses. For an
// existing file this overwrites values in place, so the user's order and any
// unknown keys stay as they are.
template <typename Json>
void StoreSettings(const Config& config, Json& j) {
    j["server_url"] = ToUtf8(config.serverUrl);
    Json tags = Json::array();
    for (const auto& tag : config.tags) tags.push_back(ToUtf8(tag));
    j["tags"] = tags;
    j["hotwords"] = ToUtf8(config.hotwords);
    j["keep_audio_days"] = config.keepAudioDays;
    j["sensitive_by_default"] = config.sensitiveByDefault;
    j["microphone"] = ToUtf8(config.microphone);
    j["speakers"] = ToUtf8(config.speakers);
    j["upload_delay_seconds"] = config.uploadDelaySeconds;
    j["separate_channels"] = config.separateChannels;
    j["echo_cancellation"] = config.echoCancellation;
    Json apps = Json::array();
    for (const auto& app : config.loopbackApps) apps.push_back(ToUtf8(app));
    j["loopback_apps"] = apps;
    j["call_detection"] = kCallDetectionNames[static_cast<int>(config.callDetection)];
}

}  // namespace

std::vector<std::wstring> NormalizeAppList(const std::vector<std::wstring>& names) {
    std::vector<std::wstring> result;
    for (std::wstring name : names) {
        while (!name.empty() && IsSpace(name.back())) name.pop_back();
        size_t start = 0;
        while (start < name.size() && IsSpace(name[start])) ++start;
        name.erase(0, start);
        if (name.empty()) continue;
        bool seen = std::any_of(result.begin(), result.end(), [&](const std::wstring& other) {
            return CompareStringOrdinal(other.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL;
        });
        if (!seen) result.push_back(std::move(name));
    }
    return result;
}

std::wstring Config::Path() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\CallRecorder";
    }
    CoTaskMemFree(base);
    if (dir.empty()) return {};
    SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir + L"\\config.json";
}

Config Config::Parse(const std::string& text) {
    Config config;
    json j = json::parse(text, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) {
        // Defaults would turn local-only mode off, so say so and let the app
        // decide, rather than quietly failing open.
        config.unreadable = true;
        return config;
    }

    config.serverUrl = NormalizeServerUrl(FromUtf8(JsonGet(j, "server_url", "")));
    auto tags = j.find("tags");
    if (tags != j.end() && tags->is_array()) {
        config.tags.clear();
        for (const auto& tag : *tags) {
            if (tag.is_string() && !tag.get<std::string>().empty()) config.tags.push_back(FromUtf8(tag.get<std::string>()));
        }
    }
    config.hotwords = FromUtf8(JsonGet(j, "hotwords", ""));
    config.keepAudioDays = GetClampedInt(j, "keep_audio_days", config.keepAudioDays, 0, 36500);
    config.sensitiveByDefault = JsonGet(j, "sensitive_by_default", config.sensitiveByDefault);
    config.microphone = FromUtf8(JsonGet(j, "microphone", ""));
    config.speakers = FromUtf8(JsonGet(j, "speakers", ""));
    config.uploadDelaySeconds = GetClampedInt(j, "upload_delay_seconds", config.uploadDelaySeconds, 0, 3600);
    config.separateChannels = JsonGet(j, "separate_channels", config.separateChannels);
    config.echoCancellation = JsonGet(j, "echo_cancellation", config.echoCancellation);
    auto apps = j.find("loopback_apps");
    if (apps != j.end() && apps->is_array()) {
        std::vector<std::wstring> names;
        for (const auto& app : *apps) {
            if (app.is_string()) names.push_back(FromUtf8(app.get<std::string>()));
        }
        config.loopbackApps = NormalizeAppList(names);
    }
    // Anything unrecognised keeps the default rather than turning detection off.
    std::string detection = JsonGet(j, "call_detection", std::string());
    for (int i = 0; i < static_cast<int>(std::size(kCallDetectionNames)); ++i) {
        if (_stricmp(detection.c_str(), kCallDetectionNames[i]) == 0) {
            config.callDetection = static_cast<CallDetection>(i);
        }
    }
    return config;
}

Config Config::Load() {
    Config config;
    std::wstring path = Path();
    if (path.empty()) return config;

    auto text = ReadFileText(path);
    if (!text) {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
            config.unreadable = true;  // there, but locked or damaged: don't overwrite it
            return config;
        }
        ordered_json defaults;
        StoreSettings(config, defaults);
        WriteFileAtomically(path, defaults.dump(2) + "\n");
        return config;
    }
    return Parse(*text);
}

bool Config::Save() const {
    std::wstring path = Path();
    if (path.empty()) return false;
    // ordered_json keeps the user's key order and any keys we don't know.
    ordered_json j = ordered_json::object();
    auto text = ReadFileText(path);
    if (text) {
        ordered_json existing = ordered_json::parse(*text, nullptr, /*allow_exceptions=*/false);
        if (!existing.is_discarded() && existing.is_object()) {
            j = std::move(existing);
        } else if (!CopyFileW(path.c_str(), (path + L".bad").c_str(), FALSE)) {
            return false;  // don't destroy the user's hand edits without a copy
        }
    } else {
        DWORD error = GetLastError();
        if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) return false;
    }
    StoreSettings(*this, j);
    return WriteFileAtomically(path, j.dump(2) + "\n");
}

std::wstring NormalizeServerUrl(std::wstring url) {
    auto isSpace = [](wchar_t c) { return c == L' ' || c == L'\t' || c == L'\r' || c == L'\n'; };
    while (!url.empty() && isSpace(url.back())) url.pop_back();
    size_t start = 0;
    while (start < url.size() && isSpace(url[start])) ++start;
    url.erase(0, start);
    if (url.empty()) return url;
    if (url.find(L"://") == std::wstring::npos) url = L"https://" + url;
    while (!url.empty() && url.back() == L'/') url.pop_back();
    if (url.ends_with(L"/api/v1")) url.resize(url.size() - 7);
    while (!url.empty() && url.back() == L'/') url.pop_back();
    return url;
}

namespace {
constexpr wchar_t kCredentialName[] = L"CallRecorder:Speakr";
}

bool WriteSpeakrToken(const std::string& token) {
    std::wstring secret = FromUtf8(token);
    CREDENTIALW credential{};
    credential.Type = CRED_TYPE_GENERIC;
    credential.TargetName = const_cast<wchar_t*>(kCredentialName);
    credential.Comment = const_cast<wchar_t*>(L"Speakr API token for CallRecorder uploads");
    credential.UserName = const_cast<wchar_t*>(L"CallRecorder");
    credential.Persist = CRED_PERSIST_LOCAL_MACHINE;
    credential.CredentialBlob = reinterpret_cast<BYTE*>(secret.data());
    credential.CredentialBlobSize = static_cast<DWORD>(secret.size() * sizeof(wchar_t));
    bool ok = CredWriteW(&credential, 0) != FALSE;
    SecureZeroMemory(secret.data(), secret.size() * sizeof(wchar_t));
    return ok;
}

std::string ReadSpeakrToken() {
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(kCredentialName, CRED_TYPE_GENERIC, 0, &credential)) return {};
    std::wstring token(reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
                       credential->CredentialBlobSize / sizeof(wchar_t));
    CredFree(credential);
    return ToUtf8(token);
}
