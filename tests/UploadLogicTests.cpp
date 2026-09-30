#include "TestHarness.h"

#include "UploadLogic.h"

using namespace UploadLogic;
using nlohmann::json;
using nlohmann::ordered_json;

TEST(SameServer_IgnoresCaseSlashesAndApiSuffix) {
    CHECK(SameServer(L"https://speakr.example.com", L"https://speakr.example.com"));
    CHECK(SameServer(L"https://Speakr.Example.com", L"https://speakr.example.com"));
    CHECK(SameServer(L"https://speakr.example.com/", L"https://speakr.example.com"));
    CHECK(SameServer(L"https://speakr.example.com/api/v1", L"https://speakr.example.com"));
    CHECK(SameServer(L"speakr.example.com", L"https://speakr.example.com"));
    CHECK(SameServer(L"  https://speakr.example.com  ", L"https://speakr.example.com"));
}

TEST(SameServer_DifferentServersAndEmptyNeverMatch) {
    CHECK(!SameServer(L"https://a.example.com", L"https://b.example.com"));
    CHECK(!SameServer(L"https://a.example.com", L"http://a.example.com"));
    CHECK(!SameServer(L"https://a.example.com:8443", L"https://a.example.com"));
    CHECK(!SameServer(L"https://a.example.com/one", L"https://a.example.com/two"));
    CHECK(!SameServer(L"", L""));  // recorded before setup: never "the same"
    CHECK(!SameServer(L"", L"https://a.example.com"));
    CHECK(!SameServer(L"https://a.example.com", L""));
}

// Regression: emptiness is checked after normalising, so two blank
// (whitespace-only) addresses don't compare equal.
TEST(SameServer_WhitespaceOnlyAddressesNeverMatch) {
    CHECK(!SameServer(L"   ", L"   "));
    CHECK(!SameServer(L" ", L""));
    CHECK(!SameServer(L"	", L"https://a.example.com"));
}

TEST(ParseInstant_KnownValue) {
    // 1970-01-01 is 11644473600 seconds after the FILETIME epoch.
    CHECK(ParseInstant(L"1970-01-01T00:00:00Z") == std::optional<int64_t>(11644473600LL));
    CHECK(ParseInstant(L"1601-01-01T00:00:00Z") == std::optional<int64_t>(0));
}

TEST(ParseInstant_EquivalentSpellingsAgree) {
    auto base = ParseInstant(L"2026-09-30T13:00:00Z");
    CHECK(base.has_value());
    for (const wchar_t* same : {L"2026-09-30T13:00:00Z", L"2026-09-30T13:00:00z", L"2026-09-30T13:00:00",
                                L"2026-09-30 13:00:00", L"2026-09-30 13:00:00Z", L"2026-09-30T13:00Z",
                                L"2026-09-30T13:00:00.000Z", L"2026-09-30T13:00:00.999999Z",
                                L"2026-09-30T13:00:00,5Z", L"2026-09-30T14:00:00+01:00",
                                L"2026-09-30T14:00:00+0100", L"2026-09-30T14:00:00+01",
                                L"2026-09-30T08:00:00-05:00", L"2026-09-30T18:30:00+05:30",
                                L"2026-09-30T13:00:00.5+00:00"}) {
        CHECK(ParseInstant(same) == base);
    }
}

TEST(ParseInstant_OffsetsShiftTheInstant) {
    auto z = ParseInstant(L"2026-09-30T13:00:00Z");
    auto plus = ParseInstant(L"2026-09-30T13:00:00+02:00");
    auto minus = ParseInstant(L"2026-09-30T13:00:00-02:00");
    CHECK(z && plus && minus);
    if (z && plus && minus) {
        CHECK_EQ(*z - *plus, int64_t(2 * 3600));
        CHECK_EQ(*minus - *z, int64_t(2 * 3600));
    }
}

TEST(ParseInstant_RejectsMalformedText) {
    for (const wchar_t* bad : {L"", L" ", L"garbage", L"2026-09-30", L"2026-09-30T", L"2026-09-30T13",
                               L"2026-09-30T13:0", L"26-09-30T13:00:00Z", L"2026/09/30 13:00:00",
                               L"2026-09-30T13:00:00Zjunk", L"2026-09-30T13:00:00 Z", L"2026-09-30T13:00:00+",
                               L"2026-09-30T13:00:00+2", L"2026-09-30T13:00:0a", L"2026-13-01T00:00:00Z",
                               L"2026-00-10T00:00:00Z", L"2026-02-30T00:00:00Z", L"2026-09-30T24:00:00Z",
                               L"2026-09-30T13:60:00Z", L"2026-09-30T13:00:60Z", L"+2026-09-30T13:00:00Z",
                               L"2026-09-30T13:00:00.Z1"}) {
        CHECK(!ParseInstant(bad).has_value());
    }
}

TEST(SameInstant_Cases) {
    CHECK(SameInstant(L"2026-09-30T13:00:00Z", L"2026-09-30T14:00:00+01:00"));
    CHECK(SameInstant(L"2026-09-30T13:00:00", L"2026-09-30 13:00:00.250"));
    CHECK(!SameInstant(L"2026-09-30T13:00:00Z", L"2026-09-30T13:00:01Z"));
    CHECK(!SameInstant(L"2026-09-30T13:00:00Z", L"2026-09-30T13:00:00+01:00"));
    CHECK(!SameInstant(L"", L""));
    CHECK(!SameInstant(L"garbage", L"garbage"));
    CHECK(!SameInstant(L"2026-09-30T13:00:00Z", L"garbage"));
}

TEST(Instants_IsInFutureIsOverdueSecondsUntil) {
    const int64_t now = *ParseInstant(L"2026-09-30T12:00:00Z");
    CHECK(SecondsUntil(L"2026-09-30T12:00:10Z", now) == std::optional<int64_t>(10));
    CHECK(SecondsUntil(L"2026-09-30T11:59:50Z", now) == std::optional<int64_t>(-10));
    CHECK(SecondsUntil(L"2026-09-30T12:00:00Z", now) == std::optional<int64_t>(0));
    CHECK(!SecondsUntil(L"", now).has_value());
    CHECK(!SecondsUntil(L"garbage", now).has_value());

    CHECK(IsInFuture(L"2026-09-30T12:00:01Z", now));
    CHECK(!IsInFuture(L"2026-09-30T12:00:00Z", now));  // grace period over exactly now
    CHECK(!IsInFuture(L"2026-09-30T11:00:00Z", now));
    CHECK(!IsInFuture(L"", now));         // no time: no grace period
    CHECK(!IsInFuture(L"garbage", now));  // unreadable: no grace period

    // Overdue: strictly more than 24 h after the upload.
    CHECK(IsOverdue(L"2026-09-29T11:59:59Z", now));
    CHECK(!IsOverdue(L"2026-09-29T12:00:00Z", now));
    CHECK(!IsOverdue(L"2026-09-30T11:00:00Z", now));
    CHECK(!IsOverdue(L"2026-10-05T00:00:00Z", now));  // future upload time
    CHECK(!IsOverdue(L"", now));
    CHECK(!IsOverdue(L"garbage", now));
    // Time zones count.
    CHECK(IsOverdue(L"2026-09-29T14:00:00+03:00", now));
}

TEST(Instants_DefaultNowIsRealClock) {
    CHECK(IsInFuture(L"2999-01-01T00:00:00Z"));
    CHECK(!IsInFuture(L"2000-01-01T00:00:00Z"));
    CHECK(IsOverdue(L"2000-01-01T00:00:00Z"));
    CHECK(!IsOverdue(NowIsoUtc()));
}

TEST(ClassifyStatus_KnownStatuses) {
    auto completed = ClassifyStatus(json{{"status", "COMPLETED"}});
    CHECK(completed.status == SpeakrStatus::Completed);
    auto failed = ClassifyStatus(json{{"status", "FAILED"}, {"error_message", "boom"}});
    CHECK(failed.status == SpeakrStatus::Failed);
    CHECK_EQ(failed.errorMessage, std::string("boom"));
    auto failedNoMessage = ClassifyStatus(json{{"status", "FAILED"}});
    CHECK(failedNoMessage.status == SpeakrStatus::Failed);
    CHECK_EQ(failedNoMessage.errorMessage, std::string());
    auto failedBadMessage = ClassifyStatus(json{{"status", "FAILED"}, {"error_message", 5}});
    CHECK(failedBadMessage.status == SpeakrStatus::Failed);
    CHECK_EQ(failedBadMessage.errorMessage, std::string());
    auto processing = ClassifyStatus(json{{"status", "PROCESSING"}, {"error_message", "ignored"}});
    CHECK(processing.status == SpeakrStatus::InProgress);
    CHECK_EQ(processing.errorMessage, std::string());
}

TEST(ClassifyStatus_AnythingUnexpectedIsInProgress) {
    for (const json& body : {json{{"status", nullptr}}, json{{"status", 5}}, json{{"status", json::array()}},
                             json{{"status", "completed"}},  // case-sensitive
                             json{{"status", "PENDING"}}, json{{"status", ""}}, json::object(), json(nullptr),
                             json::array({"COMPLETED"}), json("COMPLETED"), json(7)}) {
        CHECK(ClassifyStatus(body).status == SpeakrStatus::InProgress);
    }
    json discarded = json::parse("<html>", nullptr, false);
    CHECK(ClassifyStatus(discarded).status == SpeakrStatus::InProgress);
    CHECK(ClassifyStatus(ordered_json{{"status", "COMPLETED"}}).status == SpeakrStatus::Completed);
}

static json Page(std::initializer_list<json> recordings) {
    return json{{"recordings", json(recordings)}};
}

TEST(FindUploadedMatch_MatchesTitleAndInstant) {
    json page = Page({
        {{"id", 1}, {"title", "Other"}, {"meeting_date", "2026-09-30T13:00:00"}},
        {{"id", 2}, {"title", "Recording 30 Sep 2026 14:00"}, {"meeting_date", "2026-09-30T13:00:00"}},
    });
    CHECK_EQ(FindUploadedMatch(page, "Recording 30 Sep 2026 14:00", L"2026-09-30T13:00:00Z"), 2);
    // Same instant expressed with an offset on our side.
    CHECK_EQ(FindUploadedMatch(page, "Recording 30 Sep 2026 14:00", L"2026-09-30T14:00:00+01:00"), 2);
}

TEST(FindUploadedMatch_WrongOrMissingFieldsGiveZero) {
    const std::wstring when = L"2026-09-30T13:00:00Z";
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "T"}, {"meeting_date", "2026-09-30T13:00:01"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "T"}, {"meeting_date", "2026-10-01T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "t"}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "T"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"title", "T"}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 0}, {"title", "T"}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", -4}, {"title", "T"}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", "7"}, {"title", "T"}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", 5}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "T"}, {"meeting_date", 20260930}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "T"}, {"meeting_date", nullptr}}}), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "T"}, {"meeting_date", "not a date"}}}), "T", when), 0);
    // Our own date unparseable: never match.
    CHECK_EQ(FindUploadedMatch(Page({{{"id", 1}, {"title", "T"}, {"meeting_date", "2026-09-30T13:00:00"}}}), "T", L""), 0);
    CHECK_EQ(FindUploadedMatch(Page({}), "T", when), 0);
}

TEST(FindUploadedMatch_NonArrayOrNonObjectPagesGiveZero) {
    const std::wstring when = L"2026-09-30T13:00:00Z";
    CHECK_EQ(FindUploadedMatch(json{{"recordings", "x"}}, "T", when), 0);
    CHECK_EQ(FindUploadedMatch(json{{"recordings", nullptr}}, "T", when), 0);
    CHECK_EQ(FindUploadedMatch(json{{"recordings", json::object()}}, "T", when), 0);
    CHECK_EQ(FindUploadedMatch(json::object(), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(json::array(), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(json(nullptr), "T", when), 0);
    CHECK_EQ(FindUploadedMatch(json("s"), "T", when), 0);
    // Non-object entries are skipped, later valid ones still match.
    json page = Page({json(1), json(nullptr), json::array({1}),
                      {{"id", 9}, {"title", "T"}, {"meeting_date", "2026-09-30T13:00:00"}}});
    CHECK_EQ(FindUploadedMatch(page, "T", when), 9);
}

TEST(FindUploadedMatch_UnicodeTitleAndFirstMatchWins) {
    std::string title = "R\xC3\xA9sum\xC3\xA9 \xE6\x97\xA5\xE6\x9C\xAC";  // UTF-8
    json page = Page({{{"id", 3}, {"title", title}, {"meeting_date", "2026-09-30T13:00:00"}},
                      {{"id", 4}, {"title", title}, {"meeting_date", "2026-09-30T13:00:00"}}});
    CHECK_EQ(FindUploadedMatch(page, title, L"2026-09-30T13:00:00Z"), 3);
}

TEST(ListHasNext_Cases) {
    CHECK(ListHasNext(json{{"pagination", {{"has_next", true}}}}));
    CHECK(!ListHasNext(json{{"pagination", {{"has_next", false}}}}));
    CHECK(!ListHasNext(json{{"pagination", {{"has_next", "true"}}}}));
    CHECK(!ListHasNext(json{{"pagination", {{"has_next", 1}}}}));
    CHECK(!ListHasNext(json{{"pagination", {{"has_next", nullptr}}}}));
    CHECK(!ListHasNext(json{{"pagination", json::object()}}));
    CHECK(!ListHasNext(json{{"pagination", "x"}}));
    CHECK(!ListHasNext(json{{"pagination", nullptr}}));
    CHECK(!ListHasNext(json{{"pagination", json::array({true})}}));
    CHECK(!ListHasNext(json::object()));
    CHECK(!ListHasNext(json::array()));
    CHECK(!ListHasNext(json(nullptr)));
    CHECK(!ListHasNext(json(true)));
}
