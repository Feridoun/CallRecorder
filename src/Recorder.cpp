#include "Recorder.h"

#include "OpusFileWriter.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
// After mmdeviceapi.h, which defines DEFINE_PROPERTYKEY.
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <utility>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

constexpr int kRate = OpusFileWriter::kSampleRate;
constexpr int kBitrate = 24000;             // ~11 MB per hour, plenty for speech
constexpr int64_t kLatency = kRate / 10;    // mix 100 ms behind real time
constexpr size_t kMaxBacklog = kRate * 3 / 10;  // drop audio older than this
constexpr int64_t kMaxCatchUp = kRate * 2;  // larger gaps (sleep/hibernate) are skipped
constexpr REFERENCE_TIME kBufferDuration = 10'000'000;  // 1 s WASAPI buffer
constexpr ULONGLONG kRetryIntervalMs = 2000;

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

// One WASAPI capture stream, converted by Windows to 48 kHz mono float, plus
// the samples captured but not yet mixed.
class CaptureSource {
public:
    CaptureSource(const wchar_t* name, EDataFlow flow, ERole role, bool loopback)
        : name_(name), flow_(flow), role_(role), loopback_(loopback) {}
    ~CaptureSource() { Close(); }

    const wchar_t* Name() const { return name_; }
    bool IsOpen() const { return capture_ != nullptr; }

    // An empty ID follows the Windows default. Takes effect on the next Open().
    // Returns whether the choice changed.
    bool Choose(const std::wstring& id) { return std::exchange(chosenId_, id) != id; }
    // A device was chosen but isn't the one open (or nothing is open).
    bool FellBack() const { return !chosenId_.empty() && openedId_ != chosenId_; }
    bool FollowsDefault() const { return chosenId_.empty() || FellBack(); }

    // Opens the chosen device, or the default one if it isn't available.
    bool Open(IMMDeviceEnumerator* enumerator) {
        Close();
        ComPtr<IMMDevice> device;
        DWORD state = 0;
        if (!chosenId_.empty() && SUCCEEDED(enumerator->GetDevice(chosenId_.c_str(), &device)) &&
            SUCCEEDED(device->GetState(&state)) && state == DEVICE_STATE_ACTIVE && OpenDevice(device.Get())) {
            return true;
        }
        device.Reset();
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow_, role_, &device))) return false;
        return OpenDevice(device.Get());
    }

    void Close() {
        if (client_) client_->Stop();
        capture_.Reset();
        client_.Reset();
        openedId_.clear();
        pending_.clear();
        primed_ = false;
    }

private:
    bool OpenDevice(IMMDevice* device) {
        if (FAILED(device->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, &client_))) return false;

        WAVEFORMATEX format{};
        format.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
        format.nChannels = 1;
        format.nSamplesPerSec = kRate;
        format.wBitsPerSample = 32;
        format.nBlockAlign = sizeof(float);
        format.nAvgBytesPerSec = kRate * sizeof(float);

        DWORD flags = AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
        if (loopback_) flags |= AUDCLNT_STREAMFLAGS_LOOPBACK;

        ComPtr<IAudioCaptureClient> capture;
        if (FAILED(client_->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, kBufferDuration, 0, &format, nullptr)) ||
            FAILED(client_->GetService(IID_PPV_ARGS(&capture))) || FAILED(client_->Start())) {
            Close();
            return false;
        }
        capture_ = capture;
        LPWSTR id = nullptr;
        if (SUCCEEDED(device->GetId(&id))) openedId_ = id;
        CoTaskMemFree(id);
        return true;
    }

public:

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
                pending_.insert(pending_.end(), samples, samples + frames);
            }
            capture_->ReleaseBuffer(frames);
        }
        if (FAILED(hr)) {
            Close();
            return false;
        }
        return true;
    }

    // Adds up to `count` samples into `mix`. Loopback delivers nothing while
    // nothing is playing, so after running dry a source waits until it has
    // built up kLatency of audio again; otherwise every 10 ms tick would
    // underrun and chop the audio into fragments.
    void MixInto(std::vector<float>& mix, size_t count) {
        if (pending_.size() > count + kMaxBacklog) {
            pending_.erase(pending_.begin(), pending_.end() - static_cast<ptrdiff_t>(count + kMaxBacklog));
        }
        if (!primed_) {
            if (pending_.size() < static_cast<size_t>(kLatency)) return;
            primed_ = true;
        }
        size_t take = std::min(count, pending_.size());
        for (size_t i = 0; i < take; ++i) mix[i] += pending_[i];
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<ptrdiff_t>(take));
        if (pending_.empty()) primed_ = false;
    }

    void DiscardPending() {
        pending_.clear();
        primed_ = false;
    }

private:
    const wchar_t* name_;
    EDataFlow flow_;
    ERole role_;
    bool loopback_;
    std::wstring chosenId_;
    std::wstring openedId_;
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    std::vector<float> pending_;
    bool primed_ = false;
};

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
                     bool /*separateChannels*/, std::wstring& error) {
    if (IsRunning()) {
        error = L"Already recording.";
        return false;
    }
    SetDevices(devices);
    ResetEvent(stopEvent_);
    paused_ = false;
    recordedSamples_ = 0;

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
        warnings_.push_back(std::move(message));
    }
    PostMessageW(notifyWindow_, kWarningMessage, 0, 0);
}

void Recorder::Run(std::wstring audioPath, std::wstring title, HANDLE ready, std::wstring* startError) {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

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
    CaptureSource* sources[] = {&mic, &system};
    // Returns whether each source's choice changed.
    auto takeChoice = [&] {
        std::lock_guard lock(devicesMutex_);
        devicesChosen_ = false;
        return std::array{mic.Choose(devices_.microphone), system.Choose(devices_.speakers)};
    };
    takeChoice();
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

    OpusFileWriter writer;
    bool ok = false;
    if (!enumerator) {
        *startError = L"Windows audio is unavailable.";
    } else {
        bool micOpened = mic.Open(enumerator.Get());
        bool systemOpened = system.Open(enumerator.Get());
        if (!micOpened && !systemOpened) {
            *startError = L"No microphone or playback device could be opened.";
        } else {
            ok = writer.Open(audioPath, title, kBitrate, *startError);
        }
    }
    SetEvent(ready);  // startError must not be touched after this

    if (ok) {
        if (!mic.IsOpen()) Warn(L"No microphone found. Recording system audio only.");
        if (!system.IsOpen()) Warn(L"Couldn't capture system audio. Recording microphone only.");
        warnFallback();

        LARGE_INTEGER frequency, start;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        int64_t consumed = 0;  // samples of the timeline mixed so far, including paused time
        ULONGLONG lastRetry = GetTickCount64();
        bool writeFailed = false;
        std::vector<float> mix;

        for (bool stopping = false; !stopping;) {
            stopping = WaitForSingleObject(stopEvent_, 10) != WAIT_TIMEOUT;

            bool newDefault = defaultChanged.exchange(false);
            bool arrived = deviceArrived.exchange(false);
            if (devicesChosen_) {
                auto changed = takeChoice();
                for (size_t i = 0; i < std::size(sources); ++i) {
                    if (changed[i]) sources[i]->Open(enumerator.Get());
                }
                warnFallback();
            } else if (newDefault || arrived) {
                // A new default matters to sources following it; a device
                // arriving may be the chosen one coming back.
                for (auto* source : sources) {
                    if ((newDefault && source->FollowsDefault()) || (arrived && source->FellBack())) {
                        source->Open(enumerator.Get());
                    }
                }
                warnFallback();
            } else if ((!mic.IsOpen() || !system.IsOpen()) && GetTickCount64() - lastRetry > kRetryIntervalMs) {
                lastRetry = GetTickCount64();
                for (auto* source : sources) {
                    if (!source->IsOpen()) source->Open(enumerator.Get());
                }
            }

            for (auto* source : sources) {
                bool wasOpen = source->IsOpen();
                if (!source->Drain() && wasOpen) {
                    Warn(std::wstring(L"Lost the ") + source->Name() + L". Reconnecting...");
                }
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
            }

            size_t count = static_cast<size_t>(due);
            mix.assign(count, 0.0f);
            for (auto* source : sources) source->MixInto(mix, count);
            for (float& sample : mix) sample = std::clamp(sample, -1.0f, 1.0f);
            consumed += due;

            if (!paused_) {
                if (!writer.Write(mix.data(), static_cast<int>(count)) && !writeFailed) {
                    writeFailed = true;
                    Warn(L"Writing the recording failed. Is the disk full?");
                }
                recordedSamples_ += due;
            }
        }
    }

    writer.Close();
    mic.Close();
    system.Close();
    if (enumerator) enumerator->UnregisterEndpointNotificationCallback(watcher);
    watcher->Release();
    if (SUCCEEDED(coInit)) CoUninitialize();
}
