// CallRecorder: a tray app that records calls and meetings into sessions and
// uploads them to Speakr for transcription and summaries.

#include "Autostart.h"
#include "CallDetector.h"
#include "CallLogic.h"
#include "Config.h"
#include "HistoryWindow.h"
#include "Recorder.h"
#include "Session.h"
#include "SettingsDialog.h"
#include "Uploader.h"
#include "Util.h"
#include "resource.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

namespace {

constexpr wchar_t kAppName[] = L"CallRecorder";
constexpr UINT kTrayMessage = WM_APP + 1;
static_assert(kTrayMessage != Recorder::kWarningMessage && kTrayMessage != Uploader::kMessage &&
              Recorder::kWarningMessage != Uploader::kMessage);
constexpr UINT kTrayId = 1;
constexpr UINT_PTR kTooltipTimer = 1;
constexpr UINT_PTR kMenuTimer = 2;  // delays the left-click menu so a double-click can win
constexpr UINT_PTR kSetupTimer = 3;  // opens Settings on first run, once the tray is up
constexpr UINT_PTR kCallTimer = 4;   // looks for calls starting and ending
constexpr UINT kCallPollMs = 2000;
constexpr double kMinimumSeconds = 2.0;  // shorter sessions are treated as accidental
// A recording started and stopped by call detection that's shorter than this
// was most likely a microphone check (e.g. Teams' pre-join screen), not a call.
constexpr double kMinimumCallSeconds = 20.0;

enum Command : UINT {
    kToggle = 100, kPause, kMarker, kSensitive, kUploadNow, kRetryRefused, kOpenSpeakr, kSettings, kOpenFolder, kExit,
    kKeepLast, kUploadHeld, kRecordings,
    kTagFirst = 1000,  // kTagFirst + i toggles App::menuTags[i]
    kTagLast = 1999,
};

struct Hotkey {
    int id;
    UINT key;
    const wchar_t* label;
    Command command;
};
constexpr Hotkey kHotkeys[] = {
    {1, 'R', L"Ctrl+Alt+R", kToggle},
    {2, 'P', L"Ctrl+Alt+P", kPause},
    {3, 'K', L"Ctrl+Alt+K", kMarker},  // Ctrl+Alt+M is commonly taken by other apps
};
constexpr size_t kHotkeyCount = std::size(kHotkeys);

struct App {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    UINT taskbarCreated = 0;
    NOTIFYICONDATAW tray{};
    HICON idleIcon = nullptr;
    HICON recordingIcon = nullptr;
    HICON localOnlyIcon = nullptr;
    HICON pausedIcon = nullptr;
    std::unique_ptr<Recorder> recorder;
    std::unique_ptr<Uploader> uploader;
    std::optional<Session> session;
    std::wstring sessionsDir;
    std::array<bool, kHotkeyCount> hotkeyRegistered{};  // parallel to kHotkeys; another app may own a combination
    std::wstring lastMetaPath;  // sidecar of the last recording saved this run, for "Keep last recording"
    bool sensitive = false;  // applies to the current session and the next one
    std::vector<std::wstring> tags;      // Speakr tags for the current session and the next one
    std::vector<std::wstring> menuTags;  // the Tags submenu's items, in order, while it's open
    std::function<void()> notificationAction;  // runs when the current notification is clicked
    CallLogic::CallTracker calls;
    std::vector<DetectedCall> detectedCalls;  // from the last check
    std::wstring callApp;  // the call the current recording is of ("Teams"); empty if none
    bool callStartedRecording = false;  // call detection started the current recording
    bool callSubjectFound = false;  // the current recording's title names its meeting
    POINT menuPoint{};
    ULONGLONG ignoreSelectUntil = 0;
};
App app;

HICON MakeDotIcon(COLORREF color) {
    int size = GetSystemMetrics(SM_CXSMICON);
    HDC screen = GetDC(nullptr);
    HDC dc = CreateCompatibleDC(screen);
    HBITMAP image = CreateCompatibleBitmap(screen, size, size);
    HBITMAP mask = CreateBitmap(size, size, 1, 1, nullptr);
    RECT all{0, 0, size, size};
    int inset = size / 8;

    // Black image + white mask outside the dot = transparent.
    HGDIOBJ previous = SelectObject(dc, image);
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(BLACK_BRUSH)));
    HBRUSH fill = CreateSolidBrush(color);
    SelectObject(dc, fill);
    SelectObject(dc, GetStockObject(NULL_PEN));
    Ellipse(dc, inset, inset, size - inset + 1, size - inset + 1);

    SelectObject(dc, mask);
    FillRect(dc, &all, static_cast<HBRUSH>(GetStockObject(WHITE_BRUSH)));
    SelectObject(dc, GetStockObject(BLACK_BRUSH));
    Ellipse(dc, inset, inset, size - inset + 1, size - inset + 1);
    SelectObject(dc, previous);

    ICONINFO info{TRUE, 0, 0, mask, image};
    HICON icon = CreateIconIndirect(&info);
    DeleteObject(fill);
    DeleteObject(image);
    DeleteObject(mask);
    DeleteDC(dc);
    ReleaseDC(nullptr, screen);
    return icon;
}

bool IsRecording() {
    return app.session.has_value();
}

void OpenUrl(const std::wstring& url) {
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

// Shows a notification; clicking it runs `onClick`, if given.
void NotifyThen(const std::wstring& title, const std::wstring& text, std::function<void()> onClick,
                DWORD flags = NIIF_INFO) {
    app.notificationAction = std::move(onClick);
    NOTIFYICONDATAW data = app.tray;
    data.uFlags = NIF_INFO;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    data.dwInfoFlags = flags;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void Notify(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO,
            const std::wstring& clickUrl = {}) {
    NotifyThen(title, text, clickUrl.empty() ? std::function<void()>() : [clickUrl] { OpenUrl(clickUrl); }, flags);
}

// One line describing the upload queue, e.g. "1 uploading, 2 in Speakr".
std::wstring UploadSummary() {
    Uploader::Status status = app.uploader->GetStatus();
    if (!status.blocker.empty()) return L"Uploads paused: " + status.blocker;
    std::wstring parts;
    auto add = [&](int count, const wchar_t* what) {
        if (count == 0) return;
        parts += (parts.empty() ? L"" : L", ") + std::to_wstring(count) + L" " + what;
    };
    add(status.waiting, L"waiting to upload");
    add(status.processing, L"being transcribed");
    add(status.held, L"held");
    add(status.problems, L"with problems");
    return parts.empty() ? L"All recordings are in Speakr" : parts;
}

void UpdateTray() {
    std::wstring tip;
    if (!IsRecording()) {
        app.tray.hIcon = app.idleIcon;
        tip = std::wstring(kAppName) + L": ready";
        if (app.sensitive) tip += L" (next session local only)";
        tip += L"\n" + UploadSummary();
    } else {
        bool paused = app.recorder->IsPaused();
        app.tray.hIcon = paused ? app.pausedIcon : app.session->sensitive ? app.localOnlyIcon : app.recordingIcon;
        tip = (paused ? L"Paused " : L"Recording ") + FormatDuration(app.recorder->RecordedSeconds());
        if (app.session->sensitive) tip += L" (local only)";
    }
    wcsncpy_s(app.tray.szTip, tip.c_str(), _TRUNCATE);
    app.tray.uFlags = NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    Shell_NotifyIconW(NIM_MODIFY, &app.tray);
}

void AddTrayIcon() {
    app.tray.cbSize = sizeof app.tray;
    app.tray.hWnd = app.window;
    app.tray.uID = kTrayId;
    app.tray.uFlags = NIF_MESSAGE | NIF_ICON | NIF_TIP | NIF_SHOWTIP;
    app.tray.uCallbackMessage = kTrayMessage;
    app.tray.hIcon = app.idleIcon;
    Shell_NotifyIconW(NIM_ADD, &app.tray);
    app.tray.uVersion = NOTIFYICON_VERSION_4;
    Shell_NotifyIconW(NIM_SETVERSION, &app.tray);
    UpdateTray();
}

std::wstring JoinTags(const std::vector<std::wstring>& tags) {
    std::wstring joined;
    for (const auto& tag : tags) joined += (joined.empty() ? L"" : L", ") + tag;
    return joined;
}

DeviceChoice ChosenDevices() {
    Config config = Config::Load();
    return {config.microphone, config.speakers, config.loopbackApps, config.echoCancellation};
}

// The hotkey for a command, if it registered (else the tray menu is the only way).
const Hotkey* RegisteredHotkey(Command command) {
    for (size_t i = 0; i < kHotkeyCount; ++i) {
        if (kHotkeys[i].command == command && app.hotkeyRegistered[i]) return &kHotkeys[i];
    }
    return nullptr;
}

// "\tCtrl+Alt+R" for a menu item, or nothing if the hotkey isn't ours.
std::wstring Accelerator(Command command) {
    const Hotkey* hotkey = RegisteredHotkey(command);
    return hotkey ? std::wstring(L"\t") + hotkey->label : std::wstring();
}

std::wstring Plural(int count, const wchar_t* singular, const wchar_t* plural) {
    return std::to_wstring(count) + L" " + (count == 1 ? singular : plural);
}

// "1 minute", "90 seconds".
std::wstring DelayText(int seconds) {
    if (seconds % 60 == 0) return Plural(seconds / 60, L"minute", L"minutes");
    return Plural(seconds, L"second", L"seconds");
}

// Now plus `seconds`, as an ISO UTC timestamp.
std::wstring IsoUtcIn(int seconds) {
    FILETIME now;
    GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER ticks;
    ticks.LowPart = now.dwLowDateTime;
    ticks.HighPart = now.dwHighDateTime;
    ticks.QuadPart += static_cast<ULONGLONG>(seconds) * 10'000'000ULL;  // 100 ns ticks
    FILETIME later{ticks.LowPart, ticks.HighPart};
    SYSTEMTIME utc;
    FileTimeToSystemTime(&later, &utc);
    return FormatIsoUtc(utc);
}

// The meeting's name for a call found by the last check, if its window gave one.
std::wstring DetectedSubject(const std::wstring& callApp) {
    for (const auto& call : app.detectedCalls) {
        if (call.app == callApp) return call.subject;
    }
    return {};
}

// callApp: the call being recorded ("Teams"), if known; otherwise a call in
// progress, if any. automatic: started by call detection, not the user.
void StartRecording(std::wstring callApp = {}, bool automatic = false) {
    Config config = Config::Load();
    // An unreadable config fails closed: nothing leaves the PC until it's fixed.
    Session session = Session::Create(app.sessionsDir, app.sensitive || config.unreadable, app.tags, config.serverUrl);
    std::vector<std::wstring> activeCalls = app.calls.Active();
    if (callApp.empty() && !activeCalls.empty()) callApp = activeCalls.front();
    std::wstring subject = callApp.empty() ? std::wstring() : DetectedSubject(callApp);
    if (!callApp.empty()) {
        session.title = CallLogic::CallTitle(callApp, subject, FormatFriendlyLocal(session.startedUtc));
    }
    std::wstring error;
    if (!app.recorder->Start(session.audioPath, session.title, ChosenDevices(), config.separateChannels, error)) {
        Notify(L"Couldn't start recording", error, NIIF_ERROR);
        return;
    }
    session.Save();
    app.session = std::move(session);
    app.callApp = callApp;
    app.callStartedRecording = automatic;
    app.callSubjectFound = !subject.empty();
    SetTimer(app.window, kTooltipTimer, 1000, nullptr);
    UpdateTray();
    const Hotkey* stopKey = RegisteredHotkey(kToggle);
    std::wstring stopHint = stopKey ? std::wstring(L"Press ") + stopKey->label + L" to stop." : L"Stop it from the tray menu.";
    std::wstring text = app.session->sensitive ? L"Local only: this session won't be uploaded." : stopHint;
    if (!app.session->sensitive && !app.tags.empty()) text += L"\nTags: " + JoinTags(app.tags);
    // Unlike the call app's own recording, nobody else on the call is told.
    if (automatic) text = L"Let everyone on the call know it's being recorded.\n" + text;
    Notify(automatic ? L"Recording " + callApp + L" call" : L"Recording started", text);
}

// byCallEnd: call detection is stopping it because the call ended.
void StopRecording(bool byCallEnd = false) {
    if (!IsRecording()) return;
    double minimum = byCallEnd && app.callStartedRecording ? kMinimumCallSeconds : kMinimumSeconds;
    app.callApp.clear();
    app.callStartedRecording = false;
    app.callSubjectFound = false;
    app.recorder->Stop();
    KillTimer(app.window, kTooltipTimer);
    Session session = std::move(*app.session);
    app.session.reset();
    UpdateTray();

    double seconds = app.recorder->RecordedSeconds();
    if (seconds < minimum) {
        session.Discard();
        Notify(L"Recording discarded", minimum == kMinimumSeconds
                                           ? L"It was shorter than 2 seconds."
                                           : L"The call lasted under 20 seconds, so it was probably a microphone check.");
        return;
    }
    session.Finish(seconds);
    // The grace period lets "Keep last recording on this PC" stop the upload.
    int delay = session.sensitive ? 0 : std::max(Config::Load().uploadDelaySeconds, 0);
    if (delay > 0) session.uploadAfterUtc = IsoUtcIn(delay);
    session.Save();
    app.lastMetaPath = session.metaPath;

    std::wstring outcome = L"\nUploading to Speakr.";
    if (session.sensitive) {
        outcome = L"\nKept on this PC only.";
    } else if (delay > 0) {
        outcome = L"\nUploading to Speakr in " + DelayText(delay) +
                  L". To keep it on this PC, choose Keep last recording on this PC.";
    }
    Notify(L"Recording saved", session.title + L" (" + FormatDuration(seconds) + L")" + outcome);
    if (!session.sensitive) app.uploader->Wake();
}

// Stops the last recording from being uploaded, if it hasn't been yet.
void KeepLastRecording() {
    if (app.lastMetaPath.empty()) return;
    std::wstring message;
    {
        std::lock_guard lock(Session::FileMutex());
        auto session = Session::Load(app.lastMetaPath);
        if (!session) {
            app.lastMetaPath.clear();
            Notify(L"Recording not found", L"It may have been deleted.", NIIF_WARNING);
            return;
        }
        const std::string& state = session->uploadState;
        bool attempted = !session->uploadAttemptUtc.empty();  // a request may have reached Speakr
        if (state == UploadState::kLocalOnly) {
            message = L"It's already kept on this PC only.";
        } else if (state == UploadState::kUploaded || state == UploadState::kDone || state == UploadState::kFailed ||
                   state == UploadState::kMissing || state == UploadState::kStuck ||
                   (state == UploadState::kPending && attempted)) {
            message = L"It has already gone to Speakr. Delete it there if you need to.";
        } else {
            session->SetSensitive(true);
            message = session->Save() ? L"Kept on this PC. It won't be uploaded."
                                      : L"Couldn't update the recording's file.";
        }
    }
    Notify(L"Last recording", message);
}

// Puts sessions Speakr refused back in the queue (e.g. after fixing the cause).
void RetryRefused() {
    std::lock_guard lock(Session::FileMutex());
    for (const auto& path : Session::ListMetaFiles(app.sessionsDir)) {
        auto session = Session::Load(path);
        if (!session || session->uploadState != UploadState::kRejected) continue;
        session->uploadState = UploadState::kPending;
        session->uploadError.clear();
        session->Save();
    }
    app.uploader->Wake();
}

void TogglePause() {
    if (!IsRecording()) return;
    bool paused = !app.recorder->IsPaused();
    app.recorder->SetPaused(paused);
    UpdateTray();
    const Hotkey* pauseKey = RegisteredHotkey(kPause);
    std::wstring hint = pauseKey ? std::wstring(pauseKey->label) + L" to toggle."
                                 : std::wstring(L"Use the tray menu to ") + (paused ? L"resume." : L"pause.");
    Notify(paused ? L"Recording paused" : L"Recording resumed", hint, NIIF_INFO | NIIF_NOSOUND);
}

void AddMarker() {
    if (!IsRecording()) return;
    double at = app.recorder->RecordedSeconds();
    std::wstring label = L"Marker " + std::to_wstring(app.session->markers.size() + 1);
    app.session->markers.push_back({at, label});
    app.session->Save();
    Notify(label + L" at " + FormatDuration(at), L"Saved with this session.", NIIF_INFO | NIIF_NOSOUND);
}

void ToggleSensitive() {
    app.sensitive = !app.sensitive;
    if (IsRecording()) {
        app.session->SetSensitive(app.sensitive);
        app.session->Save();
    }
    UpdateTray();
}

// Ticks or unticks a tag for the current session (if any) and the next ones.
void ToggleTag(size_t index) {
    if (index >= app.menuTags.size()) return;
    const std::wstring& tag = app.menuTags[index];
    auto found = std::find(app.tags.begin(), app.tags.end(), tag);
    if (found != app.tags.end()) {
        app.tags.erase(found);
    } else {
        app.tags.push_back(tag);
    }
    if (IsRecording()) {
        app.session->tags = app.tags;
        app.session->Save();
    }
}

// " or press Ctrl+Alt+R", if that hotkey is ours.
std::wstring ToggleKeyHint() {
    const Hotkey* key = RegisteredHotkey(kToggle);
    return key ? std::wstring(L" or press ") + key->label : std::wstring();
}

void OnCallStarted(const std::wstring& callApp, CallDetection mode) {
    if (IsRecording()) {
        // Recording already, e.g. started by hand just before joining: it's of this call.
        if (app.callApp.empty()) {
            app.callApp = callApp;
            app.session->title = CallLogic::CallTitle(callApp, {}, FormatFriendlyLocal(app.session->startedUtc));
            app.session->Save();
        }
        return;
    }
    if (mode == CallDetection::kAuto) return StartRecording(callApp, true);
    NotifyThen(callApp + L" call detected", L"Click here to record it" + ToggleKeyHint() + L".", [callApp] {
        if (!IsRecording() && app.calls.IsActive(callApp)) StartRecording(callApp);
    });
}

void OnCallEnded(const std::wstring& callApp, CallDetection mode) {
    if (!IsRecording() || app.callApp != callApp) return;
    if (mode == CallDetection::kAuto) return StopRecording(true);
    std::wstring id = app.session->id;
    NotifyThen(callApp + L" call ended", L"Still recording. Click here to stop" + ToggleKeyHint() + L".", [id] {
        if (IsRecording() && app.session->id == id) StopRecording();
    });
}

// Runs every kCallPollMs while call detection is on.
void CheckCalls() {
    app.detectedCalls = DetectCalls();
    std::vector<std::wstring> active;
    for (const auto& call : app.detectedCalls) active.push_back(call.app);
    auto events = app.calls.Update(active, GetTickCount64());
    // Read afresh, so a hand edit to config.json applies like other settings.
    CallDetection mode = events.empty() ? CallDetection::kOff : Config::Load().callDetection;
    if (mode == CallDetection::kOff) events.clear();
    for (const auto& event : events) {
        event.started ? OnCallStarted(event.app, mode) : OnCallEnded(event.app, mode);
    }
    // A Teams meeting window can appear a little after the call starts.
    if (IsRecording() && !app.callApp.empty() && !app.callSubjectFound) {
        std::wstring subject = DetectedSubject(app.callApp);
        if (!subject.empty()) {
            app.session->title =
                CallLogic::CallTitle(app.callApp, subject, FormatFriendlyLocal(app.session->startedUtc));
            app.session->Save();
            app.callSubjectFound = true;
        }
    }
}

// Starts or stops looking for calls, as the settings say.
void ApplyCallDetection() {
    if (Config::Load().callDetection == CallDetection::kOff) {
        KillTimer(app.window, kCallTimer);
        app.calls.Clear();
        app.detectedCalls.clear();
    } else {
        SetTimer(app.window, kCallTimer, kCallPollMs, nullptr);
    }
}

// Opens Settings and applies what can change straight away: devices (even
// mid-recording) and anything the uploader reads.
void OpenSettings() {
    bool separateBefore = Config::Load().separateChannels;
    if (!ShowSettingsDialog(app.instance, nullptr)) return;
    if (IsRecording()) {
        app.recorder->SetDevices(ChosenDevices());
        if (Config::Load().separateChannels != separateBefore) {
            Notify(L"Settings saved", L"The channel layout applies from the next recording.", NIIF_INFO | NIIF_NOSOUND);
        }
    }
    ApplyCallDetection();
    app.uploader->Wake();
}

// Made for another Speakr server, or before one was set up: waits for the user.
bool IsHeld(const Session& session, const Config& config) {
    if (session.uploadState == UploadState::kHeld) return true;
    return session.uploadState == UploadState::kPending && session.status != "recording" &&
           session.serverUrl.has_value() &&
           _wcsicmp(NormalizeServerUrl(*session.serverUrl).c_str(), NormalizeServerUrl(config.serverUrl).c_str()) != 0;
}

// Asks what to do with recordings made for another (or no) Speakr server.
void HandleHeld() {
    Config config = Config::Load();
    if (config.serverUrl.empty()) return OpenSettings();
    int count = app.uploader->GetStatus().held;
    if (count <= 0) return;
    bool one = count == 1;
    std::wstring text = (one ? std::wstring(L"1 recording was") : std::to_wstring(count) + L" recordings were") +
                        L" made before Speakr was set up or for a different server.\n\nUpload " +
                        (one ? L"it" : L"them") + L" to " + config.serverUrl + L"?\n\nYes: upload " +
                        (one ? L"it" : L"them") + L". No: keep " + (one ? L"it" : L"them") + L" on this PC only.";
    int answer = MessageBoxW(app.window, text.c_str(), kAppName,
                             MB_YESNOCANCEL | MB_ICONQUESTION | MB_DEFBUTTON3 | MB_SETFOREGROUND);
    if (answer != IDYES && answer != IDNO) return;
    {
        std::lock_guard lock(Session::FileMutex());
        for (const auto& path : Session::ListMetaFiles(app.sessionsDir)) {
            auto session = Session::Load(path);
            if (!session || !IsHeld(*session, config)) continue;
            if (answer == IDYES) {
                session->serverUrl = config.serverUrl;
                session->uploadState = UploadState::kPending;
                session->uploadError.clear();
            } else {
                session->SetSensitive(true);
            }
            session->Save();
        }
    }
    app.uploader->Wake();
    UpdateTray();
}

bool IsSetUp() {
    return !Config::Load().serverUrl.empty() && !ReadSpeakrToken().empty();
}

std::wstring MenuLabel(const std::wstring& text) {
    // A single & would be read as a keyboard accelerator.
    std::wstring label;
    for (wchar_t c : text) label += c == L'&' ? L"&&" : std::wstring(1, c);
    return label;
}

// Speakr's tags plus any ticked ones it doesn't have yet (they're created on
// upload), sorted case-insensitively.
HMENU BuildTagsMenu() {
    std::vector<std::wstring> known = app.uploader->KnownTags();
    if (known.empty()) app.uploader->Wake();  // fetch them for next time
    app.menuTags = app.tags;
    for (const auto& tag : known) {
        if (std::find(app.menuTags.begin(), app.menuTags.end(), tag) == app.menuTags.end()) app.menuTags.push_back(tag);
    }
    std::sort(app.menuTags.begin(), app.menuTags.end(), [](const std::wstring& a, const std::wstring& b) {
        return CompareStringOrdinal(a.c_str(), -1, b.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    if (app.menuTags.size() > kTagLast - kTagFirst + 1) app.menuTags.resize(kTagLast - kTagFirst + 1);

    HMENU tags = CreatePopupMenu();
    AppendMenuW(tags, MF_STRING | MF_GRAYED, 0,
                IsRecording() ? L"For this recording and the next ones" : L"For the next recording");
    AppendMenuW(tags, MF_SEPARATOR, 0, nullptr);
    if (app.menuTags.empty()) {
        AppendMenuW(tags, MF_STRING | MF_GRAYED, 0, L"No tags in Speakr yet");
    }
    for (size_t i = 0; i < app.menuTags.size(); ++i) {
        bool ticked = std::find(app.tags.begin(), app.tags.end(), app.menuTags[i]) != app.tags.end();
        AppendMenuW(tags, MF_STRING | (ticked ? MF_CHECKED : MF_UNCHECKED), kTagFirst + static_cast<UINT>(i),
                    MenuLabel(app.menuTags[i]).c_str());
    }
    return tags;
}

void ShowMenu(POINT at) {
    HMENU menu = CreatePopupMenu();
    bool recording = IsRecording();
    bool paused = recording && app.recorder->IsPaused();
    UINT whenRecording = recording ? MF_ENABLED : MF_GRAYED;

    std::vector<std::wstring> activeCalls = app.calls.Active();
    std::wstring startLabel = activeCalls.empty() ? L"Start recording" : L"Record " + activeCalls.front() + L" call";
    AppendMenuW(menu, MF_STRING, kToggle, ((recording ? L"Stop recording" : startLabel) + Accelerator(kToggle)).c_str());
    AppendMenuW(menu, MF_STRING | whenRecording, kPause,
                ((paused ? L"Resume" : L"Pause") + Accelerator(kPause)).c_str());
    AppendMenuW(menu, MF_STRING | whenRecording, kMarker, (L"Add marker" + Accelerator(kMarker)).c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (app.sensitive ? MF_CHECKED : MF_UNCHECKED), kSensitive,
                L"Sensitive: keep on this PC only");
    std::wstring tagsLabel = L"Tags: " + (app.tags.empty() ? std::wstring(L"none") : JoinTags(app.tags));
    if (tagsLabel.size() > 60) tagsLabel = tagsLabel.substr(0, 57) + L"...";
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildTagsMenu()), tagsLabel.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, UploadSummary().c_str());
    AppendMenuW(menu, MF_STRING | (app.lastMetaPath.empty() ? MF_GRAYED : MF_ENABLED), kKeepLast,
                L"Keep last recording on this PC");
    AppendMenuW(menu, MF_STRING, kUploadNow, L"Upload now");
    Uploader::Status status = app.uploader->GetStatus();
    if (status.held > 0) {
        std::wstring label = L"Upload " + Plural(status.held, L"held recording", L"held recordings") + L"...";
        AppendMenuW(menu, MF_STRING, kUploadHeld, label.c_str());
    }
    // problems also counts missing and stuck ones, which retrying wouldn't help;
    // RetryRefused only requeues the refused ones.
    if (status.problems > 0) AppendMenuW(menu, MF_STRING, kRetryRefused, L"Retry refused uploads");
    AppendMenuW(menu, MF_STRING, kOpenSpeakr, L"Open Speakr");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kRecordings, L"Recordings...");
    AppendMenuW(menu, MF_STRING, kOpenFolder, L"Open recordings folder");
    AppendMenuW(menu, MF_STRING, kSettings, L"Settings...");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kExit, recording ? L"Stop, save and exit" : L"Exit");
    SetMenuDefaultItem(menu, kToggle, FALSE);

    // Required so the menu closes when the user clicks elsewhere.
    SetForegroundWindow(app.window);
    TrackPopupMenu(menu, TPM_RIGHTBUTTON | TPM_BOTTOMALIGN, at.x, at.y, 0, app.window, nullptr);
    PostMessageW(app.window, WM_NULL, 0, 0);
    DestroyMenu(menu);
}

void RunCommand(UINT command) {
    if (command >= kTagFirst && command <= kTagLast) return ToggleTag(command - kTagFirst);
    switch (command) {
        case kToggle: IsRecording() ? StopRecording() : StartRecording(); break;
        case kPause: TogglePause(); break;
        case kMarker: AddMarker(); break;
        case kSensitive: ToggleSensitive(); break;
        case kKeepLast: KeepLastRecording(); break;
        case kUploadHeld: HandleHeld(); break;
        case kRecordings:
            ShowHistoryWindow(app.instance, app.sessionsDir, Config::Load().serverUrl, [] { app.uploader->Wake(); });
            break;
        case kUploadNow: app.uploader->Wake(); break;
        case kRetryRefused: RetryRefused(); break;
        case kOpenSpeakr: {
            std::wstring url = Config::Load().serverUrl;
            url.empty() ? OpenSettings() : OpenUrl(url);
            break;
        }
        case kSettings: OpenSettings(); break;
        case kOpenFolder: OpenUrl(app.sessionsDir); break;
        case kExit: DestroyWindow(app.window); break;
    }
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    if (app.taskbarCreated != 0 && message == app.taskbarCreated) {  // Explorer restarted
        AddTrayIcon();
        return 0;
    }
    switch (message) {
        case kTrayMessage:
            switch (LOWORD(lParam)) {
                case NIN_SELECT:  // left click: menu, unless it turns into a double-click
                    if (GetTickCount64() < app.ignoreSelectUntil) break;
                    app.menuPoint = {GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)};
                    SetTimer(window, kMenuTimer, GetDoubleClickTime(), nullptr);
                    break;
                case WM_LBUTTONDBLCLK:  // double-click: start/stop recording
                    KillTimer(window, kMenuTimer);
                    app.ignoreSelectUntil = GetTickCount64() + GetDoubleClickTime();
                    RunCommand(kToggle);
                    break;
                case NIN_KEYSELECT:
                case WM_CONTEXTMENU:
                    ShowMenu({GET_X_LPARAM(wParam), GET_Y_LPARAM(wParam)});
                    break;
                case NIN_BALLOONUSERCLICK:
                    if (app.notificationAction) {
                        // A copy: the action may show a notification, which replaces it.
                        auto action = app.notificationAction;
                        action();
                    }
                    break;
            }
            return 0;
        case WM_HOTKEY:
            for (const auto& hotkey : kHotkeys) {
                if (hotkey.id == static_cast<int>(wParam)) RunCommand(hotkey.command);
            }
            return 0;
        case WM_COMMAND:
            RunCommand(LOWORD(wParam));
            return 0;
        case WM_TIMER:
            if (wParam == kTooltipTimer) UpdateTray();
            if (wParam == kMenuTimer) {
                KillTimer(window, kMenuTimer);
                ShowMenu(app.menuPoint);
            }
            if (wParam == kCallTimer) CheckCalls();
            if (wParam == kSetupTimer) {
                KillTimer(window, kSetupTimer);
                OpenSettings();
            }
            return 0;
        case Recorder::kWarningMessage: {
            // Windows shows one notification at a time; the latest wins.
            for (const auto& warning : app.recorder->TakeWarnings()) Notify(kAppName, warning, NIIF_WARNING);
            return 0;
        }
        case Uploader::kMessage:
            // Windows shows one notification at a time; the latest wins.
            for (const auto& event : app.uploader->TakeEvents()) {
                Notify(event.title, event.text, event.problem ? NIIF_WARNING : NIIF_INFO, event.url);
            }
            UpdateTray();
            return 0;
        case WM_QUERYENDSESSION:
            return TRUE;
        case WM_ENDSESSION:
            if (wParam) StopRecording();  // Windows is shutting down or signing out
            return 0;
        case WM_DESTROY:
            StopRecording();
            for (const auto& hotkey : kHotkeys) UnregisterHotKey(window, hotkey.id);
            Shell_NotifyIconW(NIM_DELETE, &app.tray);
            PostQuitMessage(0);
            return 0;
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, PWSTR, int) {
    HANDLE singleInstance = CreateMutexW(nullptr, TRUE, L"Local\\CallRecorder.SingleInstance");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        MessageBoxW(nullptr, L"CallRecorder is already running. Look for it in the system tray.", kAppName,
                    MB_ICONINFORMATION);
        return 0;
    }
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);

    app.sessionsDir = SessionsDirectory();
    if (app.sessionsDir.empty()) {
        MessageBoxW(nullptr, L"Couldn't create the recordings folder under %LOCALAPPDATA%.", kAppName, MB_ICONERROR);
        return 1;
    }

    app.instance = instance;
    WNDCLASSEXW windowClass{sizeof windowClass};
    windowClass.lpfnWndProc = WindowProc;
    windowClass.hInstance = instance;
    windowClass.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    windowClass.lpszClassName = L"CallRecorderWindow";
    RegisterClassExW(&windowClass);
    // A hidden top-level window rather than a message-only one: message-only
    // windows don't receive the TaskbarCreated broadcast.
    app.window = CreateWindowExW(0, windowClass.lpszClassName, kAppName, WS_OVERLAPPED, 0, 0, 0, 0, nullptr,
                                 nullptr, instance, nullptr);
    app.taskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
    Config config = Config::Load();
    app.sensitive = config.sensitiveByDefault || config.unreadable;  // unreadable: fail closed
    app.tags = config.tags;
    Autostart::RepairPath();
    int recovered = Session::RecoverInterrupted(app.sessionsDir);  // before the uploader looks at them
    app.recorder = std::make_unique<Recorder>(app.window);
    app.uploader = std::make_unique<Uploader>(app.window, app.sessionsDir);

    app.idleIcon = MakeDotIcon(RGB(128, 128, 128));
    app.recordingIcon = MakeDotIcon(RGB(220, 38, 38));
    app.localOnlyIcon = MakeDotIcon(RGB(147, 51, 234));
    app.pausedIcon = MakeDotIcon(RGB(245, 158, 11));
    AddTrayIcon();
    ApplyCallDetection();

    std::wstring unavailable;
    for (size_t i = 0; i < kHotkeyCount; ++i) {
        const Hotkey& hotkey = kHotkeys[i];
        app.hotkeyRegistered[i] = RegisterHotKey(app.window, hotkey.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, hotkey.key);
        if (!app.hotkeyRegistered[i]) unavailable += (unavailable.empty() ? L"" : L", ") + std::wstring(hotkey.label);
    }
    // Windows shows one notification at a time; the most important goes last.
    if (config.unreadable) {
        Notify(L"Settings unreadable",
               L"Your settings file couldn't be read, so recordings stay on this PC. Fix or reset it in Settings.",
               NIIF_WARNING);
    } else if (!unavailable.empty()) {
        Notify(L"Some shortcuts are taken", unavailable + L" is used by another app. Use the tray menu instead.",
               NIIF_WARNING);
    } else if (recovered > 0) {
        Notify(L"Recovered interrupted recordings",
               std::to_wstring(recovered) + L" session(s) were cut off by a crash or power loss. The audio up to "
                                            L"that point is saved and will be uploaded.");
    }

    if (!IsSetUp()) SetTimer(app.window, kSetupTimer, 500, nullptr);

    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        if (IsHistoryWindowMessage(&message)) continue;  // Tab/Enter/Esc in the Recordings window
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }

    app.uploader.reset();
    app.recorder.reset();
    for (HICON icon : {app.idleIcon, app.recordingIcon, app.localOnlyIcon, app.pausedIcon}) DestroyIcon(icon);
    CoUninitialize();
    ReleaseMutex(singleInstance);
    CloseHandle(singleInstance);
    return 0;
}
