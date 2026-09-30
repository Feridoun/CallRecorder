#include "Autostart.h"

#include <windows.h>

#include <string>

namespace {

constexpr wchar_t kRunKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Run";
// Task Manager's on/off switch for Run entries: first byte even = enabled,
// odd = disabled; no value = enabled.
constexpr wchar_t kApprovedKey[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Explorer\\StartupApproved\\Run";
constexpr wchar_t kValueName[] = L"CallRecorder";

std::wstring ThisExe() {
    std::wstring path(MAX_PATH, L'\0');
    for (;;) {
        DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
        if (length < path.size()) {
            path.resize(length);
            return path;
        }
        path.resize(path.size() * 2);
    }
}

std::wstring RegisteredCommand() {
    DWORD bytes = 0;
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, nullptr, &bytes) != ERROR_SUCCESS) {
        return {};
    }
    std::wstring value(bytes / sizeof(wchar_t), L'\0');
    if (RegGetValueW(HKEY_CURRENT_USER, kRunKey, kValueName, RRF_RT_REG_SZ, nullptr, value.data(), &bytes) != ERROR_SUCCESS) {
        return {};
    }
    value.resize(wcsnlen(value.c_str(), value.size()));
    return value;
}

std::wstring Unquote(const std::wstring& command) {
    if (command.size() >= 2 && command.front() == L'"') {
        size_t end = command.find(L'"', 1);
        if (end != std::wstring::npos) return command.substr(1, end - 1);
    }
    return command;
}

bool DisabledInTaskManager() {
    BYTE data[12] = {};
    DWORD bytes = sizeof data;
    if (RegGetValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName, RRF_RT_REG_BINARY, nullptr, data, &bytes) != ERROR_SUCCESS) {
        return false;
    }
    return bytes > 0 && (data[0] & 1) != 0;
}

bool Register(const std::wstring& exe) {
    std::wstring command = L"\"" + exe + L"\"";
    return RegSetKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName, REG_SZ, command.c_str(),
                           static_cast<DWORD>((command.size() + 1) * sizeof(wchar_t))) == ERROR_SUCCESS;
}

}  // namespace

namespace Autostart {

bool IsEnabled() {
    return !RegisteredCommand().empty() && !DisabledInTaskManager();
}

bool SetEnabled(bool enabled) {
    // Clear Task Manager's flag either way: on enable so the entry actually
    // runs, on disable so no stale flag is left behind.
    RegDeleteKeyValueW(HKEY_CURRENT_USER, kApprovedKey, kValueName);
    if (enabled) return Register(ThisExe());
    LSTATUS status = RegDeleteKeyValueW(HKEY_CURRENT_USER, kRunKey, kValueName);
    return status == ERROR_SUCCESS || status == ERROR_FILE_NOT_FOUND;
}

void RepairPath() {
    std::wstring registered = Unquote(RegisteredCommand());
    if (registered.empty() || GetFileAttributesW(registered.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    Register(ThisExe());
}

}  // namespace Autostart
