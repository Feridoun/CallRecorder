#include "CallDetector.h"

#include "CallLogic.h"

#include <windows.h>
#include <audiopolicy.h>
#include <mmdeviceapi.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>

using Microsoft::WRL::ComPtr;

namespace {

// Processes with a running capture stream on any connected microphone: the
// same sessions the Volume Mixer lists. Apps keep the stream running while
// muted, so muting doesn't look like the call ending.
std::vector<DWORD> CapturingProcesses() {
    std::vector<DWORD> pids;
    ComPtr<IMMDeviceEnumerator> enumerator;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator)))) {
        return pids;
    }
    ComPtr<IMMDeviceCollection> devices;
    if (FAILED(enumerator->EnumAudioEndpoints(eCapture, DEVICE_STATE_ACTIVE, &devices))) return pids;
    UINT deviceCount = 0;
    devices->GetCount(&deviceCount);
    DWORD self = GetCurrentProcessId();
    for (UINT d = 0; d < deviceCount; ++d) {
        ComPtr<IMMDevice> device;
        ComPtr<IAudioSessionManager2> manager;
        ComPtr<IAudioSessionEnumerator> sessions;
        if (FAILED(devices->Item(d, &device)) ||
            FAILED(device->Activate(__uuidof(IAudioSessionManager2), CLSCTX_ALL, nullptr, &manager)) ||
            FAILED(manager->GetSessionEnumerator(&sessions))) {
            continue;
        }
        int sessionCount = 0;
        sessions->GetCount(&sessionCount);
        for (int s = 0; s < sessionCount; ++s) {
            ComPtr<IAudioSessionControl> control;
            ComPtr<IAudioSessionControl2> control2;
            AudioSessionState state = AudioSessionStateInactive;
            DWORD pid = 0;
            if (FAILED(sessions->GetSession(s, &control)) || FAILED(control.As(&control2)) ||
                FAILED(control2->GetState(&state)) || state != AudioSessionStateActive ||
                FAILED(control2->GetProcessId(&pid))) {
                continue;
            }
            if (pid != 0 && pid != self && std::find(pids.begin(), pids.end(), pid) == pids.end()) pids.push_back(pid);
        }
    }
    return pids;
}

std::vector<CallLogic::Process> Processes() {
    std::vector<CallLogic::Process> processes;
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return processes;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof entry;
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        processes.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    }
    CloseHandle(snapshot);
    return processes;
}

struct VisibleWindow {
    std::wstring exe;
    std::wstring title;
};

std::vector<VisibleWindow> VisibleWindows(const std::vector<CallLogic::Process>& processes) {
    struct Context {
        const std::vector<CallLogic::Process>& processes;
        std::vector<VisibleWindow> windows;
    } context{processes, {}};
    EnumWindows(
        [](HWND window, LPARAM param) -> BOOL {
            auto& context = *reinterpret_cast<Context*>(param);
            if (!IsWindowVisible(window)) return TRUE;
            int length = GetWindowTextLengthW(window);
            if (length <= 0) return TRUE;
            std::wstring title(length + 1, L'\0');
            title.resize(GetWindowTextW(window, title.data(), length + 1));
            DWORD pid = 0;
            GetWindowThreadProcessId(window, &pid);
            auto process = std::find_if(context.processes.begin(), context.processes.end(),
                                        [&](const CallLogic::Process& p) { return p.pid == pid; });
            if (process != context.processes.end()) context.windows.push_back({process->exe, std::move(title)});
            return TRUE;
        },
        reinterpret_cast<LPARAM>(&context));
    return context.windows;
}

}  // namespace

std::vector<DetectedCall> DetectCalls() {
    std::vector<DWORD> capturing = CapturingProcesses();
    if (capturing.empty()) return {};
    std::vector<CallLogic::Process> processes = Processes();

    std::vector<DetectedCall> calls;
    std::vector<std::wstring> browsers;
    auto callFor = [&](std::wstring_view app) -> DetectedCall& {
        auto found = std::find_if(calls.begin(), calls.end(), [&](const DetectedCall& c) { return c.app == app; });
        if (found != calls.end()) return *found;
        return calls.emplace_back(DetectedCall{std::wstring(app), {}});
    };
    for (DWORD pid : capturing) {
        CallLogic::MicUser user = CallLogic::MicUserOf(pid, processes);
        if (user.app) callFor(user.app->name);
        if (!user.browser.empty()) browsers.push_back(user.browser);
    }
    if (calls.empty() && browsers.empty()) return calls;

    for (const auto& window : VisibleWindows(processes)) {
        if (std::any_of(browsers.begin(), browsers.end(),
                        [&](const std::wstring& b) { return CallLogic::SameText(b, window.exe); })) {
            CallLogic::WebCall web = CallLogic::WebCallFromTitle(window.title);
            if (!web.found) continue;
            DetectedCall& call = callFor(web.app);
            if (call.subject.empty()) call.subject = web.subject;
            continue;
        }
        const CallLogic::CallApp* app = CallLogic::FindByExe(window.exe);
        if (!app || !app->teamsTitles) continue;
        auto call = std::find_if(calls.begin(), calls.end(), [&](const DetectedCall& c) { return c.app == app->name; });
        if (call != calls.end() && call->subject.empty()) call->subject = CallLogic::TeamsSubject(window.title);
    }
    return calls;
}
