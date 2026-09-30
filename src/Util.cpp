#include "Util.h"

#include <shlobj.h>

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
