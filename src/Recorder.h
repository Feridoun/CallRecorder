#pragma once

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <mutex>
#include <string>
#include <thread>

// Records "both sides of a call": the default communications microphone (you)
// mixed with a loopback capture of the default speakers/headset (everyone
// else). Runs on its own thread; all public methods are called from the UI
// thread.
class Recorder {
public:
    // Posted to the notify window when TakeWarning() has something to show.
    static constexpr UINT kWarningMessage = WM_APP + 2;

    explicit Recorder(HWND notifyWindow);
    ~Recorder();
    Recorder(const Recorder&) = delete;
    Recorder& operator=(const Recorder&) = delete;

    bool Start(const std::wstring& audioPath, const std::wstring& title, std::wstring& error);
    void Stop();  // Blocks until the file is finalised.

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

    std::mutex warningMutex_;
    std::wstring warning_;
};
