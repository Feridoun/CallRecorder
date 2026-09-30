#include "Config.h"

#include "Util.h"

#include <windows.h>
#include <shlobj.h>
#include <wincred.h>

#include <nlohmann/json.hpp>

#include <fstream>

using nlohmann::json;

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

Config Config::Load() {
    Config config;
    std::wstring path = Path();
    if (path.empty()) return config;

    std::ifstream in(path, std::ios::binary);
    if (!in) {
        json tags = json::array();
        for (const auto& tag : config.tags) tags.push_back(ToUtf8(tag));
        json defaults = {
            {"server_url", ToUtf8(config.serverUrl)},
            {"tags", tags},
            {"hotwords", ""},
            {"keep_audio_days", config.keepAudioDays},
            {"sensitive_by_default", config.sensitiveByDefault},
        };
        std::ofstream(path, std::ios::binary) << defaults.dump(2) << "\n";
        return config;
    }

    json j = json::parse(in, nullptr, /*allow_exceptions=*/false);
    if (j.is_discarded() || !j.is_object()) return config;  // keep defaults rather than fail

    config.serverUrl = NormalizeServerUrl(FromUtf8(j.value("server_url", ToUtf8(config.serverUrl))));
    if (j.contains("tags") && j["tags"].is_array()) {
        config.tags.clear();
        for (const auto& tag : j["tags"]) {
            if (tag.is_string() && !tag.get<std::string>().empty()) config.tags.push_back(FromUtf8(tag.get<std::string>()));
        }
    }
    config.hotwords = FromUtf8(j.value("hotwords", ""));
    config.keepAudioDays = j.value("keep_audio_days", config.keepAudioDays);
    config.sensitiveByDefault = j.value("sensitive_by_default", config.sensitiveByDefault);
    return config;
}

bool Config::SaveServerUrl(const std::wstring& url) {
    Load();  // make sure the file exists
    std::wstring path = Path();
    if (path.empty()) return false;
    // ordered_json keeps the user's key order and any keys we don't know.
    nlohmann::ordered_json j;
    {
        std::ifstream in(path, std::ios::binary);
        j = nlohmann::ordered_json::parse(in, nullptr, /*allow_exceptions=*/false);
    }
    if (j.is_discarded() || !j.is_object()) j = nlohmann::ordered_json::object();
    j["server_url"] = ToUtf8(url);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    return static_cast<bool>(out << j.dump(2) << "\n");
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
