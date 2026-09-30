#pragma once

#include <windows.h>

#include <functional>
#include <string>

// The Recordings window: every session in sessionsDir, newest first, with its
// date, duration, where it is on its way to Speakr and any error, plus actions
// on the selected one (open in Speakr, play, show in folder, retry, keep on
// this PC, delete). Modeless and single-instance: a second call brings the
// open window to the front. Runs on the UI thread; refreshes itself while open.
//
// serverUrl is the configured Speakr address (for links). onChanged is called
// after an action changed a sidecar, so the caller can wake the uploader.
void ShowHistoryWindow(HINSTANCE instance, const std::wstring& sessionsDir, const std::wstring& serverUrl,
                       std::function<void()> onChanged);

// For the message loop: true if the message was dialog navigation (Tab,
// Enter, Esc) for the Recordings window and has been handled.
bool IsHistoryWindowMessage(MSG* message);
