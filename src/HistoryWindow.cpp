#include "HistoryWindow.h"

#include "Session.h"
#include "Util.h"
#include "resource.h"

#include <commctrl.h>
#include <shellapi.h>

#include <algorithm>
#include <cmath>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace {

constexpr wchar_t kClassName[] = L"MeetingRecorderHistoryWindow";
constexpr wchar_t kAppName[] = L"MeetingRecorder";
constexpr UINT_PTR kRefreshTimer = 1;
constexpr UINT kRefreshMs = 2000;

enum ControlId {
    IDC_LIST = 100,
    IDC_EMPTY,
    IDC_HINT,
    IDC_OPEN,
    IDC_PLAY,
    IDC_FOLDER,
    IDC_UPLOAD,
    IDC_KEEP,
    IDC_DELETE,
};

enum Column { kDate, kLength, kTitle, kStatus, kDetails, kColumnCount };

// A loaded sidecar plus what we checked on disk when we loaded it.
struct Row {
    Session session;
    bool audioExists = false;
};

using Signature = std::vector<std::pair<std::wstring, ULONGLONG>>;

struct Ui {
    HINSTANCE instance = nullptr;
    HWND window = nullptr;
    HWND list = nullptr;
    HWND empty = nullptr;
    HWND hint = nullptr;
    HWND buttons[6] = {};  // IDC_OPEN .. IDC_DELETE
    HFONT font = nullptr;
    UINT dpi = 96;

    std::wstring sessionsDir;
    std::wstring serverUrl;
    std::function<void()> onChanged;

    std::vector<Row> rows;  // in list order
    Signature signature;    // of the files rows was built from
    bool rebuilding = false;
};
Ui ui;

int Scale(int value) {
    return MulDiv(value, static_cast<int>(ui.dpi), 96);
}

bool IsRecordingNow(const Session& s) {
    return s.status == "recording";
}

bool AudioExists(const Session& s) {
    return !s.audioDeleted && GetFileAttributesW(s.audioPath.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// The server this recording lives on, for links. Recordings made for another
// server keep pointing at it.
std::wstring LinkBase(const Session& s) {
    std::wstring base = s.serverUrl && !s.serverUrl->empty() ? *s.serverUrl : ui.serverUrl;
    while (!base.empty() && base.back() == L'/') base.pop_back();
    return base;
}

// What the actions accept, judged on the sidecar alone. Also used on a fresh
// reload under the file lock, since the uploader may have moved it on.
bool StateAllowsUpload(const Session& s) {
    return !IsRecordingNow(s) &&
           (s.uploadState == UploadState::kLocalOnly || s.uploadState == UploadState::kHeld ||
            s.uploadState == UploadState::kRejected || s.uploadState == UploadState::kMissing);
}

bool StateAllowsKeep(const Session& s) {
    return !IsRecordingNow(s) && s.uploadAttemptUtc.empty() &&
           (s.uploadState == UploadState::kPending || s.uploadState == UploadState::kHeld);
}

bool InSpeakr(const Session& s) {
    return s.speakrId > 0 && s.uploadState != UploadState::kMissing;
}

bool CanOpen(const Session& s) {
    return !IsRecordingNow(s) && InSpeakr(s) && !LinkBase(s).empty();
}

// Seconds until an ISO UTC time; 0 if it's past or unreadable.
double SecondsUntil(const std::wstring& iso) {
    SYSTEMTIME then;
    if (iso.empty() || !ParseIsoUtc(iso, then)) return 0;
    FILETIME a, b;
    SystemTimeToFileTime(&then, &a);
    GetSystemTimeAsFileTime(&b);
    ULARGE_INTEGER ua{{a.dwLowDateTime, a.dwHighDateTime}};
    ULARGE_INTEGER ub{{b.dwLowDateTime, b.dwHighDateTime}};
    return ua.QuadPart > ub.QuadPart ? static_cast<double>(ua.QuadPart - ub.QuadPart) / 1e7 : 0;
}

std::wstring StatusText(const Session& s) {
    if (IsRecordingNow(s)) return L"Recording now";
    const std::string& state = s.uploadState;
    if (state == UploadState::kLocalOnly) return L"On this PC only";
    if (state == UploadState::kPending) {
        double wait = SecondsUntil(s.uploadAfterUtc);
        if (wait > 0) return L"Uploading in " + std::to_wstring(static_cast<int>(std::ceil(wait))) + L" s";
        return L"Waiting to upload";
    }
    if (state == UploadState::kUploaded) return L"In Speakr, being transcribed";
    if (state == UploadState::kDone) return L"Notes ready";
    if (state == UploadState::kFailed) return L"Speakr couldn't process it";
    if (state == UploadState::kRejected) return L"Speakr refused it";
    if (state == UploadState::kHeld) return L"Held: not sent to this server";
    if (state == UploadState::kMissing) return L"No longer in Speakr";
    if (state == UploadState::kStuck) return L"Not finished in Speakr";
    return FromUtf8(state);
}

std::wstring DetailsText(const Row& row) {
    const Session& s = row.session;
    if (IsRecordingNow(s)) return {};
    std::wstring details;
    auto add = [&](const std::wstring& text) {
        if (text.empty()) return;
        details += (details.empty() ? L"" : L"; ") + text;
    };
    if (s.status == "interrupted") add(L"Cut short by a crash");
    add(s.uploadError);
    if (s.audioDeleted) {
        if (s.uploadState == UploadState::kDone) add(L"Local audio deleted");
    } else if (!row.audioExists) {
        add(L"Audio file missing");
    }
    return details;
}

std::wstring LengthText(const Session& s) {
    return !IsRecordingNow(s) && s.durationSeconds > 0 ? FormatDuration(s.durationSeconds) : std::wstring();
}

// ---- Listing ---------------------------------------------------------------

Signature Scan() {
    Signature signature;
    for (const auto& path : Session::ListMetaFiles(ui.sessionsDir)) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        ULONGLONG written = 0;
        if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) {
            written = (static_cast<ULONGLONG>(data.ftLastWriteTime.dwHighDateTime) << 32) |
                      data.ftLastWriteTime.dwLowDateTime;
        }
        signature.emplace_back(path, written);
    }
    std::sort(signature.begin(), signature.end());
    return signature;
}

int SelectedIndex() {
    return ListView_GetNextItem(ui.list, -1, LVNI_SELECTED);
}

const Row* SelectedRow() {
    int index = SelectedIndex();
    return index >= 0 && static_cast<size_t>(index) < ui.rows.size() ? &ui.rows[index] : nullptr;
}

void UpdateButtons() {
    const Row* row = SelectedRow();
    const Session* s = row ? &row->session : nullptr;
    bool idle = s && !IsRecordingNow(*s);
    bool haveServer = !ui.serverUrl.empty();

    bool upload = false;
    std::wstring hint;
    if (s && StateAllowsUpload(*s)) {
        if (!haveServer) {
            hint = L"To upload this recording, set up your Speakr server in Settings.";
        } else if (!row->audioExists) {
            hint = L"The audio file is gone, so this can't be uploaded.";
        } else {
            upload = true;
        }
    }

    const bool enabled[6] = {
        s && CanOpen(*s),           // IDC_OPEN
        idle && row->audioExists,   // IDC_PLAY
        idle,                       // IDC_FOLDER
        upload,                     // IDC_UPLOAD
        s && StateAllowsKeep(*s),   // IDC_KEEP
        idle,                       // IDC_DELETE
    };

    // A disabled button can't hold the keyboard focus, so move it to the list.
    HWND focus = GetFocus();
    bool focusOnButton = std::find(std::begin(ui.buttons), std::end(ui.buttons), focus) != std::end(ui.buttons);
    for (int i = 0; i < 6; ++i) EnableWindow(ui.buttons[i], enabled[i]);
    if (focusOnButton && !IsWindowEnabled(focus)) SetFocus(ui.list);
    SetWindowTextW(ui.hint, hint.c_str());
}

void SetRowTexts(int index, const Row& row) {
    const Session& s = row.session;
    ListView_SetItemText(ui.list, index, kDate, const_cast<LPWSTR>(FormatFriendlyLocal(s.startedUtc).c_str()));
    // FormatFriendlyLocal's result is a temporary; the call above finishes with
    // it before the statement ends, so nothing dangles.
    ListView_SetItemText(ui.list, index, kLength, const_cast<LPWSTR>(LengthText(s).c_str()));
    ListView_SetItemText(ui.list, index, kTitle, const_cast<LPWSTR>(s.title.c_str()));
    ListView_SetItemText(ui.list, index, kStatus, const_cast<LPWSTR>(StatusText(s).c_str()));
    ListView_SetItemText(ui.list, index, kDetails, const_cast<LPWSTR>(DetailsText(row).c_str()));
}

void ShowEmptyState(bool empty) {
    ShowWindow(ui.empty, empty ? SW_SHOW : SW_HIDE);
    ShowWindow(ui.list, empty ? SW_HIDE : SW_SHOW);
}

// Rebuilds the list from disk if any sidecar appeared, went away or was
// rewritten (or if force is set). Keeps the selection and scroll position.
void Refresh(bool force) {
    if (!ui.window) return;
    Signature signature = Scan();
    if (!force && signature == ui.signature) {
        // Nothing on disk changed, but "Uploading in 45 s" counts down.
        for (size_t i = 0; i < ui.rows.size(); ++i) {
            if (ui.rows[i].session.uploadState == UploadState::kPending && !ui.rows[i].session.uploadAfterUtc.empty()) {
                SetRowTexts(static_cast<int>(i), ui.rows[i]);
            }
        }
        return;
    }

    std::vector<Row> rows;
    bool loadFailed = false;
    for (const auto& entry : signature) {
        auto session = Session::Load(entry.first);
        if (!session) {
            loadFailed = true;
            continue;
        }
        Row row;
        row.audioExists = AudioExists(*session);
        row.session = std::move(*session);
        rows.push_back(std::move(row));
    }
    // Stamps sort by date, so descending is newest first.
    std::sort(rows.begin(), rows.end(), [](const Row& a, const Row& b) { return a.session.id > b.session.id; });
    // A file caught mid-write (or unreadable) is retried on the next tick.
    ui.signature = loadFailed ? Signature() : std::move(signature);

    std::wstring selectedId;
    if (const Row* selected = SelectedRow()) selectedId = selected->session.id;
    int top = ListView_GetTopIndex(ui.list);

    ui.rebuilding = true;
    SendMessageW(ui.list, WM_SETREDRAW, FALSE, 0);
    ListView_DeleteAllItems(ui.list);
    ui.rows = std::move(rows);
    for (size_t i = 0; i < ui.rows.size(); ++i) {
        LVITEMW item{};
        item.mask = LVIF_TEXT;
        item.iItem = static_cast<int>(i);
        item.pszText = const_cast<LPWSTR>(L"");
        ListView_InsertItem(ui.list, &item);
        SetRowTexts(static_cast<int>(i), ui.rows[i]);
        if (!selectedId.empty() && ui.rows[i].session.id == selectedId) {
            ListView_SetItemState(ui.list, static_cast<int>(i), LVIS_SELECTED | LVIS_FOCUSED,
                                  LVIS_SELECTED | LVIS_FOCUSED);
        }
    }
    if (top > 0 && !ui.rows.empty()) {
        RECT bounds{};
        if (ListView_GetItemRect(ui.list, 0, &bounds, LVIR_BOUNDS)) {
            ListView_Scroll(ui.list, 0, top * (bounds.bottom - bounds.top));
        }
    }
    SendMessageW(ui.list, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(ui.list, nullptr, TRUE);
    ui.rebuilding = false;

    ShowEmptyState(ui.rows.empty());
    UpdateButtons();
}

// ---- Actions ---------------------------------------------------------------

void Tell(const std::wstring& text) {
    MessageBoxW(ui.window, text.c_str(), kAppName, MB_OK | MB_ICONINFORMATION);
}

bool Confirm(const std::wstring& text, UINT icon) {
    return MessageBoxW(ui.window, text.c_str(), kAppName, MB_YESNO | icon | MB_DEFBUTTON2) == IDYES;
}

// Reloads the sidecar under the file lock, re-checks it still qualifies (the
// uploader may have moved it on while a dialog was open) and applies the
// change. Then wakes the uploader and refreshes either way.
template <typename Allowed, typename Change>
void ChangeSession(const Session& shown, Allowed allowed, Change change) {
    std::wstring problem;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(Session::FileMutex());
        auto session = Session::Load(shown.metaPath);
        if (!session) {
            problem = L"That recording is no longer there.";
        } else if (!allowed(*session)) {
            problem = L"That recording changed in the meantime. Check its status and try again.";
        } else {
            change(*session);
            if (session->Save()) {
                changed = true;
            } else {
                problem = L"Couldn't save the change.";
            }
        }
    }
    if (!problem.empty()) Tell(problem);
    if (changed && ui.onChanged) ui.onChanged();
    Refresh(true);
}

void OpenInSpeakr(const Session& s) {
    if (!CanOpen(s)) return;
    std::wstring url = LinkBase(s) + L"/recordings/" + std::to_wstring(s.speakrId);
    ShellExecuteW(nullptr, L"open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void Play(const Session& s) {
    ShellExecuteW(nullptr, L"open", s.audioPath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ShowInFolder(const Row& row) {
    const std::wstring& path = row.audioExists ? row.session.audioPath : row.session.metaPath;
    std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void Upload(const Row& row) {
    const Session shown = row.session;
    std::wstring question = L"Upload this recording to " + ui.serverUrl + L"?";
    if (shown.uploadState == UploadState::kLocalOnly) question += L"\n\nIt was set to stay on this PC.";
    if (!Confirm(question, MB_ICONQUESTION) || !ui.window) return;

    const std::wstring server = ui.serverUrl;
    ChangeSession(
        shown, [](const Session& s) { return StateAllowsUpload(s) && AudioExists(s); },
        [&](Session& s) {
            if (s.uploadState == UploadState::kMissing) s.speakrId = 0;
            s.SetSensitive(false);  // also sets the state to pending
            s.serverUrl = server;
            s.uploadError.clear();
            s.uploadAttemptUtc.clear();
            s.uploadAfterUtc.clear();
        });
}

void KeepOnThisPc(const Row& row) {
    ChangeSession(row.session, StateAllowsKeep, [](Session& s) { s.SetSensitive(true); });
}

void DeleteRecording(const Row& row) {
    const Session shown = row.session;
    std::wstring question = L"Delete this recording?\n\n";
    question += InSpeakr(shown) ? L"This deletes the audio and details from this PC. The copy in Speakr stays."
                                : L"This deletes the audio and details from this PC. It can't be undone.";
    if (!Confirm(question, MB_ICONWARNING) || !ui.window) return;

    ChangeSession(
        shown, [](const Session& s) { return !IsRecordingNow(s); }, [](Session& s) { s.Discard(); });
}

// The Save in ChangeSession would rewrite the sidecar Discard just deleted, so
// deleting has its own path.
void DeleteUnderLock(const Row& row) {
    const Session shown = row.session;
    std::wstring question = L"Delete this recording?\n\n";
    question += InSpeakr(shown) ? L"This deletes the audio and details from this PC. The copy in Speakr stays."
                                : L"This deletes the audio and details from this PC. It can't be undone.";
    if (!Confirm(question, MB_ICONWARNING) || !ui.window) return;

    std::wstring problem;
    bool deleted = false;
    {
        std::lock_guard<std::mutex> lock(Session::FileMutex());
        auto session = Session::Load(shown.metaPath);
        if (!session) {
            problem = L"That recording is no longer there.";
        } else if (IsRecordingNow(*session)) {
            problem = L"That recording has started again. Stop it first.";
        } else {
            session->Discard();
            deleted = true;
        }
    }
    if (!problem.empty()) Tell(problem);
    if (deleted && ui.onChanged) ui.onChanged();
    Refresh(true);
}

// Double-click or Enter on a row.
void DefaultAction() {
    const Row* row = SelectedRow();
    if (!row || IsRecordingNow(row->session)) return;
    if (CanOpen(row->session)) return OpenInSpeakr(row->session);
    if (row->audioExists) Play(row->session);
}

void OnCommand(int id) {
    // Copy: an action's dialog lets the timer rebuild ui.rows under us.
    const Row* selected = SelectedRow();
    if (!selected && id != IDOK && id != IDCANCEL) return;
    Row row = selected ? *selected : Row();
    switch (id) {
        case IDC_OPEN: OpenInSpeakr(row.session); break;
        case IDC_PLAY: Play(row.session); break;
        case IDC_FOLDER: ShowInFolder(row); break;
        case IDC_UPLOAD: Upload(row); break;
        case IDC_KEEP: KeepOnThisPc(row); break;
        case IDC_DELETE: DeleteUnderLock(row); break;
        case IDOK:
            if (GetFocus() == ui.list) DefaultAction();
            break;
        case IDCANCEL: DestroyWindow(ui.window); break;
    }
}

// ---- Window ----------------------------------------------------------------

void MakeFont() {
    if (ui.font) DeleteObject(ui.font);
    ui.font = nullptr;
    NONCLIENTMETRICSW metrics{};
    metrics.cbSize = sizeof(metrics);
    if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(metrics), &metrics, 0, ui.dpi)) {
        ui.font = CreateFontIndirectW(&metrics.lfMessageFont);
    }
    HWND controls[] = {ui.list, ui.empty, ui.hint};
    for (HWND control : controls) SendMessageW(control, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
    for (HWND button : ui.buttons) SendMessageW(button, WM_SETFONT, reinterpret_cast<WPARAM>(ui.font), TRUE);
}

void Layout() {
    if (!ui.window) return;
    RECT client;
    GetClientRect(ui.window, &client);
    const int width = client.right;
    const int height = client.bottom;
    const int margin = Scale(12);
    const int gap = Scale(8);
    const int buttonWidth = Scale(120);
    const int buttonHeight = Scale(28);
    const int hintHeight = Scale(20);

    const int buttonsTop = height - margin - buttonHeight;
    const int hintTop = buttonsTop - Scale(6) - hintHeight;
    const int listHeight = std::max(0, hintTop - Scale(4) - margin);
    const int listWidth = std::max(0, width - 2 * margin);

    MoveWindow(ui.list, margin, margin, listWidth, listHeight, TRUE);
    MoveWindow(ui.empty, margin, margin, listWidth, listHeight, TRUE);
    MoveWindow(ui.hint, margin, hintTop, listWidth, hintHeight, TRUE);

    // Open, Play, Show in folder, Upload and Keep run from the left; Delete,
    // which can't be undone, sits alone at the right.
    for (int i = 0; i < 5; ++i) {
        MoveWindow(ui.buttons[i], margin + i * (buttonWidth + gap), buttonsTop, buttonWidth, buttonHeight, TRUE);
    }
    MoveWindow(ui.buttons[5], width - margin - buttonWidth, buttonsTop, buttonWidth, buttonHeight, TRUE);

    // Details takes whatever the fixed columns leave.
    ListView_SetColumnWidth(ui.list, kDate, Scale(150));
    ListView_SetColumnWidth(ui.list, kLength, Scale(70));
    ListView_SetColumnWidth(ui.list, kTitle, Scale(200));
    ListView_SetColumnWidth(ui.list, kStatus, Scale(210));
    RECT listClient;
    GetClientRect(ui.list, &listClient);
    int fixed = Scale(150 + 70 + 200 + 210);
    ListView_SetColumnWidth(ui.list, kDetails, std::max(Scale(150), static_cast<int>(listClient.right) - fixed));
}

bool Create(HWND window) {
    ui.window = window;
    ui.dpi = GetDpiForWindow(window);

    ui.list = CreateWindowExW(WS_EX_CLIENTEDGE, WC_LISTVIEWW, L"",
                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | LVS_REPORT | LVS_SINGLESEL | LVS_SHOWSELALWAYS,
                              0, 0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_LIST)),
                              ui.instance, nullptr);
    ui.empty = CreateWindowExW(0, L"STATIC", L"No recordings yet. Press Ctrl+Alt+R to start one.",
                               WS_CHILD | SS_CENTER | SS_CENTERIMAGE, 0, 0, 0, 0, window,
                               reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_EMPTY)), ui.instance, nullptr);
    ui.hint = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_LEFT | SS_ENDELLIPSIS, 0, 0, 0, 0, window,
                              reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_HINT)), ui.instance, nullptr);
    if (!ui.list || !ui.empty || !ui.hint) return false;

    ListView_SetExtendedListViewStyle(ui.list, LVS_EX_FULLROWSELECT | LVS_EX_DOUBLEBUFFER | LVS_EX_LABELTIP);
    const wchar_t* headings[kColumnCount] = {L"Date", L"Length", L"Title", L"Status", L"Details"};
    for (int i = 0; i < kColumnCount; ++i) {
        LVCOLUMNW column{};
        column.mask = LVCF_TEXT | LVCF_FMT;
        column.fmt = i == kLength ? LVCFMT_RIGHT : LVCFMT_LEFT;
        column.pszText = const_cast<LPWSTR>(headings[i]);
        ListView_InsertColumn(ui.list, i, &column);
    }

    const wchar_t* labels[6] = {L"Open in Speakr", L"Play", L"Show in folder", L"Upload", L"Keep on this PC", L"Delete"};
    for (int i = 0; i < 6; ++i) {
        ui.buttons[i] = CreateWindowExW(0, L"BUTTON", labels[i], WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_PUSHBUTTON, 0,
                                        0, 0, 0, window, reinterpret_cast<HMENU>(static_cast<INT_PTR>(IDC_OPEN + i)),
                                        ui.instance, nullptr);
        if (!ui.buttons[i]) return false;
        EnableWindow(ui.buttons[i], FALSE);
    }

    MakeFont();
    Layout();
    Refresh(true);
    SetTimer(window, kRefreshTimer, kRefreshMs, nullptr);
    return true;
}

LRESULT CALLBACK WindowProc(HWND window, UINT message, WPARAM wParam, LPARAM lParam) {
    switch (message) {
        case WM_CREATE:
            return Create(window) ? 0 : -1;
        case WM_SIZE:
            Layout();
            return 0;
        case WM_GETMINMAXINFO: {
            // Enough width for the six buttons in a row.
            RECT frame{0, 0, Scale(800), Scale(330)};
            AdjustWindowRectExForDpi(&frame, WS_OVERLAPPEDWINDOW, FALSE, WS_EX_CONTROLPARENT, ui.dpi);
            auto* info = reinterpret_cast<MINMAXINFO*>(lParam);
            info->ptMinTrackSize.x = frame.right - frame.left;
            info->ptMinTrackSize.y = frame.bottom - frame.top;
            return 0;
        }
        case WM_DPICHANGED: {
            ui.dpi = HIWORD(wParam);
            MakeFont();
            const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
            SetWindowPos(window, nullptr, suggested->left, suggested->top, suggested->right - suggested->left,
                         suggested->bottom - suggested->top, SWP_NOZORDER | SWP_NOACTIVATE);
            Layout();
            return 0;
        }
        case WM_SETFOCUS:
            SetFocus(ui.list);
            return 0;
        case WM_TIMER:
            // No point reading files nobody can see.
            if (wParam == kRefreshTimer && IsWindowVisible(window) && !IsIconic(window)) Refresh(false);
            return 0;
        case WM_COMMAND:
            if (HIWORD(wParam) == BN_CLICKED || LOWORD(wParam) == IDOK || LOWORD(wParam) == IDCANCEL) {
                OnCommand(LOWORD(wParam));
            }
            return 0;
        case WM_NOTIFY: {
            auto* header = reinterpret_cast<NMHDR*>(lParam);
            if (header->idFrom != IDC_LIST) break;
            switch (header->code) {
                case LVN_ITEMCHANGED: {
                    auto* changed = reinterpret_cast<NMLISTVIEW*>(lParam);
                    if (!ui.rebuilding && (changed->uChanged & LVIF_STATE)) UpdateButtons();
                    break;
                }
                case NM_DBLCLK:
                    if (reinterpret_cast<NMITEMACTIVATE*>(lParam)->iItem >= 0) DefaultAction();
                    break;
                case NM_RETURN:
                    DefaultAction();
                    break;
            }
            return 0;
        }
        case WM_CLOSE:
            DestroyWindow(window);
            return 0;
        case WM_DESTROY:
            KillTimer(window, kRefreshTimer);
            return 0;
        case WM_NCDESTROY: {
            // Children are gone by now, so the font is no longer in use.
            if (ui.font) DeleteObject(ui.font);
            ui = Ui();
            break;
        }
    }
    return DefWindowProcW(window, message, wParam, lParam);
}

}  // namespace

void ShowHistoryWindow(HINSTANCE instance, const std::wstring& sessionsDir, const std::wstring& serverUrl,
                       std::function<void()> onChanged) {
    if (ui.window) {
        // Already open: pick up the current settings and bring it forward.
        ui.serverUrl = serverUrl;
        ui.onChanged = std::move(onChanged);
        if (IsIconic(ui.window)) ShowWindow(ui.window, SW_RESTORE);
        SetForegroundWindow(ui.window);
        Refresh(true);
        return;
    }

    INITCOMMONCONTROLSEX controls{sizeof(controls), ICC_LISTVIEW_CLASSES};
    InitCommonControlsEx(&controls);

    WNDCLASSEXW existing{};
    if (!GetClassInfoExW(instance, kClassName, &existing)) {
        WNDCLASSEXW wc{};
        wc.cbSize = sizeof(wc);
        wc.lpfnWndProc = WindowProc;
        wc.hInstance = instance;
        wc.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_APP));
        wc.hCursor = LoadCursorW(nullptr, IDC_ARROW);
        wc.hbrBackground = reinterpret_cast<HBRUSH>(COLOR_WINDOW + 1);
        wc.lpszClassName = kClassName;
        if (!RegisterClassExW(&wc)) return;
    }

    ui = Ui();
    ui.instance = instance;
    ui.sessionsDir = sessionsDir;
    ui.serverUrl = serverUrl;
    ui.onChanged = std::move(onChanged);
    ui.dpi = GetDpiForSystem();

    // About 900x480 logical pixels of client area, centred on the primary
    // monitor's work area.
    const DWORD style = WS_OVERLAPPEDWINDOW;
    RECT frame{0, 0, Scale(900), Scale(480)};
    AdjustWindowRectExForDpi(&frame, style, FALSE, WS_EX_CONTROLPARENT, ui.dpi);
    RECT work{};
    SystemParametersInfoW(SPI_GETWORKAREA, 0, &work, 0);
    const int width = std::min<int>(frame.right - frame.left, work.right - work.left);
    const int height = std::min<int>(frame.bottom - frame.top, work.bottom - work.top);
    const int x = work.left + (work.right - work.left - width) / 2;
    const int y = work.top + (work.bottom - work.top - height) / 2;

    HWND window = CreateWindowExW(WS_EX_CONTROLPARENT, kClassName, L"MeetingRecorder recordings", style, x, y, width,
                                  height, nullptr, nullptr, instance, nullptr);
    if (!window) {
        ui = Ui();
        return;
    }
    ShowWindow(window, SW_SHOW);
    SetForegroundWindow(window);
}

bool IsHistoryWindowMessage(MSG* message) {
    return ui.window && IsDialogMessageW(ui.window, message);
}
