#pragma once

#include <string>

// MeetingRecorder was called CallRecorder up to version 1.2. On first run
// after the rename, this moves what CallRecorder left behind to the new
// names: the settings and recordings folders, the Speakr token in Credential
// Manager and the Start at login entry. Anything already under the new name
// is kept, and the old copy left alone.
namespace Migration {

// True if CallRecorder is running, in which case its folders are in use and
// it should be exited before migrating.
bool OldAppRunning();

// Runs before anything reads settings or recordings. Returns the old folders
// that couldn't be moved, one per line; empty if all went well. The caller
// should then stop without creating the new folders, so the next run retries.
std::wstring FromCallRecorder();

}  // namespace Migration
