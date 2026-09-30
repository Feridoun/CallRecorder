#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Call detection's decisions as small pure functions, so they can be
// unit-tested without a real call: which app a microphone user is, what a
// window title says about the meeting, and when a call has really started or
// ended. CallDetector.cpp feeds these from Windows' audio sessions and window
// list.
namespace CallLogic {

// A calling app MeetingRecorder recognises.
struct CallApp {
    std::wstring_view name;              // shown to the user and used in titles, e.g. L"Teams"
    std::wstring_view exes[4];           // executables whose microphone use means a call; windows to read titles from
    bool teamsTitles = false;            // its windows are titled "<meeting> | Microsoft Teams"
};

inline constexpr CallApp kCallApps[] = {
    {L"Teams", {L"ms-teams.exe", L"Teams.exe"}, true},
    {L"Zoom", {L"Zoom.exe"}},
    {L"Webex", {L"CiscoCollabHost.exe", L"Webex.exe", L"atmgr.exe"}},
    {L"Slack", {L"slack.exe"}},
    {L"Skype", {L"Skype.exe"}},
    {L"WhatsApp", {L"WhatsApp.exe", L"WhatsApp.Root.exe"}},
    {L"Discord", {L"Discord.exe"}},
    {L"8x8", {L"8x8 Work.exe"}},
    {L"MicroSIP", {L"microsip.exe"}},
    {L"Linphone", {L"linphone.exe"}},
    {L"RingCentral", {L"RingCentral.exe"}},
};

// Browsers use the microphone for many things, so a browser only counts as
// being in a call when one of its windows is showing a meeting (WebCall).
inline constexpr std::wstring_view kBrowsers[] = {L"chrome.exe", L"msedge.exe", L"firefox.exe", L"brave.exe",
                                                  L"opera.exe",  L"vivaldi.exe", L"arc.exe"};

inline bool SameText(std::wstring_view a, std::wstring_view b) {
    return CompareStringOrdinal(a.data(), static_cast<int>(a.size()), b.data(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

inline const CallApp* FindByExe(std::wstring_view exe) {
    for (const auto& app : kCallApps) {
        for (auto name : app.exes) {
            if (!name.empty() && SameText(name, exe)) return &app;
        }
    }
    return nullptr;
}

inline const CallApp* FindByName(std::wstring_view name) {
    for (const auto& app : kCallApps) {
        if (app.name == name) return &app;
    }
    return nullptr;
}

inline bool IsBrowser(std::wstring_view exe) {
    return std::any_of(std::begin(kBrowsers), std::end(kBrowsers), [&](auto b) { return SameText(b, exe); });
}

struct Process {
    DWORD pid;
    DWORD parent;
    std::wstring exe;
};

// Who a process using the microphone belongs to: a call app, a browser, or
// neither. Its parents count too, since apps often capture audio in a helper
// process (a browser's audio service, or Teams' WebView2).
struct MicUser {
    const CallApp* app = nullptr;
    std::wstring browser;  // executable name, if it's a browser's
};

inline MicUser MicUserOf(DWORD pid, const std::vector<Process>& processes) {
    constexpr int kMaxDepth = 4;
    for (int depth = 0; depth < kMaxDepth && pid != 0; ++depth) {
        auto process = std::find_if(processes.begin(), processes.end(), [&](const Process& p) { return p.pid == pid; });
        if (process == processes.end()) break;
        if (const CallApp* app = FindByExe(process->exe)) return {app, {}};
        if (IsBrowser(process->exe)) return {nullptr, process->exe};
        if (process->parent == pid) break;
        pid = process->parent;
    }
    return {};
}

inline std::wstring Trim(std::wstring_view text) {
    size_t start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring_view::npos) return {};
    return std::wstring(text.substr(start, text.find_last_not_of(L" \t\r\n") - start + 1));
}

// Parts of a Teams title that name a section of the main window, which shows
// whatever the user clicked on rather than the call.
inline bool IsTeamsSection(std::wstring_view part) {
    constexpr std::wstring_view kSections[] = {L"Activity", L"Chat",     L"Teams",       L"Calendar",
                                               L"Calls",    L"Files",    L"Apps",        L"OneDrive",
                                               L"Meet",     L"Search",   L"Communities", L"Assignments",
                                               L"Meeting chat"};
    return std::any_of(std::begin(kSections), std::end(kSections), [&](auto s) { return SameText(s, part); });
}

// Parts of a Teams title that label a meeting window without naming it.
inline bool IsTeamsWindowLabel(std::wstring_view part) {
    constexpr std::wstring_view kLabels[] = {L"Microsoft Teams", L"Meeting", L"Call", L"Meeting compact view",
                                             L"Compact meeting view", L"Meeting controls"};
    return std::any_of(std::begin(kLabels), std::end(kLabels), [&](auto l) { return SameText(l, part); });
}

// The meeting's name from a Teams window title, or empty. A meeting or call
// window is titled "Weekly sync | Microsoft Teams"; the main window names a
// view as well ("Chat | Anika | Microsoft Teams") and is ignored, since what
// it shows needn't be the call. Works for Teams in a browser too, whose
// title then goes on with " - Google Chrome" or similar.
inline std::wstring TeamsSubject(std::wstring_view title) {
    constexpr std::wstring_view kSuffix = L" | Microsoft Teams";
    size_t end = title.rfind(kSuffix);
    if (end == std::wstring_view::npos) return {};
    std::vector<std::wstring> parts;
    std::wstring_view rest = title.substr(0, end);
    while (true) {
        size_t bar = rest.find(L" | ");
        std::wstring part = Trim(rest.substr(0, bar));
        if (!part.empty()) parts.push_back(std::move(part));
        if (bar == std::wstring_view::npos) break;
        rest = rest.substr(bar + 3);
    }
    if (std::any_of(parts.begin(), parts.end(), [](const std::wstring& part) { return IsTeamsSection(part); })) {
        return {};
    }
    // "Meeting compact view | Weekly sync" still names one meeting.
    std::erase_if(parts, [](const std::wstring& part) { return IsTeamsWindowLabel(part); });
    if (parts.size() != 1) return {};
    return parts.front();
}

// A Google Meet code such as "abc-defg-hij", which says nothing about the meeting.
inline bool IsMeetCode(std::wstring_view text) {
    if (text.size() != 12 || text[3] != L'-' || text[8] != L'-') return false;
    for (size_t i = 0; i < text.size(); ++i) {
        if (i == 3 || i == 8) continue;
        if (text[i] < L'a' || text[i] > L'z') return false;
    }
    return true;
}

// A call running in a browser window, judged from the window's title.
struct WebCall {
    bool found = false;
    std::wstring_view app;  // "Google Meet" or "Teams"
    std::wstring subject;   // the meeting's name, if the title gives one
};

// Browser titles end with " - Google Chrome", " - Personal - Microsoft Edge"
// and so on. Google Meet tabs are titled "Meet - <meeting name or code>".
inline WebCall WebCallFromTitle(std::wstring_view title) {
    for (std::wstring_view prefix : {std::wstring_view(L"Meet - "), std::wstring_view(L"Meet – ")}) {
        if (!title.starts_with(prefix)) continue;
        std::wstring_view rest = title.substr(prefix.size());
        size_t end = std::min(rest.find(L" - "), rest.find(L" – "));
        std::wstring subject = Trim(rest.substr(0, end));
        if (IsMeetCode(subject)) subject.clear();
        return {true, L"Google Meet", std::move(subject)};
    }
    // A Teams tab only counts while it shows a meeting, not chat or calendar.
    std::wstring subject = TeamsSubject(title);
    if (!subject.empty()) return {true, L"Teams", std::move(subject)};
    return {};
}

// The recording's title: "Weekly sync (Teams)", or "Zoom call 30 Sep 2026
// 14:02" when the app doesn't say what the call is.
inline std::wstring CallTitle(std::wstring_view app, std::wstring_view subject, std::wstring_view friendlyStart) {
    std::wstring clean;
    for (wchar_t c : subject) clean += c < L' ' ? L' ' : c;
    clean = Trim(clean);
    constexpr size_t kMaxSubject = 120;
    if (clean.size() > kMaxSubject) clean = Trim(clean.substr(0, kMaxSubject)) + L"...";
    if (clean.empty()) return std::wstring(app) + L" call " + std::wstring(friendlyStart);
    return clean + L" (" + std::wstring(app) + L")";
}

// Turns "which apps have a call right now", sampled every few seconds, into
// calls starting and ending. A call starts once it's been seen for
// kStartAfterMs, so a quick microphone check doesn't count, and ends once it's
// been gone for kEndAfterMs, so switching headsets or rejoining doesn't split
// it.
class CallTracker {
public:
    static constexpr uint64_t kStartAfterMs = 2000;
    static constexpr uint64_t kEndAfterMs = 10000;

    struct Event {
        std::wstring app;
        bool started;  // false: ended
    };

    std::vector<Event> Update(const std::vector<std::wstring>& active, uint64_t nowMs) {
        std::vector<Event> events;
        for (const auto& app : active) {
            auto entry = Find(app);
            if (entry == entries_.end()) {
                entries_.push_back({app, nowMs, nowMs, false});
                entry = entries_.end() - 1;
            }
            entry->lastSeen = nowMs;
        }
        for (auto& entry : entries_) {
            bool seen = entry.lastSeen == nowMs;
            if (!entry.started && seen && nowMs - entry.firstSeen >= kStartAfterMs) {
                entry.started = true;
                events.push_back({entry.app, true});
            } else if (entry.started && !seen && nowMs - entry.lastSeen >= kEndAfterMs) {
                entry.started = false;
                events.push_back({entry.app, false});
            }
        }
        std::erase_if(entries_, [&](const Entry& entry) { return !entry.started && entry.lastSeen != nowMs; });
        return events;
    }

    // Apps whose call has started and not ended, oldest first.
    std::vector<std::wstring> Active() const {
        std::vector<std::wstring> apps;
        for (const auto& entry : entries_) {
            if (entry.started) apps.push_back(entry.app);
        }
        return apps;
    }
    bool IsActive(const std::wstring& app) const {
        auto entry = std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.app == app; });
        return entry != entries_.end() && entry->started;
    }
    void Clear() { entries_.clear(); }

private:
    struct Entry {
        std::wstring app;
        uint64_t firstSeen;
        uint64_t lastSeen;
        bool started;
    };
    std::vector<Entry>::iterator Find(const std::wstring& app) {
        return std::find_if(entries_.begin(), entries_.end(), [&](const Entry& e) { return e.app == app; });
    }
    std::vector<Entry> entries_;
};

}  // namespace CallLogic
