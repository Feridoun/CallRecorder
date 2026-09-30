#pragma once

#include <windows.h>

#include <mutex>
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
inline constexpr char kHeld[] = "held";             // made for another (or no) Speakr server; waits for the user
inline constexpr char kMissing[] = "missing";       // Speakr no longer has it (HTTP 404); local audio is kept
inline constexpr char kStuck[] = "stuck";           // Speakr hadn't finished it a day after upload; no longer polled
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

    // The Speakr server this recording belongs to (normalised URL). nullopt in
    // sidecars written before 1.2, which belong to whichever server is
    // configured. Empty: recorded before Speakr was set up. A session is only
    // ever uploaded to, or checked against, its own server; one bound to a
    // different server (or to none) is held until the user adopts it.
    std::optional<std::wstring> serverUrl;

    std::string uploadState = UploadState::kPending;
    std::wstring uploadError;
    int speakrId = 0;
    std::wstring uploadedUtc;
    std::wstring finishedUtc;  // when Speakr reported done/failed
    // Set (and saved) just before each upload request, cleared when Speakr
    // gives a definite answer. Non-empty on a pending session means an earlier
    // attempt may have reached Speakr, so check before uploading again.
    std::wstring uploadAttemptUtc;
    // Don't upload before this time: the grace period after stopping, during
    // which "Keep last recording on this PC" can still stop the upload.
    std::wstring uploadAfterUtc;
    bool audioDeleted = false;

    // serverUrl: the configured Speakr server (empty if not set up yet).
    static Session Create(const std::wstring& directory, bool sensitive, std::vector<std::wstring> tags,
                          const std::wstring& serverUrl);
    static std::optional<Session> Load(const std::wstring& metaPath);

    bool Save() const;
    void Finish(double recordedSeconds);
    void SetSensitive(bool value);
    void Discard() const;  // deletes both files

    // Marks sidecars left in "recording" state by a crash as "interrupted".
    // Their audio is still playable up to the point of the crash.
    static int RecoverInterrupted(const std::wstring& directory);
    static std::vector<std::wstring> ListMetaFiles(const std::wstring& directory);

    // Held by anyone doing load-modify-save on a sidecar outside the recording
    // itself (the uploader, the tray menu, the recordings window), so that
    // e.g. "keep on this PC" and the start of an upload can't interleave.
    static std::mutex& FileMutex();
};
