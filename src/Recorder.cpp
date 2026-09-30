#include "Recorder.h"

#include "OpusFileWriter.h"

#include <audioclient.h>
#include <mmdeviceapi.h>
#include <wrl/client.h>

#include <algorithm>
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

// Sets a flag whenever the default audio devices change (headset plugged in,
// Bluetooth connects, ...), so capture can move to the new device.
class DeviceWatcher final : public IMMNotificationClient {
public:
    explicit DeviceWatcher(std::atomic<bool>* changed) : changed_(changed) {}

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
        *changed_ = true;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE OnDeviceStateChanged(LPCWSTR, DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceAdded(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnDeviceRemoved(LPCWSTR) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE OnPropertyValueChanged(LPCWSTR, const PROPERTYKEY) override { return S_OK; }

private:
    LONG refs_ = 1;
    std::atomic<bool>* changed_;
};

// One WASAPI capture stream, converted by Windows to 48 kHz mono float, plus
// the samples captured but not yet mixed.
class CaptureSource {
public:
    CaptureSource(const wchar_t* name, EDataFlow flow, ERole role, bool loopback)
        : name_(name), flow_(flow), role_(role), loopback_(loopback) {}
    ~CaptureSource() { Close(); }

    const wchar_t* Name() const { return name_; }
    bool IsOpen() const { return capture_ != nullptr; }

    bool Open(IMMDeviceEnumerator* enumerator) {
        Close();
        ComPtr<IMMDevice> device;
        if (FAILED(enumerator->GetDefaultAudioEndpoint(flow_, role_, &device))) return false;
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
        return true;
    }

    void Close() {
        if (client_) client_->Stop();
        capture_.Reset();
        client_.Reset();
        pending_.clear();
        primed_ = false;
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
    ComPtr<IAudioClient> client_;
    ComPtr<IAudioCaptureClient> capture_;
    std::vector<float> pending_;
    bool primed_ = false;
};

}  // namespace

Recorder::Recorder(HWND notifyWindow)
    : notifyWindow_(notifyWindow), stopEvent_(CreateEventW(nullptr, TRUE, FALSE, nullptr)) {}

Recorder::~Recorder() {
    Stop();
    CloseHandle(stopEvent_);
}

bool Recorder::Start(const std::wstring& audioPath, const std::wstring& title, std::wstring& error) {
    if (IsRunning()) {
        error = L"Already recording.";
        return false;
    }
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

double Recorder::RecordedSeconds() const {
    return static_cast<double>(recordedSamples_) / kRate;
}

std::wstring Recorder::TakeWarning() {
    std::lock_guard lock(warningMutex_);
    return std::exchange(warning_, {});
}

void Recorder::Warn(std::wstring message) {
    {
        std::lock_guard lock(warningMutex_);
        warning_ = std::move(message);
    }
    PostMessageW(notifyWindow_, kWarningMessage, 0, 0);
}

void Recorder::Run(std::wstring audioPath, std::wstring title, HANDLE ready, std::wstring* startError) {
    HRESULT coInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);

    ComPtr<IMMDeviceEnumerator> enumerator;
    CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&enumerator));

    std::atomic<bool> devicesChanged{false};
    auto* watcher = new DeviceWatcher(&devicesChanged);
    if (enumerator) enumerator->RegisterEndpointNotificationCallback(watcher);

    // The communications role is what Teams, Zoom and softphones use, so the
    // mic follows whichever headset the call is actually on.
    CaptureSource mic(L"microphone", eCapture, eCommunications, false);
    CaptureSource system(L"system audio", eRender, eConsole, true);
    CaptureSource* sources[] = {&mic, &system};

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

        LARGE_INTEGER frequency, start;
        QueryPerformanceFrequency(&frequency);
        QueryPerformanceCounter(&start);
        int64_t consumed = 0;  // samples of the timeline mixed so far, including paused time
        ULONGLONG lastRetry = GetTickCount64();
        bool writeFailed = false;
        std::vector<float> mix;

        for (bool stopping = false; !stopping;) {
            stopping = WaitForSingleObject(stopEvent_, 10) != WAIT_TIMEOUT;

            if (devicesChanged.exchange(false)) {
                for (auto* source : sources) source->Open(enumerator.Get());
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
