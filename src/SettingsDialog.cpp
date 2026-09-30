#include "SettingsDialog.h"

#include "Autostart.h"
#include "Config.h"
#include "HttpClient.h"
#include "Recorder.h"
#include "Util.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <nlohmann/json.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// TODO(merge): declared in HttpClient.h
bool IsUnencryptedPublicUrl(const std::wstring& url);

namespace {

HWND openDialog = nullptr;
constexpr int kMaxUploadDelay = 3600;  // seconds

// Posted by the connection-test thread to the dialog; lParam is a TestResult*
// the receiver deletes.
constexpr UINT kTestDone = WM_APP + 1;

struct TestResult {
    std::wstring text;
    bool ok;
    bool thenSave;  // the test was started by Save, which carries on if it's fine
};

// Shared with the test thread so it never touches a dialog that's gone
// (an HWND value can be reused, so IsWindow wouldn't tell).
struct Liveness {
    std::mutex mutex;
    bool alive = true;
};

constexpr wchar_t kInsecureWarning[] =
    L"This address uses http, so your recordings and API token would travel unencrypted. Use https if you can.";
constexpr wchar_t kUnreadableNote[] =
    L"Your settings file couldn't be read. Saving will replace it; the old file is kept as config.json.bad.";

// What the dialog was opened with, to tell what the user changed.
struct State {
    Config original;
    std::vector<std::wstring> micIds;      // IDC_MIC's items, in order; "" is the default
    std::vector<std::wstring> speakerIds;  // IDC_SPEAKERS's items, in order; "" is the default
    bool testing = false;                  // a connection test is running
    std::shared_ptr<Liveness> liveness;
};
State state;

std::wstring GetText(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    std::wstring text(GetWindowTextLengthW(control) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    return text;
}

void SetStatus(HWND dialog, const std::wstring& text) {
    SetDlgItemTextW(dialog, IDC_STATUS, text.c_str());
}

std::wstring Trim(const std::wstring& text) {
    size_t start = text.find_first_not_of(L" \t\r\n");
    if (start == std::wstring::npos) return {};
    return text.substr(start, text.find_last_not_of(L" \t\r\n") - start + 1);
}

std::wstring JoinList(const std::vector<std::wstring>& tags) {
    std::wstring joined;
    for (const auto& tag : tags) joined += (joined.empty() ? L"" : L", ") + tag;
    return joined;
}

std::vector<std::wstring> SplitList(const std::wstring& text) {
    std::vector<std::wstring> tags;
    size_t start = 0;
    while (start <= text.size()) {
        size_t comma = text.find(L',', start);
        if (comma == std::wstring::npos) comma = text.size();
        std::wstring tag = Trim(text.substr(start, comma - start));
        if (!tag.empty() && std::find(tags.begin(), tags.end(), tag) == tags.end()) tags.push_back(tag);
        start = comma + 1;
    }
    return tags;
}

// Asks Speakr who the token belongs to. Returns a line for the dialog;
// ok is set if the server and token both work.
std::wstring CheckConnection(const std::wstring& url, const std::string& token, bool& ok) {
    ok = false;
    HttpClient http(url, token);
    HttpResponse response = http.Get(L"/api/v1/users/me");
    if (response.Ok()) {
        auto parsed = nlohmann::json::parse(response.body, nullptr, false);
        if (!parsed.is_object()) {
            return L"That address answered, but not like Speakr does. Check the address.";
        }
        ok = true;
        std::string user = parsed.value("username", "");
        return user.empty() ? L"Connected to Speakr." : L"Connected to Speakr as " + FromUtf8(user) + L".";
    }
    if (response.status == 401 || response.status == 403) return L"Speakr rejected the token. Create a new one.";
    if (response.status == 404) return L"No Speakr API at that address. Check it, and that Speakr is up to date.";
    if (response.status >= 300 && response.status < 400) {
        return L"The server redirected elsewhere. Check the address (http or https?).";
    }
    return L"Couldn't connect: " + response.Describe() + L".";
}

// Token from the box, or the saved one if the box is empty.
std::string TokenToUse(HWND dialog) {
    std::wstring typed = GetText(dialog, IDC_TOKEN);
    return typed.empty() ? ReadSpeakrToken() : ToUtf8(typed);
}

bool IsInsecure(HWND dialog) {
    std::wstring text = Trim(GetText(dialog, IDC_URL));
    return !text.empty() && IsUnencryptedPublicUrl(NormalizeServerUrl(text));
}

// What the status line says when no test result is showing.
void ShowBaselineStatus(HWND dialog) {
    if (IsInsecure(dialog)) return SetStatus(dialog, kInsecureWarning);
    SetStatus(dialog, state.original.unreadable ? kUnreadableNote : L"");
}

// While a test runs, the boxes and buttons that would change or repeat it are
// off; Cancel stays usable.
void SetTesting(HWND dialog, bool testing) {
    state.testing = testing;
    if (testing) SetFocus(GetDlgItem(dialog, IDCANCEL));
    for (int id : {IDC_TEST, IDOK, IDC_URL, IDC_TOKEN}) EnableWindow(GetDlgItem(dialog, id), !testing);
    if (!testing) SetFocus(GetDlgItem(dialog, IDOK));
}

// Starts checking the address and token in the boxes on a worker thread (a
// black-holed server can take minutes, and the hotkeys need the UI thread).
// The result comes back as kTestDone. Returns false (with the reason shown)
// if the address or token is missing.
bool BeginTest(HWND dialog, bool thenSave) {
    std::wstring url = NormalizeServerUrl(GetText(dialog, IDC_URL));
    std::string token = TokenToUse(dialog);
    if (url.empty()) return SetStatus(dialog, L"Enter your Speakr address."), false;
    if (token.empty()) return SetStatus(dialog, L"Enter an API token."), false;
    SetTesting(dialog, true);
    SetStatus(dialog, L"Checking...");
    std::thread([dialog, url, token, thenSave, live = state.liveness] {
        bool ok = false;
        std::wstring text = CheckConnection(url, token, ok);
        auto* result = new TestResult{std::move(text), ok, thenSave};
        // Posting under the lock: the dialog can't finish being destroyed meanwhile.
        std::lock_guard lock(live->mutex);
        if (!live->alive || !PostMessageW(dialog, kTestDone, 0, reinterpret_cast<LPARAM>(result))) delete result;
    }).detach();
    return true;
}

// Fills a device list: the Windows default, each connected device, and the
// chosen one if it's unplugged (recording uses the default until it's back).
void FillDevices(HWND dialog, int id, bool microphone, const std::wstring& chosen, std::vector<std::wstring>& ids) {
    HWND combo = GetDlgItem(dialog, id);
    ids = {L""};
    SendMessageW(combo, CB_ADDSTRING, 0,
                 reinterpret_cast<LPARAM>(microphone ? L"Windows default (communications)" : L"Windows default"));
    int selected = 0;
    for (const auto& device : Recorder::ListDevices(microphone)) {
        if (device.id == chosen) selected = static_cast<int>(ids.size());
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(device.name.c_str()));
        ids.push_back(device.id);
    }
    if (!chosen.empty() && selected == 0) {
        std::wstring name = Recorder::DeviceName(chosen);
        if (name.empty()) name = L"Chosen device";
        selected = static_cast<int>(ids.size());
        SendMessageW(combo, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>((name + L" (not connected)").c_str()));
        ids.push_back(chosen);
    }
    SendMessageW(combo, CB_SETCURSEL, selected, 0);
}

std::wstring SelectedDevice(HWND dialog, int id, const std::vector<std::wstring>& ids) {
    auto index = SendDlgItemMessageW(dialog, id, CB_GETCURSEL, 0, 0);
    return index >= 0 && static_cast<size_t>(index) < ids.size() ? ids[index] : std::wstring();
}

void Init(HWND dialog) {
    HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
    HICON big = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
    HICON small = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                 GetSystemMetrics(SM_CXSMICON), GetSystemMetrics(SM_CYSMICON), 0));
    SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
    SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));

    state = {};
    state.liveness = std::make_shared<Liveness>();
    state.original = Config::Load();
    const Config& config = state.original;

    SetDlgItemTextW(dialog, IDC_URL, config.serverUrl.c_str());
    SendDlgItemMessageW(dialog, IDC_URL, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(L"https://speakr.example.com"));
    bool haveToken = !ReadSpeakrToken().empty();
    SendDlgItemMessageW(dialog, IDC_TOKEN, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(haveToken ? L"Saved. Leave blank to keep it."
                                                           : L"Paste your API token"));

    FillDevices(dialog, IDC_MIC, true, config.microphone, state.micIds);
    FillDevices(dialog, IDC_SPEAKERS, false, config.speakers, state.speakerIds);

    SetDlgItemTextW(dialog, IDC_TAGS, JoinList(config.tags).c_str());
    SetDlgItemTextW(dialog, IDC_HOTWORDS, config.hotwords.c_str());
    SendDlgItemMessageW(dialog, IDC_HOTWORDS, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(L"e.g. Anika, Kubernetes, SLA"));
    SetDlgItemInt(dialog, IDC_KEEP_DAYS, std::max(config.keepAudioDays, 0), FALSE);
    SendDlgItemMessageW(dialog, IDC_UPLOAD_DELAY, EM_LIMITTEXT, 4, 0);
    SetDlgItemInt(dialog, IDC_UPLOAD_DELAY, static_cast<UINT>(std::clamp(config.uploadDelaySeconds, 0, kMaxUploadDelay)),
                  FALSE);
    SetDlgItemTextW(dialog, IDC_LOOPBACK_APPS, JoinList(config.loopbackApps).c_str());
    SendDlgItemMessageW(dialog, IDC_LOOPBACK_APPS, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(L"e.g. Teams.exe, Zoom.exe. Leave empty to record all playback audio."));
    CheckDlgButton(dialog, IDC_SEPARATE, config.separateChannels ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_SENSITIVE, config.sensitiveByDefault ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_AUTOSTART, Autostart::IsEnabled() ? BST_CHECKED : BST_UNCHECKED);

    ShowBaselineStatus(dialog);
    SetForegroundWindow(dialog);
    SetFocus(GetDlgItem(dialog, config.serverUrl.empty() ? IDC_URL : haveToken ? IDC_MIC : IDC_TOKEN));
}

// Writes the dialog's settings and closes it.
void FinishSave(HWND dialog) {
    Config config = state.original;
    config.serverUrl = NormalizeServerUrl(GetText(dialog, IDC_URL));
    std::wstring typedToken = Trim(GetText(dialog, IDC_TOKEN));

    config.microphone = SelectedDevice(dialog, IDC_MIC, state.micIds);
    config.speakers = SelectedDevice(dialog, IDC_SPEAKERS, state.speakerIds);
    config.tags = SplitList(GetText(dialog, IDC_TAGS));
    config.hotwords = Trim(GetText(dialog, IDC_HOTWORDS));
    BOOL daysOk = FALSE;
    UINT days = GetDlgItemInt(dialog, IDC_KEEP_DAYS, &daysOk, FALSE);
    config.keepAudioDays = daysOk ? static_cast<int>(std::min(days, 36500u)) : 0;
    config.sensitiveByDefault = IsDlgButtonChecked(dialog, IDC_SENSITIVE) == BST_CHECKED;
    BOOL delayOk = FALSE;
    UINT delay = GetDlgItemInt(dialog, IDC_UPLOAD_DELAY, &delayOk, FALSE);
    if (delayOk) config.uploadDelaySeconds = static_cast<int>(std::min<UINT>(delay, kMaxUploadDelay));
    config.separateChannels = IsDlgButtonChecked(dialog, IDC_SEPARATE) == BST_CHECKED;
    config.loopbackApps = SplitList(GetText(dialog, IDC_LOOPBACK_APPS));

    if (!typedToken.empty() && !WriteSpeakrToken(ToUtf8(typedToken))) {
        return SetStatus(dialog, L"Windows Credential Manager refused to store the token.");
    }
    if (!config.Save()) {
        return (void)MessageBoxW(dialog, (L"Couldn't write " + Config::Path() + L".").c_str(), L"CallRecorder",
                                 MB_OK | MB_ICONERROR);
    }
    bool autostart = IsDlgButtonChecked(dialog, IDC_AUTOSTART) == BST_CHECKED;
    if (autostart != Autostart::IsEnabled() && !Autostart::SetEnabled(autostart)) {
        MessageBoxW(dialog, L"Your settings were saved, but Windows refused to change Start at login.",
                    L"CallRecorder", MB_OK | MB_ICONWARNING);
    }
    EndDialog(dialog, IDOK);
}

// Checks the connection first if the address or token changed, so saving a
// device or tag doesn't need the server to be reachable. The save then
// finishes when the test's result arrives (OnTestDone).
void Save(HWND dialog) {
    if (state.testing) return;
    bool connectionChanged = NormalizeServerUrl(GetText(dialog, IDC_URL)) != state.original.serverUrl ||
                             !Trim(GetText(dialog, IDC_TOKEN)).empty();
    if (!connectionChanged) return FinishSave(dialog);
    if (IsInsecure(dialog) &&
        MessageBoxW(dialog, (std::wstring(kInsecureWarning) + L"\n\nSave this address anyway?").c_str(),
                    L"CallRecorder", MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    BeginTest(dialog, true);
}

void OnTestDone(HWND dialog, const TestResult& result) {
    SetTesting(dialog, false);
    std::wstring text = result.text;
    if (IsInsecure(dialog)) text += L"\n" + std::wstring(kInsecureWarning);
    SetStatus(dialog, text);
    if (!result.thenSave) return;
    if (!result.ok &&
        MessageBoxW(dialog, (result.text + L"\n\nSave these settings anyway?").c_str(), L"CallRecorder",
                    MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }
    FinishSave(dialog);
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_INITDIALOG:
            openDialog = dialog;
            Init(dialog);
            return FALSE;  // focus set in Init
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_TEST:
                    if (!state.testing) BeginTest(dialog, false);
                    return TRUE;
                case IDC_URL:
                    if (HIWORD(wParam) == EN_CHANGE && !state.testing) ShowBaselineStatus(dialog);
                    return TRUE;
                case IDC_OPEN_FILE:
                    Config::Load();  // make sure the file exists
                    ShellExecuteW(dialog, L"open", L"notepad.exe", Config::Path().c_str(), nullptr, SW_SHOWNORMAL);
                    return TRUE;
                case IDOK: Save(dialog); return TRUE;
                case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
            }
            break;
        case kTestDone:
            OnTestDone(dialog, *std::unique_ptr<TestResult>(reinterpret_cast<TestResult*>(lParam)));
            return TRUE;
        case WM_DESTROY: {
            openDialog = nullptr;
            {
                std::lock_guard lock(state.liveness->mutex);
                state.liveness->alive = false;
            }
            // A result posted just before this can't be delivered any more.
            MSG pending;
            while (PeekMessageW(&pending, dialog, kTestDone, kTestDone, PM_REMOVE)) {
                delete reinterpret_cast<TestResult*>(pending.lParam);
            }
            break;
        }
    }
    return FALSE;
}

}  // namespace

bool ShowSettingsDialog(HINSTANCE instance, HWND owner) {
    if (openDialog) {
        SetForegroundWindow(openDialog);
        return false;
    }
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), owner, DialogProc, 0) == IDOK;
}

#ifndef CALLRECORDER_HAVE_URL_CHECK
// Temporary until HttpClient.cpp provides it after the merge; remove then.
bool IsUnencryptedPublicUrl(const std::wstring&) {
    return false;
}
#endif
