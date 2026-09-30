#include "Util.h"

#include <shlobj.h>

#include <algorithm>
#include <cstdio>

std::string ToUtf8(const std::wstring& text) {
    if (text.empty()) return {};
    int size = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()),
                                   nullptr, 0, nullptr, nullptr);
    std::string out(size, '\0');
    WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size,
                        nullptr, nullptr);
    return out;
}

std::wstring FromUtf8(const std::string& text) {
    if (text.empty()) return {};
    int size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(size, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

static SYSTEMTIME ToLocal(const SYSTEMTIME& utc) {
    SYSTEMTIME local{};
    SystemTimeToTzSpecificLocalTime(nullptr, &utc, &local);
    return local;
}

std::wstring FormatIsoUtc(const SYSTEMTIME& utc) {
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02uT%02u:%02u:%02uZ", utc.wYear, utc.wMonth, utc.wDay, utc.wHour,
               utc.wMinute, utc.wSecond);
    return buf;
}

std::wstring NowIsoUtc() {
    SYSTEMTIME now;
    GetSystemTime(&now);
    return FormatIsoUtc(now);
}

bool ParseIsoUtc(const std::wstring& text, SYSTEMTIME& utc) {
    utc = {};
    unsigned year, month, day, hour, minute, second;
    if (swscanf_s(text.c_str(), L"%4u-%2u-%2uT%2u:%2u:%2u", &year, &month, &day, &hour, &minute, &second) != 6) {
        return false;
    }
    if (month < 1 || month > 12 || day < 1 || day > 31 || hour > 23 || minute > 59 || second > 59) {
        utc = {};  // hand-edited or damaged; callers treat a zero year as "unset"
        return false;
    }
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);
    return true;
}

double DaysSinceIsoUtc(const std::wstring& text) {
    SYSTEMTIME then, now;
    FILETIME thenFile, nowFile;
    if (!ParseIsoUtc(text, then) || !SystemTimeToFileTime(&then, &thenFile)) return -1;
    GetSystemTime(&now);
    SystemTimeToFileTime(&now, &nowFile);
    ULARGE_INTEGER a{{thenFile.dwLowDateTime, thenFile.dwHighDateTime}};
    ULARGE_INTEGER b{{nowFile.dwLowDateTime, nowFile.dwHighDateTime}};
    return (static_cast<double>(b.QuadPart) - static_cast<double>(a.QuadPart)) / (10'000'000.0 * 86400);
}

std::wstring FormatFriendlyLocal(const SYSTEMTIME& utc) {
    static const wchar_t* kMonths[] = {L"Jan", L"Feb", L"Mar", L"Apr", L"May", L"Jun",
                                       L"Jul", L"Aug", L"Sep", L"Oct", L"Nov", L"Dec"};
    SYSTEMTIME t = ToLocal(utc);
    wchar_t buf[32];
    swprintf_s(buf, L"%u %s %04u %02u:%02u", t.wDay, kMonths[(t.wMonth + 11) % 12], t.wYear, t.wHour,
               t.wMinute);
    return buf;
}

std::wstring FormatFileStamp(const SYSTEMTIME& utc) {
    SYSTEMTIME t = ToLocal(utc);
    wchar_t buf[32];
    swprintf_s(buf, L"%04u-%02u-%02u_%02u%02u%02u", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute,
               t.wSecond);
    return buf;
}

std::wstring FormatDuration(double seconds) {
    auto total = static_cast<long long>(seconds);
    wchar_t buf[32];
    if (total >= 3600) {
        swprintf_s(buf, L"%lld:%02lld:%02lld", total / 3600, (total / 60) % 60, total % 60);
    } else {
        swprintf_s(buf, L"%lld:%02lld", total / 60, total % 60);
    }
    return buf;
}

std::wstring SessionsDirectory() {
    PWSTR base = nullptr;
    std::wstring dir;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &base))) {
        dir = std::wstring(base) + L"\\CallRecorder\\sessions";
    }
    CoTaskMemFree(base);
    if (!dir.empty()) SHCreateDirectoryExW(nullptr, dir.c_str(), nullptr);
    return dir;
}

bool IsPlainFileName(const std::wstring& name) {
    if (name.empty() || name.find(L"..") != std::wstring::npos) return false;
    return std::none_of(name.begin(), name.end(), [](wchar_t c) { return c < 32 || c == L'\\' || c == L'/' || c == L':'; });
}

bool WriteFileAtomically(const std::wstring& path, const std::string& contents) {
    std::wstring temp = path + L".tmp";
    HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    size_t offset = 0;
    while (ok && offset < contents.size()) {
        DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - offset, 1u << 20));
        DWORD written = 0;
        ok = WriteFile(file, contents.data() + offset, chunk, &written, nullptr) != 0 && written > 0;
        offset += written;
    }
    // Without the flush the rename can reach the disk before the data does,
    // leaving an empty file after a power cut.
    ok = ok && FlushFileBuffers(file) != 0;
    CloseHandle(file);
    ok = ok && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

std::optional<std::string> ReadFileText(const std::wstring& path) {
    HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string text;
    char buffer[16384];
    bool ok = true;
    for (;;) {
        DWORD got = 0;
        if (!ReadFile(file, buffer, sizeof(buffer), &got, nullptr)) {
            ok = false;
            break;
        }
        if (got == 0) break;
        text.append(buffer, got);
    }
    DWORD error = GetLastError();
    CloseHandle(file);
    if (!ok) {
        SetLastError(error);  // callers tell "missing" from "unreadable" by this
        return std::nullopt;
    }
    if (text.starts_with("\xEF\xBB\xBF")) text.erase(0, 3);
    return text;
}
