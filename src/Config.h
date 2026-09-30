#pragma once

#include <string>
#include <vector>

// %APPDATA%\CallRecorder\config.json. Created with defaults on first run;
// re-read by the uploader on every cycle, so edits apply without a restart.
struct Config {
    std::wstring serverUrl;                       // e.g. https://speakr.example.com; empty until set up
    std::vector<std::wstring> tags = {L"Call"};  // ticked in the Tags menu at launch; created in Speakr if missing
    std::wstring hotwords;                        // comma-separated names/jargon to help transcription
    int keepAudioDays = 14;                       // delete local audio this long after Speakr is done; 0 = never
    bool sensitiveByDefault = false;              // start each app launch in local-only mode
    std::wstring microphone;                      // Windows device ID; empty = default communications mic
    std::wstring speakers;                        // Windows device ID; empty = default playback device
    int uploadDelaySeconds = 60;                  // wait this long after stopping before uploading (0 = at once)
    bool separateChannels = false;                // stereo: microphone on the left, everyone else on the right
    bool echoCancellation = false;                // open the mic as a communications stream (device echo cancellation)
    std::vector<std::wstring> loopbackApps;       // record only audio these apps play (e.g. Teams.exe); empty = all
    // Not a setting: true when config.json exists but couldn't be read, in
    // which case the fields above are defaults and the app fails closed
    // (starts in local-only mode). Never saved.
    bool unreadable = false;

    static std::wstring Path();
    static Config Load();  // writes the defaults file if it doesn't exist
    // The reading half of Load, for text already in hand. Never throws; text
    // that isn't a JSON object gives defaults with `unreadable` set.
    static Config Parse(const std::string& text);
    // Writes every setting above, keeping the file's key order and any keys
    // this version doesn't know.
    bool Save() const;
};

// Trims each name, drops empty ones and repeats (ignoring case), keeping order.
std::vector<std::wstring> NormalizeAppList(const std::vector<std::wstring>& names);

// Tidies a pasted Speakr address: adds https:// if there's no scheme and
// drops a trailing slash or /api/v1.
std::wstring NormalizeServerUrl(std::wstring url);

// The Speakr API token, in Windows Credential Manager ("CallRecorder:Speakr").
// Empty if not set.
std::string ReadSpeakrToken();
bool WriteSpeakrToken(const std::string& token);
