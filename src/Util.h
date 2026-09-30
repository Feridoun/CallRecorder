#pragma once

#include <windows.h>

#include <optional>
#include <string>

std::string ToUtf8(const std::wstring& text);
std::wstring FromUtf8(const std::string& text);

// "2026-09-30T13:02:15Z"
std::wstring FormatIsoUtc(const SYSTEMTIME& utc);
std::wstring NowIsoUtc();
bool ParseIsoUtc(const std::wstring& text, SYSTEMTIME& utc);
// Days elapsed since an ISO UTC timestamp; negative if it can't be parsed.
double DaysSinceIsoUtc(const std::wstring& text);
// "30 Sep 2026 14:02" in local time.
std::wstring FormatFriendlyLocal(const SYSTEMTIME& utc);
// "2026-09-30_140215" in local time; safe as a file name and sorts by date.
std::wstring FormatFileStamp(const SYSTEMTIME& utc);
// "1:02:03" or "2:03".
std::wstring FormatDuration(double seconds);

// %LOCALAPPDATA%\CallRecorder\sessions, created if missing. Deliberately not
// under Documents, which is often redirected to OneDrive.
std::wstring SessionsDirectory();

// True for a bare file name: non-empty, no path separators, drive colon, ".."
// or control characters. Guards names read from files we didn't just write.
bool IsPlainFileName(const std::wstring& name);

// Replaces `path` with `contents` so a crash or power cut leaves either the
// old file or the new one, never a partial one: writes a temp file next to it,
// flushes it to disk, then renames it over the target.
bool WriteFileAtomically(const std::wstring& path, const std::string& contents);

// The whole file, minus a UTF-8 byte order mark; nullopt if it can't be read.
std::optional<std::string> ReadFileText(const std::wstring& path);
