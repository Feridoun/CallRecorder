// CallRecorder: a tray app that records calls and meetings into sessions and
// uploads them to Speakr for transcription and summaries.

#include "Autostart.h"
#include "Config.h"
#include "ConnectionDialog.h"
#include "Recorder.h"
#include "Session.h"
#include "Uploader.h"
#include "Util.h"
#include "resource.h"

#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
#include <windowsx.h>

#include <algorithm>
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
constexpr UINT_PTR kSetupTimer = 3;  // opens the connection dialog on first run, once the tray is up
constexpr double kMinimumSeconds = 2.0;  // shorter sessions are treated as accidental

enum Command : UINT {
    kToggle = 100, kPause, kMarker, kSensitive, kUploadNow, kRetryRefused, kOpenSpeakr, kSettings, kOpenFolder, kExit,
    kAutostart, kConnection,
    kTagFirst = 1000,  // kTagFirst + i toggles App::menuTags[i]
    kTagLast = 1999,
    kMicFirst = 2000,  // kMicFirst + i records from App::menuMics[i]
    kMicLast = 2099,
    kSpeakersFirst = 2100,  // kSpeakersFirst + i records App::menuSpeakers[i]
    kSpeakersLast = 2199,
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
    bool sensitive = false;  // applies to the current session and the next one
    std::vector<std::wstring> tags;      // Speakr tags for the current session and the next one
    std::vector<std::wstring> menuTags;  // the Tags submenu's items, in order, while it's open
    std::vector<std::wstring> menuMics;      // device IDs in the Microphone submenu; "" is the default
    std::vector<std::wstring> menuSpeakers;  // device IDs in the Playback submenu; "" is the default
    std::wstring notificationUrl;  // opened when the current notification is clicked
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

void Notify(const std::wstring& title, const std::wstring& text, DWORD flags = NIIF_INFO,
            const std::wstring& clickUrl = {}) {
    app.notificationUrl = clickUrl;
    NOTIFYICONDATAW data = app.tray;
    data.uFlags = NIF_INFO;
    wcsncpy_s(data.szInfoTitle, title.c_str(), _TRUNCATE);
    wcsncpy_s(data.szInfo, text.c_str(), _TRUNCATE);
    data.dwInfoFlags = flags;
    Shell_NotifyIconW(NIM_MODIFY, &data);
}

void OpenUrl(const std::wstring& url) {
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
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
    return {config.microphone, config.speakers};
}

void StartRecording() {
    Session session = Session::Create(app.sessionsDir, app.sensitive, app.tags);
    std::wstring error;
    if (!app.recorder->Start(session.audioPath, session.title, ChosenDevices(), error)) {
        Notify(L"Couldn't start recording", error, NIIF_ERROR);
        return;
    }
    session.Save();
    app.session = std::move(session);
    SetTimer(app.window, kTooltipTimer, 1000, nullptr);
    UpdateTray();
    std::wstring text = app.sensitive ? L"Local only: this session won't be uploaded." : L"Press Ctrl+Alt+R to stop.";
    if (!app.sensitive && !app.tags.empty()) text += L"\nTags: " + JoinTags(app.tags);
    Notify(L"Recording started", text);
}

void StopRecording() {
    if (!IsRecording()) return;
    app.recorder->Stop();
    KillTimer(app.window, kTooltipTimer);
    Session session = std::move(*app.session);
    app.session.reset();
    UpdateTray();

    double seconds = app.recorder->RecordedSeconds();
    if (seconds < kMinimumSeconds) {
        session.Discard();
        Notify(L"Recording discarded", L"It was shorter than 2 seconds.");
        return;
    }
    session.Finish(seconds);
    session.Save();
    Notify(L"Recording saved", session.title + L" (" + FormatDuration(seconds) + L")" +
                                   (session.sensitive ? L"\nKept on this PC only." : L"\nUploading to Speakr."));
    if (!session.sensitive) app.uploader->Wake();
}

// Puts sessions Speakr refused back in the queue (e.g. after fixing the cause).
void RetryRefused() {
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
    Notify(paused ? L"Recording paused" : L"Recording resumed", L"Ctrl+Alt+P to toggle.", NIIF_INFO | NIIF_NOSOUND);
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

// Saves the device picked in the Microphone or Playback submenu and, if
// recording, switches to it straight away.
void ChooseDevice(bool microphone, size_t index) {
    const auto& ids = microphone ? app.menuMics : app.menuSpeakers;
    if (index >= ids.size()) return;
    bool saved = microphone ? Config::SaveMicrophone(ids[index]) : Config::SaveSpeakers(ids[index]);
    if (!saved) {
        Notify(L"Couldn't save the device", L"Couldn't write " + Config::Path() + L".", NIIF_WARNING);
        return;
    }
    if (IsRecording()) app.recorder->SetDevices(ChosenDevices());
}

void OpenConnectionDialog() {
    if (ShowConnectionDialog(app.instance, nullptr)) app.uploader->Wake();
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

// The Microphone or Playback submenu: the Windows default, then each
// connected device. Fills app.menuMics or app.menuSpeakers and sets `label`
// for the parent menu item.
HMENU BuildDeviceMenu(bool microphone, const std::wstring& chosen, std::wstring& label) {
    auto& ids = microphone ? app.menuMics : app.menuSpeakers;
    UINT first = microphone ? kMicFirst : kSpeakersFirst;
    UINT last = microphone ? kMicLast : kSpeakersLast;
    ids = {L""};

    HMENU menu = CreatePopupMenu();
    AppendMenuW(menu, MF_STRING | (chosen.empty() ? MF_CHECKED : MF_UNCHECKED), first,
                microphone ? L"Windows default (communications)" : L"Windows default");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    std::wstring chosenName;
    for (const auto& device : Recorder::ListDevices(microphone)) {
        if (first + ids.size() > last) break;
        bool ticked = device.id == chosen;
        if (ticked) chosenName = device.name;
        AppendMenuW(menu, MF_STRING | (ticked ? MF_CHECKED : MF_UNCHECKED), first + static_cast<UINT>(ids.size()),
                    MenuLabel(device.name).c_str());
        ids.push_back(device.id);
    }
    if (!chosen.empty() && chosenName.empty()) {
        // Chosen but unplugged: recording uses the default until it's back.
        chosenName = Recorder::DeviceName(chosen);
        if (chosenName.empty()) chosenName = L"Chosen device";
        chosenName += L" (not connected)";
        AppendMenuW(menu, MF_STRING | MF_CHECKED | MF_GRAYED, 0, MenuLabel(chosenName).c_str());
    }

    label = (microphone ? L"Microphone: " : L"Playback: ") +
            (chosen.empty() ? std::wstring(L"Windows default") : chosenName);
    if (label.size() > 60) label = label.substr(0, 57) + L"...";
    label = MenuLabel(label);
    return menu;
}

void ShowMenu(POINT at) {
    HMENU menu = CreatePopupMenu();
    bool recording = IsRecording();
    bool paused = recording && app.recorder->IsPaused();
    UINT whenRecording = recording ? MF_ENABLED : MF_GRAYED;

    AppendMenuW(menu, MF_STRING, kToggle, recording ? L"Stop recording\tCtrl+Alt+R" : L"Start recording\tCtrl+Alt+R");
    AppendMenuW(menu, MF_STRING | whenRecording, kPause, paused ? L"Resume\tCtrl+Alt+P" : L"Pause\tCtrl+Alt+P");
    AppendMenuW(menu, MF_STRING | whenRecording, kMarker, L"Add marker\tCtrl+Alt+K");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | (app.sensitive ? MF_CHECKED : MF_UNCHECKED), kSensitive,
                L"Sensitive: keep on this PC only");
    std::wstring tagsLabel = L"Tags: " + (app.tags.empty() ? std::wstring(L"none") : JoinTags(app.tags));
    if (tagsLabel.size() > 60) tagsLabel = tagsLabel.substr(0, 57) + L"...";
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(BuildTagsMenu()), tagsLabel.c_str());
    DeviceChoice devices = ChosenDevices();
    std::wstring deviceLabel;
    HMENU mics = BuildDeviceMenu(true, devices.microphone, deviceLabel);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(mics), deviceLabel.c_str());
    HMENU speakers = BuildDeviceMenu(false, devices.speakers, deviceLabel);
    AppendMenuW(menu, MF_POPUP, reinterpret_cast<UINT_PTR>(speakers), deviceLabel.c_str());
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, UploadSummary().c_str());
    AppendMenuW(menu, MF_STRING, kUploadNow, L"Upload now");
    if (app.uploader->GetStatus().problems > 0) AppendMenuW(menu, MF_STRING, kRetryRefused, L"Retry refused uploads");
    AppendMenuW(menu, MF_STRING, kOpenSpeakr, L"Open Speakr");
    AppendMenuW(menu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(menu, MF_STRING, kOpenFolder, L"Open recordings folder");
    AppendMenuW(menu, MF_STRING, kConnection, L"Speakr connection...");
    AppendMenuW(menu, MF_STRING, kSettings, L"Settings...");
    AppendMenuW(menu, MF_STRING | (Autostart::IsEnabled() ? MF_CHECKED : MF_UNCHECKED), kAutostart,
                L"Start at login");
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
    if (command >= kMicFirst && command <= kMicLast) return ChooseDevice(true, command - kMicFirst);
    if (command >= kSpeakersFirst && command <= kSpeakersLast) return ChooseDevice(false, command - kSpeakersFirst);
    switch (command) {
        case kToggle: IsRecording() ? StopRecording() : StartRecording(); break;
        case kPause: TogglePause(); break;
        case kMarker: AddMarker(); break;
        case kSensitive: ToggleSensitive(); break;
        case kUploadNow: app.uploader->Wake(); break;
        case kRetryRefused: RetryRefused(); break;
        case kOpenSpeakr: {
            std::wstring url = Config::Load().serverUrl;
            url.empty() ? OpenConnectionDialog() : OpenUrl(url);
            break;
        }
        case kConnection: OpenConnectionDialog(); break;
        case kSettings:
            Config::Load();  // make sure the file exists
            ShellExecuteW(nullptr, L"open", L"notepad.exe", Config::Path().c_str(), nullptr, SW_SHOWNORMAL);
            break;
        case kOpenFolder: OpenUrl(app.sessionsDir); break;
        case kAutostart: {
            bool enable = !Autostart::IsEnabled();
            if (!Autostart::SetEnabled(enable)) {
                Notify(L"Couldn't change Start at login", L"Windows refused the registry change.", NIIF_WARNING);
            } else if (enable) {
                Notify(L"Start at login: on", L"CallRecorder will start in the tray when you sign in.",
                       NIIF_INFO | NIIF_NOSOUND);
            }
            break;
        }
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
                    if (!app.notificationUrl.empty()) OpenUrl(app.notificationUrl);
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
            if (wParam == kSetupTimer) {
                KillTimer(window, kSetupTimer);
                OpenConnectionDialog();
            }
            return 0;
        case Recorder::kWarningMessage: {
            std::wstring warning = app.recorder->TakeWarning();
            if (!warning.empty()) Notify(kAppName, warning, NIIF_WARNING);
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
    app.sensitive = config.sensitiveByDefault;
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

    std::wstring unavailable;
    for (const auto& hotkey : kHotkeys) {
        if (!RegisterHotKey(app.window, hotkey.id, MOD_CONTROL | MOD_ALT | MOD_NOREPEAT, hotkey.key)) {
            unavailable += (unavailable.empty() ? L"" : L", ") + std::wstring(hotkey.label);
        }
    }
    if (!unavailable.empty()) {
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
