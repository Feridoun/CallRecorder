#include "TestHarness.h"

#include "Util.h"

static SYSTEMTIME At(WORD y, WORD mo, WORD d, WORD h, WORD mi, WORD s) {
    SYSTEMTIME t{};
    t.wYear = y;
    t.wMonth = mo;
    t.wDay = d;
    t.wHour = h;
    t.wMinute = mi;
    t.wSecond = s;
    return t;
}

TEST(IsoUtc_FormatAndParseRoundTrip) {
    SYSTEMTIME t = At(2026, 9, 30, 13, 2, 15);
    std::wstring text = FormatIsoUtc(t);
    CHECK_EQ(text, L"2026-09-30T13:02:15Z");
    SYSTEMTIME back;
    CHECK(ParseIsoUtc(text, back));
    CHECK(back.wYear == 2026 && back.wMonth == 9 && back.wDay == 30);
    CHECK(back.wHour == 13 && back.wMinute == 2 && back.wSecond == 15);
    CHECK_EQ(FormatIsoUtc(back), text);
}

TEST(IsoUtc_FormatPadsFields) {
    CHECK_EQ(FormatIsoUtc(At(2026, 1, 2, 3, 4, 5)), L"2026-01-02T03:04:05Z");
}

TEST(IsoUtc_ParseRejectsOutOfRangeAndClearsOutput) {
    for (const wchar_t* bad : {L"2026-13-01T00:00:00Z", L"2026-00-01T00:00:00Z", L"2026-01-00T00:00:00Z",
                               L"2026-01-32T00:00:00Z", L"2026-01-01T24:00:00Z", L"2026-01-01T00:60:00Z",
                               L"2026-01-01T00:00:60Z", L"", L"garbage", L"2026-09-30", L"2026-09-30 13:02:15"}) {
        SYSTEMTIME t = At(1999, 1, 1, 1, 1, 1);
        CHECK(!ParseIsoUtc(bad, t));
        CHECK_EQ(t.wYear, 0);  // callers treat year 0 as "unset"
    }
}

TEST(IsoUtc_ParseAcceptsBoundaries) {
    SYSTEMTIME t;
    CHECK(ParseIsoUtc(L"2026-01-01T00:00:00Z", t));
    CHECK(ParseIsoUtc(L"2026-12-31T23:59:59Z", t));
    CHECK(t.wMonth == 12 && t.wDay == 31 && t.wHour == 23 && t.wMinute == 59 && t.wSecond == 59);
}

TEST(IsoUtc_NowFormatsAsParseable) {
    SYSTEMTIME t;
    CHECK(ParseIsoUtc(NowIsoUtc(), t));
    CHECK(t.wYear >= 2024);
}

TEST(DaysSinceIsoUtc_Behaviour) {
    CHECK(DaysSinceIsoUtc(L"garbage") < 0);
    CHECK(DaysSinceIsoUtc(L"") < 0);
    double days = DaysSinceIsoUtc(L"2000-01-01T00:00:00Z");
    CHECK(days > 365.0 * 25);
    CHECK(DaysSinceIsoUtc(NowIsoUtc()) < 0.01);
    // The future is negative-or-tiny days, never an error value confusion.
    CHECK(DaysSinceIsoUtc(L"2999-01-01T00:00:00Z") < 0);
}

TEST(FormatDuration_Cases) {
    CHECK_EQ(FormatDuration(0), L"0:00");
    CHECK_EQ(FormatDuration(5), L"0:05");
    CHECK_EQ(FormatDuration(59.9), L"0:59");
    CHECK_EQ(FormatDuration(60), L"1:00");
    CHECK_EQ(FormatDuration(123), L"2:03");
    CHECK_EQ(FormatDuration(3599), L"59:59");
    CHECK_EQ(FormatDuration(3600), L"1:00:00");
    CHECK_EQ(FormatDuration(3723), L"1:02:03");
    CHECK_EQ(FormatDuration(36000), L"10:00:00");
}

TEST(FormatFileStamp_IsFileNameSafeAndSortable) {
    std::wstring a = FormatFileStamp(At(2026, 9, 30, 12, 0, 0));
    std::wstring b = FormatFileStamp(At(2026, 10, 1, 12, 0, 0));
    CHECK_EQ(a.size(), size_t(17));
    CHECK(IsPlainFileName(a));
    CHECK(a < b);
}

TEST(IsPlainFileName_Cases) {
    CHECK(IsPlainFileName(L"2026-09-30_140215.opus"));
    CHECK(IsPlainFileName(L"a"));
    CHECK(IsPlainFileName(L"name with spaces.opus"));
    CHECK(!IsPlainFileName(L""));
    CHECK(!IsPlainFileName(L".."));
    CHECK(!IsPlainFileName(L"a..b"));  // conservative: any ".." is refused
    CHECK(!IsPlainFileName(L"..\\..\\evil.opus"));
    CHECK(!IsPlainFileName(L"../evil.opus"));
    CHECK(!IsPlainFileName(L"sub\\x.opus"));
    CHECK(!IsPlainFileName(L"sub/x.opus"));
    CHECK(!IsPlainFileName(L"C:\\x.opus"));
    CHECK(!IsPlainFileName(L"C:x.opus"));
    CHECK(!IsPlainFileName(L"x.opus:stream"));
    CHECK(!IsPlainFileName(std::wstring(L"a\nb")));
    CHECK(!IsPlainFileName(std::wstring(L"a\0b", 3)));
    CHECK(!IsPlainFileName(L"a\tb"));
}

TEST(Utf8_RoundTrip) {
    std::wstring w = L"Z\u00fcrich \u65e5\u672c \U0001F600";
    CHECK_EQ(FromUtf8(ToUtf8(w)), w);
    CHECK_EQ(ToUtf8(L""), std::string());
    CHECK_EQ(FromUtf8(""), std::wstring());
    CHECK_EQ(ToUtf8(L"abc"), std::string("abc"));
}

TEST(WriteFileAtomically_WritesReplacesAndLeavesNoTemp) {
    TempDir dir;
    std::wstring path = dir.File(L"x.json");
    CHECK(WriteFileAtomically(path, "first"));
    CHECK_EQ(ReadRaw(path), std::string("first"));
    CHECK(WriteFileAtomically(path, "second, longer"));
    CHECK_EQ(ReadRaw(path), std::string("second, longer"));
    CHECK(WriteFileAtomically(path, "s"));  // shorter: no stale tail
    CHECK_EQ(ReadRaw(path), std::string("s"));
    CHECK(!FileExists(path + L".tmp"));
    CHECK(WriteFileAtomically(path, ""));
    CHECK_EQ(ReadRaw(path), std::string());
}

TEST(WriteFileAtomically_BinaryAndLargeContent) {
    TempDir dir;
    std::string big(3 * 1024 * 1024 + 17, '\0');
    for (size_t i = 0; i < big.size(); ++i) big[i] = static_cast<char>(i * 31);
    CHECK(WriteFileAtomically(dir.File(L"big.bin"), big));
    CHECK(ReadRaw(dir.File(L"big.bin")) == big);
}

TEST(WriteFileAtomically_FailsCleanlyWhenDirectoryMissing) {
    TempDir dir;
    CHECK(!WriteFileAtomically(dir.File(L"nope\\x.json"), "data"));
}

TEST(ReadFileText_StripsBomAndReportsMissing) {
    TempDir dir;
    WriteRaw(dir.File(L"bom.json"), "\xEF\xBB\xBF{\"a\":1}");
    auto text = ReadFileText(dir.File(L"bom.json"));
    CHECK(text.has_value());
    CHECK_EQ(text.value_or(""), std::string("{\"a\":1}"));

    WriteRaw(dir.File(L"plain.json"), "{}");
    CHECK_EQ(ReadFileText(dir.File(L"plain.json")).value_or("?"), std::string("{}"));

    WriteRaw(dir.File(L"empty.json"), "");
    CHECK_EQ(ReadFileText(dir.File(L"empty.json")).value_or("?"), std::string());

    // Only a leading BOM is stripped.
    WriteRaw(dir.File(L"mid.json"), "a\xEF\xBB\xBF");
    CHECK_EQ(ReadFileText(dir.File(L"mid.json")).value_or("?"), std::string("a\xEF\xBB\xBF"));

    CHECK(!ReadFileText(dir.File(L"missing.json")).has_value());
    CHECK_EQ(GetLastError(), DWORD(ERROR_FILE_NOT_FOUND));
}

// KNOWN BUG (fails until fixed): ParseIsoUtc only range-checks day <= 31, so
// impossible calendar dates such as 2026-02-31 are accepted and end up in a
// SYSTEMTIME that SystemTimeToFileTime rejects.
TEST(IsoUtc_ParseRejectsImpossibleCalendarDates) {
    SYSTEMTIME t;
    CHECK(!ParseIsoUtc(L"2026-02-31T00:00:00Z", t));
    CHECK(!ParseIsoUtc(L"2026-04-31T00:00:00Z", t));
    CHECK(!ParseIsoUtc(L"2026-02-29T00:00:00Z", t));  // 2026 is not a leap year
    CHECK(ParseIsoUtc(L"2028-02-29T00:00:00Z", t));
}
