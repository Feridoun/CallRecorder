// Config::Parse and the URL/app-list normalisers only. Config::Load, Save and
// Path touch %APPDATA% and are deliberately never called from tests.
#include "TestHarness.h"

#include "Config.h"

using Names = std::vector<std::wstring>;

TEST(ConfigParse_ValidFileReadsEveryKey) {
    Config c = Config::Parse(R"({
        "server_url": "https://speakr.example.com/",
        "tags": ["Call", "Work"],
        "hotwords": "Speakr, Opus",
        "keep_audio_days": 30,
        "sensitive_by_default": true,
        "microphone": "mic-id",
        "speakers": "spk-id",
        "upload_delay_seconds": 120,
        "separate_channels": true,
        "loopback_apps": ["Teams.exe", "zoom.exe"]
    })");
    CHECK(!c.unreadable);
    CHECK_EQ(c.serverUrl, L"https://speakr.example.com");
    CHECK(c.tags == Names({L"Call", L"Work"}));
    CHECK_EQ(c.hotwords, L"Speakr, Opus");
    CHECK_EQ(c.keepAudioDays, 30);
    CHECK_EQ(c.sensitiveByDefault, true);
    CHECK_EQ(c.microphone, L"mic-id");
    CHECK_EQ(c.speakers, L"spk-id");
    CHECK_EQ(c.uploadDelaySeconds, 120);
    CHECK_EQ(c.separateChannels, true);
    CHECK(c.loopbackApps == Names({L"Teams.exe", L"zoom.exe"}));
}

TEST(ConfigParse_EmptyObjectGivesDefaults) {
    Config c = Config::Parse("{}");
    Config d;
    CHECK(!c.unreadable);
    CHECK_EQ(c.serverUrl, L"");
    CHECK(c.tags == d.tags);
    CHECK_EQ(c.keepAudioDays, 14);
    CHECK_EQ(c.uploadDelaySeconds, 60);
    CHECK_EQ(c.sensitiveByDefault, false);
    CHECK_EQ(c.separateChannels, false);
    CHECK(c.loopbackApps.empty());
}

TEST(ConfigParse_UnicodeValuesSurvive) {
    Config c = Config::Parse("{\"hotwords\": \"Z\\u00fcrich, \\u65e5\\u672c\"}");
    CHECK_EQ(c.hotwords, L"Zürich, 日本");
}

TEST(ConfigParse_UnreadableInputFailsClosed) {
    for (const char* text : {"", "   ", "garbage", "{oops", "[]", "[1,2]", "null", "5", "\"str\"", "true",
                             "\xEF\xBB\xBF{}x", "{\"a\":"}) {
        Config c = Config::Parse(text);
        CHECK(c.unreadable);
        CHECK_EQ(c.serverUrl, L"");
        CHECK_EQ(c.keepAudioDays, 14);
    }
}

TEST(ConfigParse_ToleratesLeadingBom) {
    // nlohmann skips a UTF-8 BOM itself (ReadFileText strips it as well).
    CHECK(!Config::Parse("﻿{}").unreadable);
    CHECK_EQ(Config::Parse("﻿{\"keep_audio_days\": 3}").keepAudioDays, 3);
}

TEST(ConfigParse_WrongTypesFallBackPerKey) {
    Config c = Config::Parse(R"({
        "server_url": 5,
        "tags": {},
        "hotwords": null,
        "keep_audio_days": "14",
        "sensitive_by_default": "yes",
        "microphone": [],
        "speakers": {"a": 1},
        "upload_delay_seconds": true,
        "separate_channels": 1,
        "loopback_apps": "Teams.exe"
    })");
    CHECK(!c.unreadable);
    CHECK_EQ(c.serverUrl, L"");
    CHECK(c.tags == Names({L"Call"}));  // default kept
    CHECK_EQ(c.hotwords, L"");
    CHECK_EQ(c.keepAudioDays, 14);
    CHECK_EQ(c.sensitiveByDefault, false);
    CHECK_EQ(c.microphone, L"");
    CHECK_EQ(c.speakers, L"");
    CHECK_EQ(c.uploadDelaySeconds, 60);
    CHECK_EQ(c.separateChannels, false);
    CHECK(c.loopbackApps.empty());
}

TEST(ConfigParse_NullValuesForEveryKey) {
    Config c = Config::Parse(R"({"server_url":null,"tags":null,"hotwords":null,"keep_audio_days":null,
        "sensitive_by_default":null,"microphone":null,"speakers":null,"upload_delay_seconds":null,
        "separate_channels":null,"loopback_apps":null})");
    CHECK(!c.unreadable);
    CHECK(c.tags == Names({L"Call"}));
    CHECK_EQ(c.keepAudioDays, 14);
    CHECK_EQ(c.uploadDelaySeconds, 60);
}

TEST(ConfigParse_TagsSkipNonStringsAndEmpties) {
    Config c = Config::Parse(R"({"tags": ["A", 1, "", null, "B", {}]})");
    CHECK(c.tags == Names({L"A", L"B"}));
    // An explicit empty array means "no tags", not "default tags".
    CHECK(Config::Parse(R"({"tags": []})").tags.empty());
}

TEST(ConfigParse_LoopbackAppsAreNormalised) {
    Config c = Config::Parse(R"({"loopback_apps": [1, "", " Teams.exe ", "teams.exe", "Zoom.exe", null, "   "]})");
    CHECK(c.loopbackApps == Names({L"Teams.exe", L"Zoom.exe"}));
}

TEST(ConfigParse_KeepAudioDaysIsClamped) {
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": -5})").keepAudioDays, 0);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": 0})").keepAudioDays, 0);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": 36500})").keepAudioDays, 36500);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": 36501})").keepAudioDays, 36500);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": 1e300})").keepAudioDays, 36500);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": -1e300})").keepAudioDays, 0);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": 99999999999999999999})").keepAudioDays, 36500);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": 7.0})").keepAudioDays, 7);
    CHECK_EQ(Config::Parse(R"({"keep_audio_days": 7.9})").keepAudioDays, 7);
}

TEST(ConfigParse_UploadDelayIsClamped) {
    CHECK_EQ(Config::Parse(R"({"upload_delay_seconds": -1})").uploadDelaySeconds, 0);
    CHECK_EQ(Config::Parse(R"({"upload_delay_seconds": 0})").uploadDelaySeconds, 0);
    CHECK_EQ(Config::Parse(R"({"upload_delay_seconds": 3600})").uploadDelaySeconds, 3600);
    CHECK_EQ(Config::Parse(R"({"upload_delay_seconds": 3601})").uploadDelaySeconds, 3600);
    CHECK_EQ(Config::Parse(R"({"upload_delay_seconds": 2147483648})").uploadDelaySeconds, 3600);
    CHECK_EQ(Config::Parse(R"({"upload_delay_seconds": 12.5})").uploadDelaySeconds, 12);
}

TEST(ConfigParse_UnknownKeysAreIgnored) {
    Config c = Config::Parse(R"({"future_setting": {"a": [1]}, "keep_audio_days": 3})");
    CHECK(!c.unreadable);
    CHECK_EQ(c.keepAudioDays, 3);
}

TEST(NormalizeServerUrl_Cases) {
    CHECK_EQ(NormalizeServerUrl(L""), L"");
    CHECK_EQ(NormalizeServerUrl(L"   "), L"");
    CHECK_EQ(NormalizeServerUrl(L"  https://a.example.com  "), L"https://a.example.com");
    CHECK_EQ(NormalizeServerUrl(L"\t\r\nhttps://a.example.com\r\n"), L"https://a.example.com");
    CHECK_EQ(NormalizeServerUrl(L"speakr.example.com"), L"https://speakr.example.com");
    CHECK_EQ(NormalizeServerUrl(L"speakr.example.com:8899"), L"https://speakr.example.com:8899");
    CHECK_EQ(NormalizeServerUrl(L"http://10.0.0.5:8899"), L"http://10.0.0.5:8899");  // http kept as typed
    CHECK_EQ(NormalizeServerUrl(L"https://a.example.com/"), L"https://a.example.com");
    CHECK_EQ(NormalizeServerUrl(L"https://a.example.com///"), L"https://a.example.com");
    CHECK_EQ(NormalizeServerUrl(L"https://a.example.com/api/v1"), L"https://a.example.com");
    CHECK_EQ(NormalizeServerUrl(L"https://a.example.com/api/v1/"), L"https://a.example.com");
    CHECK_EQ(NormalizeServerUrl(L"https://a.example.com/speakr/api/v1"), L"https://a.example.com/speakr");
    CHECK_EQ(NormalizeServerUrl(L"https://a.example.com/speakr/"), L"https://a.example.com/speakr");
    CHECK_EQ(NormalizeServerUrl(L"a.example.com/api/v1"), L"https://a.example.com");
    CHECK_EQ(NormalizeServerUrl(L"HTTPS://A.Example.com"), L"HTTPS://A.Example.com");  // case is not altered
}

TEST(NormalizeServerUrl_IsIdempotent) {
    for (const wchar_t* in : {L"a.example.com", L" https://x/api/v1/ ", L"http://h:1/p/", L""}) {
        std::wstring once = NormalizeServerUrl(in);
        CHECK_EQ(NormalizeServerUrl(once), once);
    }
}

TEST(NormalizeAppList_TrimsDedupesKeepsOrder) {
    Names in = {L" Teams.exe ", L"", L"   ", L"zoom.exe", L"TEAMS.EXE", L"\tSlack.exe\r\n", L"Zoom.exe"};
    CHECK(NormalizeAppList(in) == Names({L"Teams.exe", L"zoom.exe", L"Slack.exe"}));
    CHECK(NormalizeAppList({}).empty());
    CHECK(NormalizeAppList({L"", L" "}).empty());
}
