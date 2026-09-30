#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

// An active Windows audio endpoint, for the device menus.
struct AudioDevice {
    std::wstring id;  // IMMDevice ID; stable across reboots and replugging
    std::wstring name;
};

// Which endpoints to record. An empty ID follows the Windows default.
struct DeviceChoice {
    std::wstring microphone;
    std::wstring speakers;
};

// Records "both sides of a call": a microphone (you) mixed with a loopback
// capture of the speakers/headset (everyone else). By default these are the
// Windows default communications microphone and default playback device.
// Runs on its own thread; all public methods are called from the UI thread.
class Recorder {
public:
    // Posted to the notify window when TakeWarning() has something to show.
    static constexpr UINT kWarningMessage = WM_APP + 2;

    // Active microphones (capture) or playback devices (render), by name.
    static std::vector<AudioDevice> ListDevices(bool microphones);
    // The name of any known device, connected or not; empty if unknown.
    static std::wstring DeviceName(const std::wstring& id);

    explicit Recorder(HWND notifyWindow);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool Start(const std::wstring& audioPath, const std::wstring& title, const DeviceChoice& devices,
               std::wstring& error);
    void Stop();  // Blocks until the file is finalised.
    // Switches devices; takes effect within 10 ms if recording.
    void SetDevices(const DeviceChoice& devices);

    bool IsRunning() const { return thread_.joinable(); }
    void SetPaused(bool paused) { paused_ = paused; }
    bool IsPaused() const { return paused_; }
    double RecordedSeconds() const;

    std::wstring TakeWarning();

private:
    void Run(std::wstring audioPath, std::wstring title, HANDLE ready, std::wstring* startError);
    void Warn(std::wstring message);

    HWND notifyWindow_;
    HANDLE stopEvent_;
    std::thread thread_;
    std::atomic<bool> paused_{false};
    std::atomic<int64_t> recordedSamples_{0};

    std::mutex devicesMutex_;
    DeviceChoice devices_;
    std::atomic<bool> devicesChosen_{false};  // devices_ changed since the thread last read it

    std::mutex warningMutex_;
    std::wstring warning_;
};
