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

    static std::wstring Path();
    static Config Load();  // writes the defaults file if it doesn't exist
    // Changes server_url in the file, leaving the other settings as they are.
    static bool SaveServerUrl(const std::wstring& url);
};

// Tidies a pasted Speakr address: adds https:// if there's no scheme and
// drops a trailing slash or /api/v1.
std::wstring NormalizeServerUrl(std::wstring url);

// The Speakr API token, in Windows Credential Manager ("CallRecorder:Speakr").
// Empty if not set.
std::string ReadSpeakrToken();
bool WriteSpeakrToken(const std::string& token);
