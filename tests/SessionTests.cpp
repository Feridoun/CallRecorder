// Session persistence in throwaway directories under %TEMP%.
#include "TestHarness.h"

#include "Session.h"
#include "Util.h"

#include <algorithm>

using Names = std::vector<std::wstring>;

static Session MakeFullSession(const TempDir& dir) {
    Session s = Session::Create(dir.Path(), false, {L"Call", L"Z\u00fcrich"}, L"https://speakr.example.com");
    s.title = L"Weekly sync \u65e5\u672c";
    SYSTEMTIME ended = s.startedUtc;
    ended.wSecond = (ended.wSecond + 5) % 60;
    s.Finish(123.45);
    s.markers = {{12.34, L"Decision"}, {60.0, L""}};
    s.uploadState = UploadState::kUploaded;
    s.uploadError = L"earlier error";
    s.speakrId = 4711;
    s.uploadedUtc = L"2026-09-30T13:00:00Z";
    s.finishedUtc = L"2026-09-30T13:05:00Z";
    s.uploadAttemptUtc = L"2026-09-30T12:59:00Z";
    s.uploadAfterUtc = L"2026-09-30T12:58:00Z";
    s.audioDeleted = true;
    return s;
}

TEST(Session_SaveLoadRoundTripsEveryField) {
    TempDir dir;
    Session s = MakeFullSession(dir);
    CHECK(s.Save());

    auto loaded = Session::Load(s.metaPath);
    CHECK(loaded.has_value());
    if (!loaded) return;
    CHECK_EQ(loaded->id, s.id);
    CHECK_EQ(loaded->metaPath, s.metaPath);
    CHECK_EQ(loaded->audioPath, s.audioPath);
    CHECK_EQ(loaded->title, s.title);
    CHECK_EQ(FormatIsoUtc(loaded->startedUtc), FormatIsoUtc(s.startedUtc));
    CHECK_EQ(FormatIsoUtc(loaded->endedUtc), FormatIsoUtc(s.endedUtc));
    CHECK_NEAR(loaded->durationSeconds, 123.5, 1e-9);  // rounded to 0.1 s on save
    CHECK_EQ(loaded->sensitive, false);
    CHECK_EQ(loaded->status, std::string("recorded"));
    CHECK_EQ(loaded->markers.size(), size_t(2));
    if (loaded->markers.size() == 2) {
        CHECK_NEAR(loaded->markers[0].offsetSeconds, 12.3, 1e-9);
        CHECK_EQ(loaded->markers[0].label, L"Decision");
        CHECK_NEAR(loaded->markers[1].offsetSeconds, 60.0, 1e-9);
        CHECK_EQ(loaded->markers[1].label, L"");
    }
    CHECK(loaded->tags.has_value());
    CHECK(loaded->tags.value_or(Names()) == Names({L"Call", L"Z\u00fcrich"}));
    CHECK(loaded->serverUrl.has_value());
    CHECK_EQ(loaded->serverUrl.value_or(L"?"), L"https://speakr.example.com");
    CHECK_EQ(loaded->uploadState, std::string(UploadState::kUploaded));
    CHECK_EQ(loaded->uploadError, L"earlier error");
    CHECK_EQ(loaded->speakrId, 4711);
    CHECK_EQ(loaded->uploadedUtc, L"2026-09-30T13:00:00Z");
    CHECK_EQ(loaded->finishedUtc, L"2026-09-30T13:05:00Z");
    CHECK_EQ(loaded->uploadAttemptUtc, L"2026-09-30T12:59:00Z");
    CHECK_EQ(loaded->uploadAfterUtc, L"2026-09-30T12:58:00Z");
    CHECK_EQ(loaded->audioDeleted, true);
    CHECK(!FileExists(s.metaPath + L".tmp"));
}

TEST(Session_ServerUrlAbsentEmptyAndSetAreDistinct) {
    TempDir dir;
    // Set
    Session set = Session::Create(dir.Path(), false, {}, L"https://a.example.com");
    CHECK(set.Save());
    auto loadedSet = Session::Load(set.metaPath);
    CHECK(loadedSet && loadedSet->serverUrl.has_value() && *loadedSet->serverUrl == L"https://a.example.com");

    // Empty: recorded before Speakr was set up
    Session empty = Session::Create(dir.Path(), false, {}, L"");
    empty.id += L"_e";
    empty.metaPath = dir.File(empty.id + L".json");
    CHECK(empty.Save());
    auto loadedEmpty = Session::Load(empty.metaPath);
    CHECK(loadedEmpty && loadedEmpty->serverUrl.has_value() && loadedEmpty->serverUrl->empty());

    // Absent: pre-1.2 sidecar
    Session absent = Session::Create(dir.Path(), false, {}, L"x");
    absent.id += L"_a";
    absent.metaPath = dir.File(absent.id + L".json");
    absent.serverUrl.reset();
    CHECK(absent.Save());
    auto loadedAbsent = Session::Load(absent.metaPath);
    CHECK(loadedAbsent && !loadedAbsent->serverUrl.has_value());
    CHECK(ReadRaw(absent.metaPath).find("server_url") == std::string::npos);
}

TEST(Session_TagsAbsentVersusEmptyAreDistinct) {
    TempDir dir;
    Session none = Session::Create(dir.Path(), false, {}, L"u");
    none.tags.reset();
    CHECK(none.Save());
    auto a = Session::Load(none.metaPath);
    CHECK(a && !a->tags.has_value());

    Session empty = Session::Create(dir.Path(), false, {}, L"u");
    empty.id += L"_b";
    empty.metaPath = dir.File(empty.id + L".json");
    CHECK(empty.Save());
    auto b = Session::Load(empty.metaPath);
    CHECK(b && b->tags.has_value() && b->tags->empty());
}

TEST(Session_CreateSetsDefaults) {
    TempDir dir;
    Session s = Session::Create(dir.Path(), false, {L"Call"}, L"https://a");
    CHECK_EQ(s.status, std::string("recording"));
    CHECK_EQ(s.uploadState, std::string(UploadState::kPending));
    CHECK_EQ(s.sensitive, false);
    CHECK_EQ(s.audioPath, dir.File(s.id + L".opus"));
    CHECK_EQ(s.metaPath, dir.File(s.id + L".json"));
    CHECK(s.title.rfind(L"Recording ", 0) == 0);
    CHECK(s.startedUtc.wYear >= 2024);

    Session sensitive = Session::Create(dir.Path(), true, {}, L"");
    CHECK_EQ(sensitive.sensitive, true);
    CHECK_EQ(sensitive.uploadState, std::string(UploadState::kLocalOnly));
    sensitive.SetSensitive(false);
    CHECK_EQ(sensitive.uploadState, std::string(UploadState::kPending));
}

TEST(Session_CreatePicksUniqueIdWhenFilesExist) {
    TempDir dir;
    Session first = Session::Create(dir.Path(), false, {}, L"u");
    // Same-second creation: occupy the stamp with only an .opus, then only a .json.
    WriteRaw(first.audioPath, "audio");
    Session second = Session::Create(dir.Path(), false, {}, L"u");
    // The clock may have ticked over to the next second between calls, in
    // which case the stamp is already unique; either way it must not collide.
    CHECK(second.id != first.id);
    CHECK(!FileExists(second.audioPath));
    CHECK(!FileExists(second.metaPath));

    WriteRaw(second.metaPath, "{}");
    Session third = Session::Create(dir.Path(), false, {}, L"u");
    CHECK(third.id != first.id);
    CHECK(third.id != second.id);
    CHECK(!FileExists(third.audioPath));
    CHECK(!FileExists(third.metaPath));
}

TEST(Session_CreateSuffixesTheStampWhenTaken) {
    // Occupy this second and the next two, so a suffix is forced unless the
    // test is stalled for more than two seconds.
    TempDir dir;
    FILETIME file;
    GetSystemTimeAsFileTime(&file);
    ULARGE_INTEGER ticks{{file.dwLowDateTime, file.dwHighDateTime}};
    std::vector<std::wstring> taken;
    for (int i = 0; i < 3; ++i) {
        ULARGE_INTEGER t = ticks;
        t.QuadPart += static_cast<ULONGLONG>(i) * 10'000'000;
        FILETIME f{t.LowPart, t.HighPart};
        SYSTEMTIME st;
        FileTimeToSystemTime(&f, &st);
        taken.push_back(FormatFileStamp(st));
        WriteRaw(dir.File(taken.back() + L".json"), "{}");
    }
    Session s = Session::Create(dir.Path(), false, {}, L"u");
    CHECK(std::find(taken.begin(), taken.end(), s.id) == taken.end());
    CHECK(s.id.find(L"_2") == 17);
}

TEST(Session_LoadOfMalformedSidecarsNeverThrows) {
    TempDir dir;
    const char* docs[] = {
        "", "garbage", "[]", "null", "5", "{}", R"({"id": 5})", R"({"id": null})", R"({"id": ""})",
        R"({"id":"x","title":5,"audio_file":5,"started_utc":5,"ended_utc":[],"duration_seconds":"9",
            "sensitive":"yes","status":5,"markers":5,"tags":5,"server_url":5,"upload_state":5,
            "upload_error":5,"speakr_id":"7","uploaded_utc":5,"finished_utc":{},"upload_attempt_utc":[],
            "upload_after_utc":true,"audio_deleted":"y"})",
        R"({"id":"x","markers":[1,"a",null,[],{"offset_seconds":"x","label":5},{}]})",
        R"({"id":"x","tags":[1,null,"ok",{}]})",
        R"({"id":"x","speakr_id":1e300})", R"({"id":"x","speakr_id":-5})", R"({"id":"x","speakr_id":99999999999999999999})",
    };
    int n = 0;
    for (const char* doc : docs) {
        std::wstring path = dir.File(L"s" + std::to_wstring(n++) + L".json");
        WriteRaw(path, doc);
        CHECK_NOTHROW(Session::Load(path));
    }
    CHECK(!Session::Load(dir.File(L"missing.json")).has_value());
    CHECK(!Session::Load(dir.File(L"s0.json")).has_value());  // empty
    CHECK(!Session::Load(dir.File(L"s3.json")).has_value());  // null
    CHECK(!Session::Load(dir.File(L"s5.json")).has_value());  // no id
    CHECK(!Session::Load(dir.File(L"s6.json")).has_value());  // id not a string
}

TEST(Session_LoadWithWrongTypesEverywhereUsesDefaults) {
    TempDir dir;
    WriteRaw(dir.File(L"w.json"),
             R"({"id":"x","title":5,"audio_file":5,"started_utc":5,"duration_seconds":"9",
                 "sensitive":"yes","status":5,"markers":5,"tags":5,"server_url":5,"upload_state":5,
                 "speakr_id":"7","audio_deleted":"y"})");
    auto s = Session::Load(dir.File(L"w.json"));
    CHECK(s.has_value());
    if (!s) return;
    CHECK_EQ(s->title, L"");
    CHECK_EQ(s->audioPath, dir.File(L"x.opus"));
    CHECK_EQ(s->startedUtc.wYear, 0);
    CHECK_EQ(s->durationSeconds, 0.0);
    CHECK_EQ(s->sensitive, false);
    CHECK_EQ(s->status, std::string());
    CHECK(s->markers.empty());
    CHECK(!s->tags.has_value());
    // A server_url of the wrong type counts as "none": held, not uploaded.
    CHECK(s->serverUrl.has_value() && s->serverUrl->empty());
    CHECK_EQ(s->uploadState, std::string(UploadState::kPending));
    CHECK_EQ(s->speakrId, 0);
    CHECK_EQ(s->audioDeleted, false);
}

TEST(Session_LoadDefaultsUploadStateFromSensitiveFlag) {
    TempDir dir;
    WriteRaw(dir.File(L"a.json"), R"({"id":"a","sensitive":true})");
    WriteRaw(dir.File(L"b.json"), R"({"id":"b","sensitive":false})");
    CHECK_EQ(Session::Load(dir.File(L"a.json"))->uploadState, std::string(UploadState::kLocalOnly));
    CHECK_EQ(Session::Load(dir.File(L"b.json"))->uploadState, std::string(UploadState::kPending));
}

TEST(Session_LoadMarkersSkipsNonObjects) {
    TempDir dir;
    WriteRaw(dir.File(L"m.json"), R"({"id":"m","markers":[1,"a",null,[],{"offset_seconds":5.5,"label":"ok"},{}]})");
    auto s = Session::Load(dir.File(L"m.json"));
    CHECK(s.has_value());
    if (!s) return;
    CHECK_EQ(s->markers.size(), size_t(2));
    CHECK_NEAR(s->markers[0].offsetSeconds, 5.5, 1e-9);
    CHECK_EQ(s->markers[0].label, L"ok");
    CHECK_EQ(s->markers[1].offsetSeconds, 0.0);
}

TEST(Session_LoadKeepsAudioPathInsideDirectory) {
    TempDir dir;
    for (const char* evil : {"..\\\\..\\\\evil.opus", "C:\\\\x.opus", "../evil.opus", "sub\\\\x.opus", "", "x:y.opus"}) {
        std::string doc = std::string(R"({"id":"abc","audio_file":")") + evil + R"("})";
        WriteRaw(dir.File(L"abc.json"), doc);
        auto s = Session::Load(dir.File(L"abc.json"));
        CHECK(s.has_value());
        if (s) CHECK_EQ(s->audioPath, dir.File(L"abc.opus"));
    }
    WriteRaw(dir.File(L"abc.json"), R"({"id":"abc","audio_file":"custom.opus"})");
    CHECK_EQ(Session::Load(dir.File(L"abc.json"))->audioPath, dir.File(L"custom.opus"));
}

TEST(Session_LoadFallsBackToFileStemWhenIdIsUnsafe) {
    TempDir dir;
    WriteRaw(dir.File(L"stem.json"), R"({"id":"..\\..\\evil"})");
    auto s = Session::Load(dir.File(L"stem.json"));
    CHECK(s.has_value());
    if (s) CHECK_EQ(s->audioPath, dir.File(L"stem.opus"));
}

TEST(Session_LoadStripsBom) {
    TempDir dir;
    WriteRaw(dir.File(L"bom.json"), "\xEF\xBB\xBF{\"id\":\"bom\"}");
    CHECK(Session::Load(dir.File(L"bom.json")).has_value());
}

TEST(Session_DiscardDeletesBothFiles) {
    TempDir dir;
    Session s = Session::Create(dir.Path(), false, {}, L"u");
    CHECK(s.Save());
    WriteRaw(s.audioPath, "audio");
    CHECK(FileExists(s.audioPath) && FileExists(s.metaPath));
    s.Discard();
    CHECK(!FileExists(s.audioPath));
    CHECK(!FileExists(s.metaPath));
    s.Discard();  // already gone: harmless
}

TEST(Session_ListMetaFilesIgnoresBadAndTmpAndSorts) {
    TempDir dir;
    WriteRaw(dir.File(L"2026-02-01_100000.json"), "{}");
    WriteRaw(dir.File(L"2026-01-01_100000.json"), "{}");
    WriteRaw(dir.File(L"2026-03-01_100000.json.bad"), "{}");
    WriteRaw(dir.File(L"2026-04-01_100000.json.tmp"), "{}");
    WriteRaw(dir.File(L"2026-05-01_100000.opus"), "x");
    WriteRaw(dir.File(L".json"), "{}");  // no stem: not a sidecar
    auto list = Session::ListMetaFiles(dir.Path());
    CHECK_EQ(list.size(), size_t(2));
    if (list.size() == 2) {
        CHECK_EQ(list[0], dir.File(L"2026-01-01_100000.json"));
        CHECK_EQ(list[1], dir.File(L"2026-02-01_100000.json"));
    }
    TempDir empty;
    CHECK(Session::ListMetaFiles(empty.Path()).empty());
    CHECK(Session::ListMetaFiles(dir.File(L"nonexistent")).empty());
}

TEST(Session_RecoverInterruptedMarksRecordingSidecars) {
    TempDir dir;
    Session s = Session::Create(dir.Path(), false, {L"Call"}, L"https://a");
    CHECK(s.Save());  // status "recording"
    WriteRaw(s.audioPath, "audio");

    Session done = Session::Create(dir.Path(), false, {}, L"https://a");
    done.id += L"_2";
    done.metaPath = dir.File(done.id + L".json");
    done.audioPath = dir.File(done.id + L".opus");
    done.Finish(10);
    CHECK(done.Save());
    WriteRaw(done.audioPath, "audio");

    CHECK_EQ(Session::RecoverInterrupted(dir.Path()), 1);
    auto a = Session::Load(s.metaPath);
    CHECK(a && a->status == "interrupted");
    if (a) {
        // Everything else is preserved.
        CHECK(a->serverUrl.has_value() && *a->serverUrl == L"https://a");
        CHECK(a->tags.has_value() && a->tags->size() == 1);
        CHECK_EQ(a->uploadState, std::string(UploadState::kPending));
    }
    auto b = Session::Load(done.metaPath);
    CHECK(b && b->status == "recorded");
    // Idempotent.
    CHECK_EQ(Session::RecoverInterrupted(dir.Path()), 0);
}

TEST(Session_RecoverInterruptedCreatesSidecarForOrphanOpus) {
    TempDir dir;
    WriteRaw(dir.File(L"orphan.opus"), "audio");
    CHECK_EQ(Session::RecoverInterrupted(dir.Path()), 1);
    auto s = Session::Load(dir.File(L"orphan.json"));
    CHECK(s.has_value());
    if (!s) return;
    CHECK_EQ(s->id, L"orphan");
    CHECK_EQ(s->status, std::string("interrupted"));
    CHECK_EQ(s->sensitive, true);
    CHECK_EQ(s->uploadState, std::string(UploadState::kLocalOnly));
    CHECK(!s->uploadError.empty());
    CHECK(s->startedUtc.wYear >= 2024);
    CHECK(!s->serverUrl.has_value());
    CHECK_EQ(s->audioPath, dir.File(L"orphan.opus"));
    CHECK_EQ(Session::RecoverInterrupted(dir.Path()), 0);
}

TEST(Session_RecoverInterruptedSetsAsideGarbageSidecar) {
    TempDir dir;
    WriteRaw(dir.File(L"broken.opus"), "audio");
    WriteRaw(dir.File(L"broken.json"), "{ this is not json");
    CHECK_EQ(Session::RecoverInterrupted(dir.Path()), 1);
    CHECK(FileExists(dir.File(L"broken.json.bad")));
    CHECK_EQ(ReadRaw(dir.File(L"broken.json.bad")), std::string("{ this is not json"));
    auto s = Session::Load(dir.File(L"broken.json"));
    CHECK(s.has_value());
    if (s) {
        CHECK_EQ(s->uploadState, std::string(UploadState::kLocalOnly));
        CHECK_EQ(s->status, std::string("interrupted"));
    }
    // The .bad file is not listed as a sidecar.
    CHECK_EQ(Session::ListMetaFiles(dir.Path()).size(), size_t(1));
}

TEST(Session_RecoverInterruptedTreatsSidecarWithoutIdAsGarbage) {
    TempDir dir;
    WriteRaw(dir.File(L"noid.opus"), "audio");
    WriteRaw(dir.File(L"noid.json"), "{}");
    CHECK_EQ(Session::RecoverInterrupted(dir.Path()), 1);
    CHECK(FileExists(dir.File(L"noid.json.bad")));
}

TEST(Session_RecoverInterruptedOnMissingDirectoryIsHarmless) {
    TempDir dir;
    CHECK_EQ(Session::RecoverInterrupted(dir.File(L"nope")), 0);
}

TEST(Session_MarkerOffsetsRoundToOneDecimal) {
    TempDir dir;
    Session s = Session::Create(dir.Path(), false, {}, L"u");
    s.markers = {{0.04, L"a"}, {0.05, L"b"}, {1234.56, L"c"}};
    CHECK(s.Save());
    auto l = Session::Load(s.metaPath);
    CHECK(l && l->markers.size() == 3);
    if (l && l->markers.size() == 3) {
        CHECK_NEAR(l->markers[0].offsetSeconds, 0.0, 1e-9);
        CHECK_NEAR(l->markers[2].offsetSeconds, 1234.6, 1e-9);
    }
}

TEST(Session_SaveToMissingDirectoryFails) {
    TempDir dir;
    Session s = Session::Create(dir.File(L"nope"), false, {}, L"u");
    CHECK(!s.Save());
}
