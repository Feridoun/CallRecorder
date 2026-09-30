#pragma once

#include <windows.h>

// The "Speakr connection" dialog: server address and API token, with a test
// against /api/v1/users/me. Saves to config.json and Credential Manager.
// Returns true if the user saved. Only one can be open at a time; a second
// call brings the open one to the front and returns false.
bool ShowConnectionDialog(HINSTANCE instance, HWND owner);
