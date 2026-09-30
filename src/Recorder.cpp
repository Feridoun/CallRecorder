#include "Recorder.h"

#include "OpusFileWriter.h"

#include <audioclient.h>
#include <audioclientactivationparams.h>
#include <avrt.h>
#include <mmdeviceapi.h>
// After mmdeviceapi.h, which defines DEFINE_PROPERTYKEY.
#include <functiondiscoverykeys_devpkey.h>
#include <tlhelp32.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <deque>
#include <map>
#include <memory>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kRate = OpusFileWriter::kSampleRate;
constexpr int kBitrate = 24000;             // ~11 MB per hour, plenty for speech
constexpr int kStereoBitrate = 32000;       // two channels kept apart
constexpr int64_t kLatency = kRate / 10;    // mix 100 ms behind real time
constexpr size_t kMaxBacklog = kRate * 3 / 10;  // drop audio older than this
constexpr int64_t kMaxCatchUp = kRate * 2;  // larger gaps (sleep/hibernate) are skipped
constexpr REFERENCE_TIME kBufferDuration = 10'000'000;  // 1 s WASAPI buffer
constexpr ULONGLONG kRetryIntervalMs = 2000;
constexpr ULONGLONG kAppScanIntervalMs = 2000;
constexpr float kSilentPeak = 0.001f;  // -60 dBFS
constexpr ULONGLONG kMicSilenceWarnMs = 60'000;

// Sets flags when the default audio devices change (headset plugged in,
// Bluetooth connects, ...), so capture can move to the new device, and when a
// device becomes available, so capture can return to the one the user chose.
class DeviceWatcher final : public IMMNotificationClient {
public:
    DeviceWatcher(std::atomic<bool>* defaultChanged, std::atomic<bool>* deviceArrived)
        : defaultChanged_(defaultChanged), deviceArrived_(deviceArrived) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IMMNotificationClient)) {
            *object = static_cast<IMMNotificationClient*>(this);
            AddRef();
            return S_OK;
        }
        *object = nullptr;
        return E_NOINTERFACE;
    }

    HRESULT STDMETHODCALLTYPE OnDefaultDeviceChanged(EDataFlow, ERole, LPCWSTR) override {
        *defaultChanged_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD state) override {
        if (state == DEVICE_STATE_ACTIVE) *deviceArrived_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override {
        *deviceArrived_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG refs_ = 1;
    std::atomic<bool>* defaultChanged_;
    std::atomic<bool>* deviceArrived_;
};

std::wstring FriendlyName(IMMDevice* device) {
    ComPtr<IPropertyStore> properties;
    if (FAILED(device->OpenPropertyStore(STGM_READ, &properties))) return {};
    PROPVARIANT value;
    PropVariantInit(&value);
    std::wstring name;
    if (SUCCEEDED(properties->GetValue(PKEY_Device_FriendlyName, &value)) && value.vt == VT_LPWSTR) {
        name = value.pwszVal;
    }
    PropVariantClear(&value);
    return name;
}

// Completes ActivateAudioInterfaceAsync. The activation runs on a Windows
// worker thread, so this must be agile (IAgileObject), and it owns its event
// and result so a caller that gave up waiting can't leave it dangling.
class ActivationHandler final : public IActivateAudioInterfaceCompletionHandler, public IAgileObject {
public:
    ActivationHandler() : done_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

    ULONG STDMETHODCALLTYPE AddRef() override { return InterlockedIncrement(&refs_); }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = InterlockedDecrement(&refs_);
        if (refs == 0) delete this;
        return refs;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override {
        if (iid == __uuidof(IUnknown) || iid == __uuidof(IActivateAudioInterfaceCompletionHandler)) {
            *object = static_cast<IActivateAudioInterfaceCompletionHandler*>(this);
        } else if (iid == __uuidof(IAgileObject)) {
            *object = static_cast<IAgileObject*>(this);
        } else {
            *object = nullptr;
            return E_NOINTERFACE;
        }
        AddRef();
        return S_OK;
    }

    HRESULT STDMETHODCALLTYPE ActivateCompleted(IActivateAudioInterfaceAsyncOperation* operation) override {
        HRESULT activation = E_FAIL;
        ComPtr<IUnknown> unknown;
        HRESULT hr = operation->GetActivateResult(&activation, &unknown);
        result_ = FAILED(hr) ? hr : activation;
        if (SUCCEEDED(result_)) result_ = unknown ? unknown.As(&client_) : E_POINTER;
        SetEvent(done_);
        return S_OK;
    }

    HANDLE Done() const { return done_; }
    // Valid once Done() is signaled.
    HRESULT Result() const { return result_; }
    ComPtr<IAudioClient> TakeClient() { return std::move(client_); }

private:
    ~ActivationHandler() { CloseHandle(done_); }

    LONG refs_ = 1;
    HANDLE done_;
    HRESULT result_ = E_FAIL;
    ComPtr<IAudioClient> client_;
};

// Activates an IAudioClient that loops back the audio of `pid` and its child
// processes. Gives up early if `cancel` is signaled.
HRESULT ActivateProcessLoopback(DWORD pid, HANDLE cancel, ComPtr<IAudioClient>& client) {
    AUDIOCLIENT_ACTIVATION_PARAMS params{};
    params.ActivationType = AUDIOCLIENT_ACTIVATION_TYPE_PROCESS_LOOPBACK;
    params.ProcessLoopbackParams.TargetProcessId = pid;
    params.ProcessLoopbackParams.ProcessLoopbackMode = PROCESS_LOOPBACK_MODE_INCLUDE_TARGET_PROCESS_TREE;
    PROPVARIANT activation{};
    activation.vt = VT_BLOB;
    activation.blob.cbSize = sizeof(params);
    activation.blob.pBlobData = reinterpret_cast<BYTE*>(&params);

    ComPtr<ActivationHandler> handler;
    handler.Attach(new ActivationHandler);
    ComPtr<IActivateAudioInterfaceAsyncOperation> operation;
    HRESULT hr = ActivateAudioInterfaceAsync(VIRTUAL_AUDIO_DEVICE_PROCESS_LOOPBACK, __uuidof(IAudioClient),
                                             &activation, handler.Get(), &operation);
    if (FAILED(hr)) return hr;
    HANDLE waitFor[] = {handler->Done(), cancel};
    if (WaitForMultipleObjects(2, waitFor, FALSE, 3000) != WAIT_OBJECT_0) return HRESULT_FROM_WIN32(ERROR_TIMEOUT);
    if (FAILED(handler->Result())) return handler->Result();
    client = handler->TakeClient();
    return S_OK;
}

// One WASAPI capture stream, converted by Windows to 48 kHz mono float, plus
// the samples captured but not yet mixed.
class CaptureSource {
public:
    CaptureSource(const wchar_t* name, EDataFlow flow, ERole role, bool loopback)
        : name_(name), flow_(flow), role_(role), loopback_(loopback) {}
    ~CaptureSource() { Close(); }

    const wchar_t* Name() const { return name_; }
    bool IsOpen() const { return capture_ != nullptr; }
    // Why the last open attempt failed (S_OK if it didn't).
    HRESULT LastError() const { return lastError_; }

    // An empty ID follows the Windows default. Takes effect on the next Open().
    // Returns whether the choice changed.
    bool Choose(const std::wstring& id) { return std::exchange(chosenId_, id) != id; }
    // Microphone only: open as a communications stream. Takes effect on the
    // next Open(); returns whether the setting changed.
    bool SetCommunications(bool on) { return std::exchange(communications_, on) != on; }
    // A device was chosen but isn't the one open (or nothing is open).
    bool FellBack() const { return !chosenId_.empty() && openedId_ != chosenId_; }
    bool FollowsDefault() const { return chosenId_.empty() || FellBack(); }

    // Opens the chosen device, or the default one if it isn't available.
    bool Open(IMMDeviceEnumerator* enumerator) {
        Close();
        lastError_ = S_OK;
        ComPtr<IMMDevice> device;
        DWORD state = 0;
        if (!chosenId_.empty() && SUCCEEDED(enumerator->GetDevice(chosenId_.c_str(), &device)) &&
            SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE) {
            lastError_ = OpenDevice(device.Get());
            if (SUCCEEDED(lastError_)) return true;
        }
        device.Reset();
        HRESULT hr = enumerator->GetDefaultAudioEndpoint(flow_, role_, &device);
        if (SUCCEEDED(hr)) hr = OpenDevice(device.Get());
        if (SUCCEEDED(hr)) {
            lastError_ = S_OK;
            return true;
        }
        // A blocked chosen device is the more useful thing to report.
        if (lastError_ != E_ACCESSDENIED) lastError_ = hr;
        return false;
    }

    // Captures what process `pid` (and its children) play, whatever device
    // they play on. Windows can't report a mix format for such a stream, so
    // ours is stereo float, averaged down to mono in Drain().
    HRESULT OpenProcessLoopback(DWORD pid, HANDLE cancel) {
        Close();
        channels_ = 2;
        // A stream may refuse the long buffer, so also try Windows' default.
        for (REFERENCE_TIME duration : {kBufferDuration, REFERENCE_TIME{0}}) {
            lastError_ = ActivateProcessLoopback(pid, cancel, client_);
            if (FAILED(lastError_)) break;
            lastError_ = Begin(duration, true);
            if (SUCCEEDED(lastError_)) return S_OK;
            client_.Reset();
            if (event_) CloseHandle(std::exchange(event_, nullptr));
        }
        return lastError_;
    }

    void Close() {
        if (client_) client_->Stop();
        capture_.Reset();
        client_.Reset();
        if (event_) CloseHandle(std::exchange(event_, nullptr));
        openedId_.clear();
        DiscardPending();
        peak_ = 0.0f;
    }

    // Moves everything WASAPI has captured into pending_. Returns false if the
    // device has gone away (unplugged, disabled), after closing the stream.
    bool Drain() {
        if (!capture_) return true;
        UINT32 packetFrames = 0;
        HRESULT hr;
        while (SUCCEEDED(hr = capture_->GetNextPacketSize(&packetFrames)) && packetFrames > 0) {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD bufferFlags = 0;
            hr = capture_->GetBuffer(&data, &frames, &bufferFlags, nullptr, nullptr);
            if (FAILED(hr)) break;
            if (bufferFlags & AUDCLNT_BUFFERFLAGS_SILENT) {
                pending_.insert(pending_.end(), frames, 0.0f);
            } else {
                auto* samples = reinterpret_cast<const float*>(data);
                if (channels_ == 1) {
                    pending_.insert(pending_.end(), samples, samples + frames);
                    for (UINT32 i = 0; i < frames; ++i) peak_ = std::max(peak_, std::fabs(samples[i]));
                } else {
                    for (UINT32 i = 0; i < frames; ++i) {
                        float mono = 0.5f * (samples[2 * i] + samples[2 * i + 1]);
                        pending_.push_back(mono);
                        peak_ = std::max(peak_, std::fabs(mono));
                    }
                }
            }
            capture_->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) {
            Close();
            return false;
        }
        return true;
    }

    // The loudest sample drained since the last call.
    float TakePeak() { return std::exchange(peak_, 0.0f); }

    // Adds up to `count` samples into `mix`. Loopback delivers nothing while
    // nothing is playing, so after running dry a source waits until it has
    // built up kLatency of audio again; otherwise every 10 ms tick would
    // underrun and chop the audio into fragments.
    //
    // The mixer runs on the QPC clock and each device on its own, so the
    // backlog slowly grows or shrinks. While it stays off target, one sample
    // is dropped or repeated now and then, far too rarely to hear, instead of
    // waiting for the hard limits (kMaxBacklog, running dry) to cut or gap
    // the audio.
    void MixInto(std::vector<float>& mix, size_t count) {
        if (pending_.size() > count + kMaxBacklog) {
            pending_.erase(pending_.begin(), pending_.end() - static_cast<ptrdiff_t>(count + kMaxBacklog));
        }
        if (!primed_) {
            if (pending_.size() < static_cast<size_t>(kLatency)) return;
            primed_ = true;
            backlog_ = -1.0;
            adjustCredit_ = 0;
        }
        size_t take = std::min(count, pending_.size());
        size_t consume = take;
        adjustCredit_ = std::min(adjustCredit_ + count, kAdjustInterval);
        if (take == count && take > 0 && adjustCredit_ >= kAdjustInterval) {
            if (backlog_ > kLatency + kDriftThreshold && pending_.size() > take) {
                consume = take + 1;  // running ahead of the clock: skip a sample
                adjustCredit_ = 0;
            } else if (backlog_ >= 0.0 && backlog_ < kLatency - kDriftThreshold) {
                consume = take - 1;  // falling behind: play the last sample twice
                adjustCredit_ = 0;
            }
        }
        for (size_t i = 0; i < take; ++i) mix[i] += pending_[i];
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(consume));
        if (pending_.empty()) {
            primed_ = false;
            return;
        }
        // What is left after mixing is the latency this source runs at.
        // Smooth it (about a second) so delivery jitter doesn't trigger fixes.
        double left = static_cast<double>(pending_.size());
        backlog_ = backlog_ < 0.0 ? left : backlog_ + (left - backlog_) / 128.0;
    }

    void DiscardPending() {
        pending_.clear();
        primed_ = false;
    }

private:
    HRESULT OpenDevice(IMMDevice* device) {
        channels_ = 1;
        HRESULT hr = device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_);
        if (FAILED(hr)) return hr;

        if (!loopback_ && communications_) {
            // Tell Windows this is a call. Devices that offer echo cancellation
            // or noise suppression for communications then apply it, which
            // stops the microphone re-recording the other side when the user
            // is on speakers. Failure only means no such processing.
            ComPtr<IAudioClient2> client2;
            if (SUCCEEDED(client_.As(&client2))) {
                AudioClientProperties properties{};
                properties.cbSize = sizeof(properties);
                properties.eCategory = AudioCategory_Communications;
                client2->SetClientProperties(&properties);
            }
        }

        hr = Begin(kBufferDuration, false);
        if (FAILED(hr)) {
            Close();
            return hr;
        }
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) openedId_ = id;
        CoTaskMemFree(id);
        return S_OK;
    }

    // Initializes and starts client_. Process-loopback streams must be event
    // driven, though we poll them like the others.
    HRESULT Begin(REFERENCE_TIME duration, bool processLoopback) {
        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = static_cast<WORD>(channels_);
        format.nSamplesPerSec = kRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = static_cast<WORD>(channels_ * sizeof(float));
        format.nAvgBytesPerSec = kRate * format.nBlockAlign;

        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;
        if (processLoopback) {
            flags |= AUDCLNT_STREAMFLAGS_EVENTCALLBACK;
            event_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            if (!event_) return HRESULT_FROM_WIN32(GetLastError());
        }

        ComPtr<IAudioCaptureClient> capture;
        HRESULT hr = client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, duration, 0, &format, nullptr);
        if (SUCCEEDED(hr) && processLoopback) hr = client_->SetEventHandle(event_);
        if (SUCCEEDED(hr)) hr = client_->GetService(IID_PPV_ARGS(&capture));
        if (SUCCEEDED(hr)) hr = client_->Start();
        if (SUCCEEDED(hr)) capture_ = capture;
        return hr;
    }

    static constexpr double kDriftThreshold = kRate / 100.0;  // 10 ms
    static constexpr size_t kAdjustInterval = 1000;           // at most one fix per 1000 samples

    const wchar_t* name_;
    EDataFlow flow_;
    ERole role_;
    bool loopback_;
    std::wstring chosenId_;
    bool communications_ = false;
    std::wstring openedId_;
    HRESULT lastError_ = S_OK;
    UINT32 channels_ = 1;
    HANDLE event_ = nullptr;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    std::vector<float> pending_;
    bool primed_ = false;
    double backlog_ = -1.0;  // smoothed samples left after mixing; < 0 until measured
    size_t adjustCredit_ = 0;
    float peak_ = 0.0f;
};

struct ProcessInfo {
    DWORD pid;
    DWORD parent;
    std::wstring exe;
};

bool SnapshotProcesses(std::vector<ProcessInfo>& processes) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) return false;
    PROCESSENTRY32W entry{};
    entry.dwSize = sizeof(entry);
    for (BOOL more = Process32FirstW(snapshot, &entry); more; more = Process32NextW(snapshot, &entry)) {
        processes.push_back({entry.th32ProcessID, entry.th32ParentProcessID, entry.szExeFile});
    }
    CloseHandle(snapshot);
    return true;
}

bool ProcessIsRunning(DWORD pid) {
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid);
    if (!process) return GetLastError() == ERROR_ACCESS_DENIED;
    DWORD exitCode = 0;
    bool running = GetExitCodeProcess(process, &exitCode) && exitCode == STILL_ACTIVE;
    CloseHandle(process);
    return running;
}

// Per-process loopback for the chosen apps: one capture per running matching
// process, all summed into the "others" signal.
class AppLoopbackSet {
public:
    explicit AppLoopbackSet(HANDLE cancel) : cancel_(cancel) {}

    void SetApps(std::vector<std::wstring> names) {
        Close();
        names_ = std::move(names);
        everOpened_ = false;
    }
    bool AnyOpen() const { return !sources_.empty(); }

    // Opens captures for newly started matching processes and closes those of
    // exited ones. Returns false if this Windows can't capture by process.
    bool Refresh() {
        std::vector<ProcessInfo> processes;
        if (!SnapshotProcesses(processes)) return true;  // try again next time

        std::vector<DWORD> matches;
        for (const auto& process : processes) {
            for (const auto& name : names_) {
                if (CompareStringOrdinal(process.exe.c_str(), -1, name.c_str(), -1, TRUE) == CSTR_EQUAL) {
                    matches.push_back(process.pid);
                    break;
                }
            }
        }
        auto matched = [&](DWORD pid) { return std::find(matches.begin(), matches.end(), pid) != matches.end(); };
        // A capture already includes the process tree, so only roots need one.
        std::vector<DWORD> roots;
        for (DWORD pid : matches) {
            auto process = std::find_if(processes.begin(), processes.end(),
                                        [&](const ProcessInfo& info) { return info.pid == pid; });
            if (process != processes.end() && !matched(process->parent)) roots.push_back(pid);
        }
        auto isRoot = [&](DWORD pid) { return std::find(roots.begin(), roots.end(), pid) != roots.end(); };

        std::erase_if(sources_, [&](const App& app) { return !isRoot(app.pid); });
        std::erase_if(failed_, [&](const auto& entry) { return !isRoot(entry.first); });

        ULONGLONG now = GetTickCount64();
        for (DWORD pid : roots) {
            if (std::any_of(sources_.begin(), sources_.end(), [&](const App& app) { return app.pid == pid; })) continue;
            auto failure = failed_.find(pid);
            if (failure != failed_.end() && now - failure->second < kFailedRetryMs) continue;

            auto source = std::make_unique<CaptureSource>(L"application audio", eRender, eConsole, true);
            HRESULT hr = source->OpenProcessLoopback(pid, cancel_);
            if (SUCCEEDED(hr)) {
                everOpened_ = true;
                failed_.erase(pid);
                sources_.push_back({pid, std::move(source)});
                continue;
            }
            // Access denied is about this process (e.g. an elevated app).
            // Anything else, while it runs and nothing has ever worked, means
            // Windows doesn't know this kind of capture.
            if (!everOpened_ && hr != E_ACCESSDENIED && hr != HRESULT_FROM_WIN32(ERROR_TIMEOUT) &&
                ProcessIsRunning(pid)) {
                return false;
            }
            failed_[pid] = now;
        }
        return true;
    }

    // Closes captures whose stream failed; Refresh() reopens them if the
    // process is still there.
    void Drain() {
        std::erase_if(sources_, [](const App& app) { return !app.source->Drain(); });
    }
    void MixInto(std::vector<float>& mix, size_t count) {
        for (auto& app : sources_) app.source->MixInto(mix, count);
    }
    void DiscardPending() {
        for (auto& app : sources_) app.source->DiscardPending();
    }
    void Close() {
        sources_.clear();
        failed_.clear();
    }

private:
    struct App {
        DWORD pid;
        std::unique_ptr<CaptureSource> source;
    };
    static constexpr ULONGLONG kFailedRetryMs = 10000;

    HANDLE cancel_;
    std::vector<std::wstring> names_;
    std::vector<App> sources_;
    std::map<DWORD, ULONGLONG> failed_;  // pid -> when opening it last failed
    bool everOpened_ = false;
};

// Linear below the knee, then smoothly saturating towards +-1, so two loud
// sources summed don't hard-clip.
float SoftLimit(float x) {
    constexpr float kKnee = 0.8f;
    if (std::isnan(x)) return 0.0f;
    float magnitude = std::fabs(x);
    if (magnitude <= kKnee) return x;
    float limited = kKnee + (1.0f - kKnee) * std::tanh((magnitude - kKnee) / (1.0f - kKnee));
    return std::copysign(limited, x);
}

}  // namespace

std::vector<AudioDevice> Recorder::ListDevices(bool microphones) {
    std::vector<AudioDevice> devices;
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDeviceCollection> collection;
    UINT count = 0;
    if (FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->EnumAudioEndpoints(microphones ? eCapture : eRender, DEVICE_STATE_ACTIVE, &collection)) ||
        FAILED(collection->GetCount(&count))) {
        return devices;
    }
    for (UINT i = 0; i < count; ++i) {
        ComPtr<IMMDevice> device;
        LPWSTR id = nullptr;
        if (FAILED(collection->Item(i, &device)) || FAILED(device->GetId(&id))) continue;
        AudioDevice entry{id, FriendlyName(device.Get())};
        CoTaskMemFree(id);
        if (entry.name.empty()) entry.name = microphones ? L"Unnamed microphone" : L"Unnamed playback device";
        devices.push_back(std::move(entry));
    }
    std::sort(devices.begin(), devices.end(), [](const AudioDevice& a, const AudioDevice& b) {
        return CompareStringOrdinal(a.name.c_str(), -1, b.name.c_str(), -1, TRUE) == CSTR_LESS_THAN;
    });
    return devices;
}

std::wstring Recorder::DeviceName(const std::wstring& id) {
    ComPtr<IMMDeviceEnumerator> enumerator;
    ComPtr<IMMDevice> device;
    if (id.empty() ||
        FAILED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator))) ||
        FAILED(enumerator->GetDevice(id.c_str(), &device))) {
        return {};
    }
    return FriendlyName(device.Get());
}

Recorder::Recorder(HWND notifyWindow)
    : notifyWindow_(notifyWindow), stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

Recorder::~Recorder() {
    Stop();
    CloseHandle(stopEvent_);
}

bool Recorder::Start(const std::wstring& audioPath, const std::wstring& title, const DeviceChoice& devices,
                     bool separateChannels, std::wstring& error) {
    if (IsRunning()) {
        error = L"Already recording.";
        return false;
    }
    SetDevices(devices);
    ResetEvent(stopEvent_);
    paused_ = false;
    recordedSamples_ = 0;
    separateChannels_ = separateChannels;
    {
        std::lock_guard lock(pauseMutex_);
        pauseEdges_.clear();
    }

    HANDLE ready = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    std::wstring startError;
    thread_ = std::thread(&Recorder::Run, this, audioPath, title, ready, &startError);
    WaitForSingleObject(ready, INFINITE);
    CloseHandle(ready);

    if (!startError.empty()) {
        thread_.join();
        error = startError;
        return false;
    }
    return true;
}

void Recorder::Stop() {
    if (!IsRunning()) return;
    SetEvent(stopEvent_);
    thread_.join();
}

void Recorder::SetDevices(const DeviceChoice& devices) {
    std::lock_guard lock(devicesMutex_);
    devices_ = devices;
    devicesChosen_ = true;
}

void Recorder::SetPaused(bool paused) {
    if (paused_.exchange(paused) == paused || !IsRunning()) return;
    // Blocks are mixed kLatency behind real time, so the thread learns of this
    // late. Note where on the timeline it happened so it can cut there.
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    int64_t position = (now.QuadPart - clockStart_) * kRate / clockFrequency_;
    std::lock_guard lock(pauseMutex_);
    pauseEdges_.push_back({position, paused});
}

double Recorder::RecordedSeconds() const {
    return static_cast<double>(recordedSamples_) / kRate;
}

std::vector<std::wstring> Recorder::TakeWarnings() {
    std::lock_guard lock(warningMutex_);
    return std::exchange(warnings_, {});
}

void Recorder::Warn(std::wstring message) {
    {
        std::lock_guard lock(warningMutex_);
        // Never queue the same message twice; it would only nag.
        if (std::find(warnings_.begin(), warnings_.end(), message) != warnings_.end()) return;
        warnings_.push_back(std::move(message));
    }
    PostMessageW(notifyWindow_, kWarningMessage, 0, 0);
}

void Recorder::Run(std::wstring audioPath, std::wstring title, HANDLE ready, std::wstring* startError) {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    // Keeps the 10 ms loop scheduled on time when the PC is busy.
    DWORD taskIndex = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Audio", &taskIndex);

    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));

    std::atomic<bool> defaultChanged{false};
    std::atomic<bool> deviceArrived{false};
    auto* watcher = new DeviceWatcher(&defaultChanged, &deviceArrived);
    if (enumerator) enumerator->RegisterEndpointNotificationCallback(watcher);

    // The communications role is what Teams, Zoom and softphones use, so the
    // mic follows whichever headset the call is actually on.
    CaptureSource mic(L"microphone", eCapture, eCommunications, false);
    CaptureSource system(L"system audio", eRender, eConsole, true);
    AppLoopbackSet apps(stopEvent_);
    CaptureSource* sources[] = {&mic, &system};

    std::vector<std::wstring> appNames;
    bool appFallback = false;  // per-process capture isn't available: use the whole device
    bool warnAppFallback = false;
    // Only the chosen apps are captured, instead of the playback device.
    auto appMode = [&] { return !appNames.empty() && !appFallback; };
    // Says whether each of mic, system and the app list changed.
    auto takeChoice = [&] {
        std::lock_guard lock(devicesMutex_);
        devicesChosen_ = false;
        bool appsChanged = appNames != devices_.loopbackApps;
        appNames = devices_.loopbackApps;
        bool micChanged = mic.Choose(devices_.microphone);
        micChanged = mic.SetCommunications(devices_.echoCancellation) || micChanged;
        return std::array{micChanged, system.Choose(devices_.speakers), appsChanged};
    };
    takeChoice();
    apps.SetApps(appNames);

    ULONGLONG lastAppScan = 0;
    auto scanApps = [&] {
        lastAppScan = GetTickCount64();
        if (!apps.Refresh()) {
            appFallback = true;
            warnAppFallback = true;
            apps.Close();
            system.Open(enumerator.Get());
        }
    };

    // Says once each time a chosen device is unavailable and the default is used.
    bool warnedFallback[std::size(sources)] = {};
    auto warnFallback = [&] {
        for (size_t i = 0; i < std::size(sources); ++i) {
            bool fellBack = sources[i]->IsOpen() && sources[i]->FellBack();
            if (fellBack && !warnedFallback[i]) {
                Warn(std::wstring(L"The chosen ") + (sources[i] == &mic ? L"microphone" : L"playback device") +
                     L" isn't available. Using the Windows default until it is.");
            }
            warnedFallback[i] = fellBack;
        }
    };
    // Says once when Windows privacy settings block the microphone, however
    // often the open is retried.
    bool warnedMicBlocked = false;
    auto warnMicBlocked = [&] {
        bool blocked = !mic.IsOpen() && mic.LastError() == E_ACCESSDENIED;
        if (blocked && !warnedMicBlocked) {
            Warn(L"Windows is blocking microphone access. Turn on Settings > Privacy & security > Microphone > "
                 L"Let desktop apps access your microphone.");
        }
        warnedMicBlocked = blocked;
    };
    auto warnAppFallbackOnce = [&] {
        if (std::exchange(warnAppFallback, false)) {
            Warn(L"Recording only chosen apps needs a newer Windows 10 or 11. Recording all system audio instead.");
        }
    };

    const bool stereo = separateChannels_;
    OpusFileWriter writer;
    bool ok = false;
    if (!enumerator) {
        *startError = L"Windows audio is unavailable.";
    } else {
        bool micOpened = mic.Open(enumerator.Get());
        if (appNames.empty()) {
            system.Open(enumerator.Get());
        } else {
            scanApps();
        }
        if (!micOpened && !system.IsOpen() && !apps.AnyOpen()) {
            *startError = L"No microphone or playback device could be opened.";
        } else {
            ok = writer.Open(audioPath, title, stereo ? kStereoBitrate : kBitrate, stereo ? 2 : 1, *startError);
        }
    }
    LARGE_INTEGER frequency, start;
    QueryPerformanceFrequency(&frequency);
    QueryPerformanceCounter(&start);
    clockStart_ = start.QuadPart;
    clockFrequency_ = frequency.QuadPart;
    SetEvent(ready);  // startError must not be touched after this

    if (ok) {
        if (!mic.IsOpen() && mic.LastError() != E_ACCESSDENIED) {
            Warn(L"No microphone found. Recording system audio only.");
        }
        if (!system.IsOpen() && !appMode()) Warn(L"Couldn't capture system audio. Recording microphone only.");
        warnMicBlocked();
        warnAppFallbackOnce();
        warnFallback();

        int64_t consumed = 0;  // samples of the timeline mixed so far, including paused time
        ULONGLONG lastRetry = GetTickCount64();
        bool writeFailed = false;
        std::vector<float> micMix, othersMix, out;
        std::deque<PauseEdge> edges;
        bool cutting = false;  // inside a paused stretch of the timeline
        ULONGLONG quietSince = GetTickCount64();
        bool warnedSilent = false;

        for (bool stopping = false; !stopping;) {
            stopping = WaitForSingleObject(stopEvent_, 10) != WAIT_TIMEOUT;

            bool newDefault = defaultChanged.exchange(false);
            bool arrived = deviceArrived.exchange(false);
            // The system source is idle while only chosen apps are captured.
            auto idle = [&](CaptureSource* source) { return source == &system && appMode(); };
            if (devicesChosen_) {
                auto changed = takeChoice();
                if (changed[2]) {
                    appFallback = false;
                    apps.SetApps(appNames);
                    if (appNames.empty()) {
                        if (!system.IsOpen()) system.Open(enumerator.Get());
                    } else {
                        system.Close();
                        scanApps();
                    }
                }
                for (size_t i = 0; i < std::size(sources); ++i) {
                    if (changed[i] && !idle(sources[i])) sources[i]->Open(enumerator.Get());
                }
                warnFallback();
            } else if (newDefault || arrived) {
                // A new default matters to sources following it; a device
                // arriving may be the chosen one coming back.
                for (auto* source : sources) {
                    if (idle(source)) continue;
                    if ((newDefault && source->FollowsDefault()) || (arrived && source->FellBack())) {
                        source->Open(enumerator.Get());
                    }
                }
                warnFallback();
            } else if ((!mic.IsOpen() || (!system.IsOpen() && !appMode())) &&
                       GetTickCount64() - lastRetry > kRetryIntervalMs) {
                lastRetry = GetTickCount64();
                for (auto* source : sources) {
                    if (!source->IsOpen() && !idle(source)) source->Open(enumerator.Get());
                }
            }
            if (appMode() && GetTickCount64() - lastAppScan >= kAppScanIntervalMs) scanApps();
            warnMicBlocked();
            warnAppFallbackOnce();

            for (auto* source : sources) {
                bool wasOpen = source->IsOpen();
                if (!source->Drain() && wasOpen) {
                    Warn(std::wstring(L"Lost the ") + source->Name() + L". Reconnecting...");
                }
            }
            apps.Drain();

            // A microphone that is open but only ever silent is probably muted
            // or the wrong one. (Loopback is silent whenever nobody speaks.)
            float micPeak = mic.TakePeak();
            ULONGLONG nowMs = GetTickCount64();
            if (!mic.IsOpen() || paused_ || micPeak >= kSilentPeak) {
                quietSince = nowMs;
                if (mic.IsOpen() && !paused_ && micPeak >= kSilentPeak) warnedSilent = false;  // sound again: re-arm
            } else if (!warnedSilent && nowMs - quietSince >= kMicSilenceWarnMs) {
                Warn(L"Your microphone has been silent for a minute. Check it isn't muted or the wrong device.");
                warnedSilent = true;
            }

            LARGE_INTEGER now;
            QueryPerformanceCounter(&now);
            int64_t timeline = (now.QuadPart - start.QuadPart) * kRate / frequency.QuadPart - kLatency;
            int64_t due = timeline - consumed;
            if (due <= 0) continue;
            if (due > kMaxCatchUp) {
                // The PC slept or the thread stalled: skip the gap instead of
                // writing minutes of silence.
                consumed = timeline - kRate / 100;
                due = kRate / 100;
                for (auto* source : sources) source->DiscardPending();
                apps.DiscardPending();
            }

            size_t count = static_cast<size_t>(due);
            micMix.assign(count, 0.0f);
            othersMix.assign(count, 0.0f);
            mic.MixInto(micMix, count);
            system.MixInto(othersMix, count);
            apps.MixInto(othersMix, count);
            const size_t channels = stereo ? 2 : 1;
            out.resize(count * channels);
            if (stereo) {
                for (size_t i = 0; i < count; ++i) {
                    out[2 * i] = SoftLimit(micMix[i]);
                    out[2 * i + 1] = SoftLimit(othersMix[i]);
                }
            } else {
                for (size_t i = 0; i < count; ++i) out[i] = SoftLimit(micMix[i] + othersMix[i]);
            }

            // Write the block except the stretches captured while paused.
            {
                std::lock_guard lock(pauseMutex_);
                edges.insert(edges.end(), pauseEdges_.begin(), pauseEdges_.end());
                pauseEdges_.clear();
            }
            int64_t blockEnd = consumed + due;
            size_t written = 0;  // samples of this block handled so far
            auto emit = [&](size_t upTo) {
                if (upTo <= written) return;
                if (!cutting) {
                    int frames = static_cast<int>(upTo - written);
                    if (!writer.Write(out.data() + written * channels, frames) && !writeFailed) {
                        writeFailed = true;
                        Warn(L"Writing the recording failed. Is the disk full?");
                    }
                    recordedSamples_ += frames;
                }
                written = upTo;
            };
            while (!edges.empty() && edges.front().position < blockEnd) {
                emit(static_cast<size_t>(std::clamp<int64_t>(edges.front().position - consumed, 0, due)));
                cutting = edges.front().paused;
                edges.pop_front();
            }
            emit(count);
            consumed = blockEnd;
        }
    }

    writer.Close();
    mic.Close();
    system.Close();
    apps.Close();
    if (enumerator) enumerator->UnregisterEndpointNotificationCallback(watcher);
    watcher->Release();
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(coInit)) CoUninitialize();
}
