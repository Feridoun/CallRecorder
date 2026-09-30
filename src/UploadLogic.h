#pragma once

#include "Config.h"
#include "Json.h"
#include "Util.h"

#include <windows.h>

#include <cstdint>
#include <optional>
#include <string>

// The uploader's decisions as small pure functions, so they can be unit-tested
// without a network or a Speakr server.
namespace UploadLogic {

// After this long in "uploaded", a recording Speakr still hasn't finished is
// given up on (state "stuck") instead of being polled forever.
inline constexpr double kStuckAfterSeconds = 24 * 60 * 60;

// Same Speakr server? Compared after NormalizeServerUrl, ignoring case. An
// empty address (recorded before Speakr was set up) is never the same.
inline bool SameServer(const std::wstring& a, const std::wstring& b) {
    std::wstring x = NormalizeServerUrl(a);
    std::wstring y = NormalizeServerUrl(b);
    if (x.empty() || y.empty()) return false;
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()),
                                TRUE) == CSTR_EQUAL;
}

// An ISO 8601 timestamp as whole seconds since 1601 (the FILETIME epoch), UTC.
// Loose about the format: "T" or space, optional fraction, and "Z", "+02:00",
// "+0200" or nothing (Speakr returns naive UTC) as the zone.
inline std::optional<int64_t> ParseInstant(const std::wstring& text) {
    size_t i = 0;
    auto number = [&](size_t digits, unsigned& out) {
        if (i + digits > text.size()) return false;
        out = 0;
        for (size_t k = 0; k < digits; ++k) {
            wchar_t c = text[i + k];
            if (c < L'0' || c > L'9') return false;
            out = out * 10 + static_cast<unsigned>(c - L'0');
        }
        i += digits;
        return true;
    };
    auto literal = [&](wchar_t c) {
        if (i < text.size() && text[i] == c) {
            ++i;
            return true;
        }
        return false;
    };

    unsigned year, month, day, hour, minute, second = 0;
    if (!number(4, year) || !literal(L'-') || !number(2, month) || !literal(L'-') || !number(2, day)) return {};
    if (!literal(L'T') && !literal(L' ')) return {};
    if (!number(2, hour) || !literal(L':') || !number(2, minute)) return {};
    if (literal(L':')) {
        if (!number(2, second)) return {};
        if (literal(L'.') || literal(L',')) {
            while (i < text.size() && text[i] >= L'0' && text[i] <= L'9') ++i;
        }
    }
    int64_t offsetMinutes = 0;
    if (literal(L'Z') || literal(L'z')) {
    } else if (i < text.size() && (text[i] == L'+' || text[i] == L'-')) {
        int sign = text[i] == L'-' ? -1 : 1;
        ++i;
        unsigned offsetHour, offsetMinute = 0;
        if (!number(2, offsetHour)) return {};
        literal(L':');
        if (i < text.size() && !number(2, offsetMinute)) return {};
        offsetMinutes = sign * static_cast<int64_t>(offsetHour * 60 + offsetMinute);
    }
    if (i != text.size()) return {};

    SYSTEMTIME utc{};
    utc.wYear = static_cast<WORD>(year);
    utc.wMonth = static_cast<WORD>(month);
    utc.wDay = static_cast<WORD>(day);
    utc.wHour = static_cast<WORD>(hour);
    utc.wMinute = static_cast<WORD>(minute);
    utc.wSecond = static_cast<WORD>(second);
    FILETIME file;
    if (!SystemTimeToFileTime(&utc, &file)) return {};
    ULARGE_INTEGER ticks{{file.dwLowDateTime, file.dwHighDateTime}};
    return static_cast<int64_t>(ticks.QuadPart / 10'000'000) - offsetMinutes * 60;
}

inline bool SameInstant(const std::wstring& a, const std::wstring& b) {
    auto x = ParseInstant(a);
    auto y = ParseInstant(b);
    return x && y && *x == *y;
}

inline int64_t NowSeconds() {
    FILETIME file;
    GetSystemTimeAsFileTime(&file);
    ULARGE_INTEGER ticks{{file.dwLowDateTime, file.dwHighDateTime}};
    return static_cast<int64_t>(ticks.QuadPart / 10'000'000);
}

// Seconds from now until the timestamp: negative once it has passed. Empty if
// the text is empty or can't be parsed.
inline std::optional<int64_t> SecondsUntil(const std::wstring& isoUtc, int64_t now = NowSeconds()) {
    auto instant = ParseInstant(isoUtc);
    if (!instant) return {};
    return *instant - now;
}

// True while the grace period hasn't ended. A missing or unreadable time
// means no grace period.
inline bool IsInFuture(const std::wstring& isoUtc, int64_t now = NowSeconds()) {
    auto left = SecondsUntil(isoUtc, now);
    return left && *left > 0;
}

// Uploaded long enough ago that Speakr should have finished. Sessions with no
// (or an unreadable) upload time never count.
inline bool IsOverdue(const std::wstring& uploadedUtc, int64_t now = NowSeconds()) {
    auto left = SecondsUntil(uploadedUtc, now);
    return left && -static_cast<double>(*left) > kStuckAfterSeconds;
}

enum class SpeakrStatus { InProgress, Completed, Failed };

struct StatusReport {
    SpeakrStatus status = SpeakrStatus::InProgress;
    std::string errorMessage;  // Failed only; may be empty
};

// Reads GET /api/v1/recordings/<id>/status. Anything unexpected (not JSON,
// null or non-string status, an unknown status) is "still in progress".
template <typename Json>
StatusReport ClassifyStatus(const Json& body) {
    StatusReport report;
    std::string status = JsonGet(body, "status", "");
    if (status == "COMPLETED") {
        report.status = SpeakrStatus::Completed;
    } else if (status == "FAILED") {
        report.status = SpeakrStatus::Failed;
        report.errorMessage = JsonGet(body, "error_message", "");
    }
    return report;
}

// Finds our earlier upload in a GET /api/v1/recordings page: same title and
// same meeting_date instant. Returns the Speakr id, or 0.
template <typename Json>
int FindUploadedMatch(const Json& page, const std::string& title, const std::wstring& meetingDate) {
    if (!page.is_object()) return 0;
    auto list = page.find("recordings");
    if (list == page.end() || !list->is_array()) return 0;
    for (const auto& recording : *list) {
        if (!recording.is_object() || JsonGet(recording, "title", "\x01") != title) continue;
        std::string date = JsonGet(recording, "meeting_date", "");
        int id = JsonGet(recording, "id", 0);
        if (id > 0 && !date.empty() && SameInstant(FromUtf8(date), meetingDate)) return id;
    }
    return 0;
}

template <typename Json>
bool ListHasNext(const Json& page) {
    if (!page.is_object()) return false;
    auto pagination = page.find("pagination");
    return pagination != page.end() && JsonGet(*pagination, "has_next", false);
}

}  // namespace UploadLogic
