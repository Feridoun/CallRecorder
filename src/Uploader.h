#pragma once

#include "Config.h"

#include <windows.h>

#include <cstdint>
#include <map>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

class HttpClient;
struct HttpResponse;
struct Session;

// Background worker that moves finished sessions to Speakr: uploads pending
// ones, waits for Speakr to transcribe and summarise them, then deletes the
// local audio after Config::keepAudioDays. Never touches local-only sessions
// or the session being recorded.
class Uploader {
public:
    static constexpr UINT kMessage = WM_APP + 3;  // posted when TakeEvents() has something

    struct Event {
        std::wstring title;
        std::wstring text;
        std::wstring url;  // opened if the notification is clicked
        bool problem = false;
    };

    struct Status {
        int waiting = 0;     // not uploaded yet
        int processing = 0;  // in Speakr, not finished
        int problems = 0;    // rejected, failed, missing in Speakr or stuck
        int held = 0;        // made for another (or no) Speakr server; waiting for the user to adopt them
        std::wstring blocker;  // why uploads are stalled, if they are
    };

    Uploader(HWND notifyWindow, std::wstring sessionsDir);
    ~Uploader();
    Uploader(const Uploader&) = delete;
    Uploader& operator=(const Uploader&) = delete;

    void Wake();  // check the queue now
    std::vector<Event> TakeEvents();
    Status GetStatus();
    std::wstring ServerUrl();
    // Tag names in Speakr as of the last check, for the Tags menu.
    std::vector<std::wstring> KnownTags();

private:
    // What one pass over the sessions found.
    struct Cycle {
        bool reachable = true;   // false once a global failure ends the pass
        Status counts;
        int newlyHeld = 0;       // sessions held during this pass
        int fastPolling = 0;     // uploaded sessions worth checking every few seconds
        std::optional<int64_t> nextDueSeconds;  // soonest end of a grace period
    };

    void Run();
    // One pass; returns how long to wait before the next.
    DWORD RunCycle();
    void ProcessSession(HttpClient* http, Session& session, const Config& config, Cycle& cycle);
    // Looks in Speakr for a recording an interrupted upload may have created.
    // Returns false on a global failure. id: the recording found, 0 if there is
    // none, -1 if Speakr has no usable list to look in.
    bool FindEarlierUpload(HttpClient& http, const Session& session, int& id);
    // Refreshes tagIds_ and the known tag list from Speakr. Callers decide
    // whether a failure matters; the start-of-cycle refresh ignores it.
    HttpResponse RefreshTags(HttpClient& http);
    // Returns false if the server is unreachable or rejects the token, which
    // ends the cycle early.
    bool Upload(HttpClient& http, Session& session, const Config& config);
    bool Check(HttpClient& http, Session& session, const Config& config);
    void ApplyRetention(Session& session, const Config& config);
    bool ResolveTags(HttpClient& http, const std::vector<std::wstring>& names, std::vector<int>& ids);
    // True if the failure affects every request (network down, bad token,
    // server error) rather than just this session.
    bool IsGlobalFailure(const HttpResponse& response);
    // Refuses the upload. clearAttempt=false keeps the "may have reached
    // Speakr" mark, for when it did.
    void Reject(Session& session, const std::wstring& reason, bool clearAttempt = true);
    void Notify(Event event);
    void SetBlocker(std::wstring blocker);

    HWND notifyWindow_;
    std::wstring sessionsDir_;
    HANDLE wakeEvent_;
    HANDLE stopEvent_;
    std::thread thread_;

    std::map<std::wstring, int> tagIds_;  // worker thread only
    std::wstring tagServer_;              // worker thread only; the server tagIds_ came from
    int consecutiveFailures_ = 0;         // worker thread only

    std::mutex mutex_;
    HttpClient* activeClient_ = nullptr;  // aborted on shutdown
    std::vector<Event> events_;
    Status status_;
    std::wstring serverUrl_;
    std::vector<std::wstring> knownTags_;
};
