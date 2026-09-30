#include "Migration.h"

#include "Autostart.h"
#include "Config.h"
#include "Util.h"

#include <windows.h>
#include <shlobj.h>
#include <wincred.h>

namespace {

constexpr wchar_t kOldName[] = L"CallRecorder";
constexpr wchar_t kNewName[] = L"MeetingRecorder";

bool Exists(const std::wstring& path) {
    return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES;
}

// Renames <known folder>\CallRecorder to <known folder>\MeetingRecorder.
// Returns the old path if it's still there afterwards and nothing replaced it.
std::wstring MoveFolder(REFKNOWNFOLDERID folder) {
    PWSTR base = nullptr;
    std::wstring root;
    if (SUCCEEDED(SHGetKnownFolderPath(folder, 0, nullptr, &base))) root = base;
    CoTaskMemFree(base);
    if (root.empty()) return {};
    std::wstring from = root + L"\\" + kOldName;
    std::wstring to = root + L"\\" + kNewName;
    if (!Exists(from) || Exists(to)) return {};
    return MoveFileExW(from.c_str(), to.c_str(), 0) ? std::wstring() : from;
}

void MoveToken() {
    std::wstring oldTarget = std::wstring(kOldName) + L":Speakr";
    PCREDENTIALW credential = nullptr;
    if (!CredReadW(oldTarget.c_str(), CRED_TYPE_GENERIC, 0, &credential)) return;
    std::wstring token(reinterpret_cast<const wchar_t*>(credential->CredentialBlob),
                       credential->CredentialBlobSize / sizeof(wchar_t));
    CredFree(credential);
    bool kept = !ReadSpeakrToken().empty() || WriteSpeakrToken(ToUtf8(token));
    SecureZeroMemory(token.data(), token.size() * sizeof(wchar_t));
    if (kept) CredDeleteW(oldTarget.c_str(), CRED_TYPE_GENERIC, 0);
}

}  // namespace

namespace Migration {

bool OldAppRunning() {
    return FindWindowW(L"CallRecorderWindow", nullptr) != nullptr;
}

std::wstring FromCallRecorder() {
    std::wstring failed;
    for (REFKNOWNFOLDERID folder : {FOLDERID_RoamingAppData, FOLDERID_LocalAppData}) {
        std::wstring stuck = MoveFolder(folder);
        if (!stuck.empty()) failed += (failed.empty() ? L"" : L"\n") + stuck;
    }
    MoveToken();
    Autostart::AdoptEntry(kOldName);
    return failed;
}

}  // namespace Migration
