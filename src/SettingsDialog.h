#pragma once

#include <windows.h>

// The Settings dialog: Speakr connection (address and API token, with a test
// against /api/v1/users/me), audio devices, and the rest of config.json.
// Saves to config.json, Credential Manager and the Start at login entry.
// Returns true if the user saved. Only one can be open at a time; a second
// call brings the open one to the front and returns false.
bool ShowSettingsDialog(HINSTANCE instance, HWND owner);
