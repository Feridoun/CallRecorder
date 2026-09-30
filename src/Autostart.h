#pragma once

// Start-at-login via HKCU\Software\Microsoft\Windows\CurrentVersion\Run, the
// per-user entry that also appears in Task Manager > Startup apps (and in
// Settings > Apps > Startup), where the user can switch it off too.
namespace Autostart {

// True if the Run entry exists and hasn't been disabled in Task Manager.
bool IsEnabled();

// Enabling points the entry at this .exe and clears any "disabled" flag Task
// Manager left behind; disabling removes both.
bool SetEnabled(bool enabled);

// If the entry points at an .exe that no longer exists (the app was moved),
// repoint it at this one. Leaves a working entry for another copy alone.
void RepairPath();

// Replaces an entry made under the app's old name with one for this .exe, if
// it was switched on. A switched-off entry is just removed.
void AdoptEntry(const wchar_t* oldName);

}  // namespace Autostart
