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
#include <string>
#include <vector>

namespace {

HWND openDialog = nullptr;

// What the dialog was opened with, to tell what the user changed.
struct State {
    Config original;
    std::vector<std::wstring> micIds;      // IDC_MIC's items, in order; "" is the default
    std::vector<std::wstring> speakerIds;  // IDC_SPEAKERS's items, in order; "" is the default
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

std::wstring JoinTags(const std::vector<std::wstring>& tags) {
    std::wstring joined;
    for (const auto& tag : tags) joined += (joined.empty() ? L"" : L", ") + tag;
    return joined;
}

std::vector<std::wstring> SplitTags(const std::wstring& text) {
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

// Checks the address and token in the boxes, showing the result. Returns
// false (with the reason shown) if either is missing.
bool Test(HWND dialog, bool& ok) {
    ok = false;
    std::wstring url = NormalizeServerUrl(GetText(dialog, IDC_URL));
    std::string token = TokenToUse(dialog);
    if (url.empty()) return SetStatus(dialog, L"Enter your Speakr address."), false;
    if (token.empty()) return SetStatus(dialog, L"Enter an API token."), false;
    SetStatus(dialog, L"Checking...");
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    SetStatus(dialog, CheckConnection(url, token, ok));
    SetCursor(previous);
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

    SetDlgItemTextW(dialog, IDC_TAGS, JoinTags(config.tags).c_str());
    SetDlgItemTextW(dialog, IDC_HOTWORDS, config.hotwords.c_str());
    SendDlgItemMessageW(dialog, IDC_HOTWORDS, EM_SETCUEBANNER, TRUE,
                        reinterpret_cast<LPARAM>(L"e.g. Anika, Kubernetes, SLA"));
    SetDlgItemInt(dialog, IDC_KEEP_DAYS, std::max(config.keepAudioDays, 0), FALSE);
    CheckDlgButton(dialog, IDC_SENSITIVE, config.sensitiveByDefault ? BST_CHECKED : BST_UNCHECKED);
    CheckDlgButton(dialog, IDC_AUTOSTART, Autostart::IsEnabled() ? BST_CHECKED : BST_UNCHECKED);

    SetForegroundWindow(dialog);
    SetFocus(GetDlgItem(dialog, config.serverUrl.empty() ? IDC_URL : haveToken ? IDC_MIC : IDC_TOKEN));
}

void Save(HWND dialog) {
    Config config = state.original;
    config.serverUrl = NormalizeServerUrl(GetText(dialog, IDC_URL));
    std::wstring typedToken = Trim(GetText(dialog, IDC_TOKEN));

    // Only check the connection when it changed, so saving a device or tag
    // doesn't need the server to be reachable.
    if (config.serverUrl != state.original.serverUrl || !typedToken.empty()) {
        bool ok;
        if (!Test(dialog, ok)) return;
        if (!ok) {
            std::wstring result = GetText(dialog, IDC_STATUS);
            if (MessageBoxW(dialog, (result + L"\n\nSave these settings anyway?").c_str(), L"CallRecorder",
                            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
                return;
            }
        }
    }

    config.microphone = SelectedDevice(dialog, IDC_MIC, state.micIds);
    config.speakers = SelectedDevice(dialog, IDC_SPEAKERS, state.speakerIds);
    config.tags = SplitTags(GetText(dialog, IDC_TAGS));
    config.hotwords = Trim(GetText(dialog, IDC_HOTWORDS));
    BOOL daysOk = FALSE;
    UINT days = GetDlgItemInt(dialog, IDC_KEEP_DAYS, &daysOk, FALSE);
    config.keepAudioDays = daysOk ? static_cast<int>(std::min(days, 36500u)) : 0;
    config.sensitiveByDefault = IsDlgButtonChecked(dialog, IDC_SENSITIVE) == BST_CHECKED;

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

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM) {
    switch (message) {
        case WM_INITDIALOG:
            openDialog = dialog;
            Init(dialog);
            return FALSE;  // focus set in Init
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_TEST: {
                    bool ok;
                    Test(dialog, ok);
                    return TRUE;
                }
                case IDC_OPEN_FILE:
                    Config::Load();  // make sure the file exists
                    ShellExecuteW(dialog, L"open", L"notepad.exe", Config::Path().c_str(), nullptr, SW_SHOWNORMAL);
                    return TRUE;
                case IDOK: Save(dialog); return TRUE;
                case IDCANCEL: EndDialog(dialog, IDCANCEL); return TRUE;
            }
            break;
        case WM_DESTROY:
            openDialog = nullptr;
            break;
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
