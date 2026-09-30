#pragma once

#include <windows.h>

#include <optional>
#include <string>
#include <vector>

// Where a session is in its journey to Speakr.
namespace UploadState {
inline constexpr char kLocalOnly[] = "local_only";  // sensitive: never uploaded
inline constexpr char kPending[] = "pending";       // waiting to upload
inline constexpr char kUploaded[] = "uploaded";     // Speakr has it, still transcribing/summarising
inline constexpr char kDone[] = "done";             // transcript + summary ready
inline constexpr char kFailed[] = "failed";         // Speakr couldn't process it
inline constexpr char kRejected[] = "rejected";     // Speakr refused the upload; see upload_error
}  // namespace UploadState

// One recording: <stamp>.opus plus a <stamp>.json sidecar describing it. The
// recorder owns the sidecar while status is "recording"; after that only the
// uploader changes it. Writes are atomic (temp file + rename).
struct Session {
    struct Marker {
        double offsetSeconds;
        std::wstring label;
    };

    std::wstring id;  // file stamp, e.g. 2026-09-30_140215
    std::wstring audioPath;
    std::wstring metaPath;
    std::wstring title;
    SYSTEMTIME startedUtc{};
    SYSTEMTIME endedUtc{};
    double durationSeconds = 0;
    bool sensitive = false;
    std::string status;  // recording | recorded | interrupted
    std::vector<Marker> markers;
    // Speakr tag names for this recording. Unset in sidecars written before
    // per-session tags existed; those get the config's default tags.
    std::optional<std::vector<std::wstring>> tags;

    std::string uploadState = UploadState::kPending;
    std::wstring uploadError;
    int speakrId = 0;
    std::wstring uploadedUtc;
    std::wstring finishedUtc;  // when Speakr reported done/failed
    bool audioDeleted = false;

    static Session Create(const std::wstring& directory, bool sensitive, std::vector<std::wstring> tags);
    static std::optional<Session> Load(const std::wstring& metaPath);

    bool Save() const;
    void Finish(double recordedSeconds);
    void SetSensitive(bool value);
    void Discard() const;  // deletes both files

    // Marks sidecars left in "recording" state by a crash as "interrupted".
    // Their audio is still playable up to the point of the crash.
    static int RecoverInterrupted(const std::wstring& directory);
    static std::vector<std::wstring> ListMetaFiles(const std::wstring& directory);
};
