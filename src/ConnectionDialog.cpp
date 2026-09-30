#include "ConnectionDialog.h"

#include "Config.h"
#include "HttpClient.h"
#include "Util.h"
#include "resource.h"

#include <commctrl.h>

#include <nlohmann/json.hpp>

#include <string>

namespace {

HWND openDialog = nullptr;

std::wstring GetText(HWND dialog, int id) {
    HWND control = GetDlgItem(dialog, id);
    std::wstring text(GetWindowTextLengthW(control) + 1, L'\0');
    text.resize(GetWindowTextW(control, text.data(), static_cast<int>(text.size())));
    return text;
}

void SetStatus(HWND dialog, const std::wstring& text) {
    SetDlgItemTextW(dialog, IDC_STATUS, text.c_str());
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

void Test(HWND dialog) {
    std::wstring url = NormalizeServerUrl(GetText(dialog, IDC_URL));
    std::string token = TokenToUse(dialog);
    if (url.empty()) return SetStatus(dialog, L"Enter your Speakr address.");
    if (token.empty()) return SetStatus(dialog, L"Enter an API token.");
    SetStatus(dialog, L"Checking...");
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok;
    SetStatus(dialog, CheckConnection(url, token, ok));
    SetCursor(previous);
}

void Save(HWND dialog) {
    std::wstring url = NormalizeServerUrl(GetText(dialog, IDC_URL));
    std::wstring typedToken = GetText(dialog, IDC_TOKEN);
    std::string token = TokenToUse(dialog);
    if (url.empty()) return SetStatus(dialog, L"Enter your Speakr address.");
    if (token.empty()) return SetStatus(dialog, L"Enter an API token.");

    SetStatus(dialog, L"Checking...");
    HCURSOR previous = SetCursor(LoadCursorW(nullptr, IDC_WAIT));
    bool ok;
    std::wstring result = CheckConnection(url, token, ok);
    SetCursor(previous);
    SetStatus(dialog, result);
    if (!ok && MessageBoxW(dialog, (result + L"\n\nSave these settings anyway?").c_str(), L"CallRecorder",
                           MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2) != IDYES) {
        return;
    }

    if (!typedToken.empty() && !WriteSpeakrToken(ToUtf8(typedToken))) {
        return SetStatus(dialog, L"Windows Credential Manager refused to store the token.");
    }
    if (!Config::SaveServerUrl(url)) return SetStatus(dialog, L"Couldn't write " + Config::Path() + L".");
    EndDialog(dialog, IDOK);
}

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM) {
    switch (message) {
        case WM_INITDIALOG: {
            openDialog = dialog;
            HINSTANCE instance = reinterpret_cast<HINSTANCE>(GetWindowLongPtrW(dialog, GWLP_HINSTANCE));
            HICON big = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
            HICON small = static_cast<HICON>(LoadImageW(instance, MAKEINTRESOURCEW(IDI_APP), IMAGE_ICON,
                                                         GetSystemMetrics(SM_CXSMICON),
                                                         GetSystemMetrics(SM_CYSMICON), 0));
            SendMessageW(dialog, WM_SETICON, ICON_BIG, reinterpret_cast<LPARAM>(big));
            SendMessageW(dialog, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(small));

            Config config = Config::Load();
            SetDlgItemTextW(dialog, IDC_URL, config.serverUrl.c_str());
            SendDlgItemMessageW(dialog, IDC_URL, EM_SETCUEBANNER, TRUE,
                                reinterpret_cast<LPARAM>(L"https://speakr.example.com"));
            bool haveToken = !ReadSpeakrToken().empty();
            SendDlgItemMessageW(dialog, IDC_TOKEN, EM_SETCUEBANNER, TRUE,
                                reinterpret_cast<LPARAM>(haveToken ? L"Saved. Leave blank to keep it."
                                                                   : L"Paste your API token"));
            SetForegroundWindow(dialog);
            SetFocus(GetDlgItem(dialog, config.serverUrl.empty() ? IDC_URL : IDC_TOKEN));
            return FALSE;  // focus set above
        }
        case WM_COMMAND:
            switch (LOWORD(wParam)) {
                case IDC_TEST: Test(dialog); return TRUE;
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

bool ShowConnectionDialog(HINSTANCE instance, HWND owner) {
    if (openDialog) {
        SetForegroundWindow(openDialog);
        return false;
    }
    return DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_CONNECTION), owner, DialogProc, 0) == IDOK;
}
