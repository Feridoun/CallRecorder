#include "TestHarness.h"

#include "CallLogic.h"

using namespace CallLogic;
using Apps = std::vector<std::wstring>;

TEST(CallApps_FoundByExeIgnoringCase) {
    CHECK(FindByExe(L"Zoom.exe") && FindByExe(L"Zoom.exe")->name == L"Zoom");
    CHECK(FindByExe(L"ZOOM.EXE") && FindByExe(L"ZOOM.EXE")->name == L"Zoom");
    CHECK(FindByExe(L"ms-teams.exe") && FindByExe(L"ms-teams.exe")->name == L"Teams");
    CHECK(FindByExe(L"Teams.exe") && FindByExe(L"Teams.exe")->name == L"Teams");
    CHECK(FindByExe(L"8x8 Work.exe") && FindByExe(L"8x8 Work.exe")->name == L"8x8");
    CHECK(FindByExe(L"notepad.exe") == nullptr);
    CHECK(FindByExe(L"") == nullptr);  // the table's unused slots are empty, not matches
}

TEST(CallApps_BrowsersAreNotCallAppsOnTheirOwn) {
    CHECK(IsBrowser(L"chrome.exe"));
    CHECK(IsBrowser(L"MSEDGE.EXE"));
    CHECK(!IsBrowser(L"Zoom.exe"));
    CHECK(FindByExe(L"chrome.exe") == nullptr);
}

TEST(MicUserOf_TheProcessOrItsParents) {
    std::vector<Process> processes = {
        {4, 0, L"System"},
        {100, 4, L"explorer.exe"},
        {200, 100, L"ms-teams.exe"},
        {210, 200, L"msedgewebview2.exe"},
        {211, 210, L"msedgewebview2.exe"},
        {300, 100, L"chrome.exe"},
        {310, 300, L"chrome.exe"},
        {400, 100, L"obs64.exe"},
        {500, 100, L"Zoom.exe"},
    };
    CHECK(MicUserOf(500, processes).app && MicUserOf(500, processes).app->name == L"Zoom");
    CHECK(MicUserOf(211, processes).app && MicUserOf(211, processes).app->name == L"Teams");  // a helper's helper
    CHECK(MicUserOf(310, processes).app == nullptr);
    CHECK_EQ(MicUserOf(310, processes).browser, L"chrome.exe");
    CHECK(MicUserOf(400, processes).app == nullptr);  // not a call app
    CHECK(MicUserOf(400, processes).browser.empty());
    CHECK(MicUserOf(999, processes).app == nullptr);  // exited since
}

TEST(MicUserOf_StopsOnLoopsAndDeepTrees) {
    std::vector<Process> loop = {{10, 11, L"a.exe"}, {11, 10, L"b.exe"}};
    CHECK(MicUserOf(10, loop).app == nullptr);
    std::vector<Process> self = {{10, 10, L"a.exe"}};
    CHECK(MicUserOf(10, self).app == nullptr);
    // Too far down to trust: parent ids can be reused by unrelated processes.
    std::vector<Process> deep = {{1, 0, L"Zoom.exe"}, {2, 1, L"x.exe"}, {3, 2, L"x.exe"}, {4, 3, L"x.exe"},
                                 {5, 4, L"x.exe"}};
    CHECK(MicUserOf(4, deep).app != nullptr);
    CHECK(MicUserOf(5, deep).app == nullptr);
}

TEST(TeamsSubject_MeetingWindowNamesTheMeeting) {
    CHECK_EQ(TeamsSubject(L"Weekly sync | Microsoft Teams"), L"Weekly sync");
    CHECK_EQ(TeamsSubject(L"Meeting compact view | Weekly sync | Microsoft Teams"), L"Weekly sync");
    CHECK_EQ(TeamsSubject(L"Budget: Q3 | review | Meeting | Microsoft Teams"), L"");  // ambiguous: two names
    CHECK_EQ(TeamsSubject(L"Weekly sync | Microsoft Teams - Google Chrome"), L"Weekly sync");
    CHECK_EQ(TeamsSubject(L"Weekly sync | Microsoft Teams - Personal - Microsoft​ Edge"), L"Weekly sync");
}

TEST(TeamsSubject_MainWindowAndSectionsAreIgnored) {
    CHECK_EQ(TeamsSubject(L"Microsoft Teams"), L"");
    CHECK_EQ(TeamsSubject(L"Chat | Anika | Microsoft Teams"), L"");
    CHECK_EQ(TeamsSubject(L"Calendar | Microsoft Teams"), L"");
    CHECK_EQ(TeamsSubject(L"Activity | Microsoft Teams"), L"");
    CHECK_EQ(TeamsSubject(L"Meeting | Microsoft Teams"), L"");
    CHECK_EQ(TeamsSubject(L"Weekly sync"), L"");
    CHECK_EQ(TeamsSubject(L""), L"");
}

TEST(WebCall_GoogleMeet) {
    WebCall named = WebCallFromTitle(L"Meet - Weekly sync - Google Chrome");
    CHECK(named.found);
    CHECK(named.app == L"Google Meet");
    CHECK_EQ(named.subject, L"Weekly sync");

    WebCall code = WebCallFromTitle(L"Meet – abc-defg-hij – Mozilla Firefox");
    CHECK(code.found);
    CHECK_EQ(code.subject, L"");  // a meeting code says nothing about the meeting

    CHECK(!WebCallFromTitle(L"Google Meet - Google Chrome").found);  // the landing page
    CHECK(!WebCallFromTitle(L"Meeting notes - Google Docs - Google Chrome").found);
}

TEST(WebCall_TeamsOnlyWhileShowingAMeeting) {
    WebCall meeting = WebCallFromTitle(L"Weekly sync | Microsoft Teams - Google Chrome");
    CHECK(meeting.found);
    CHECK(meeting.app == L"Teams");
    CHECK_EQ(meeting.subject, L"Weekly sync");
    CHECK(!WebCallFromTitle(L"Chat | Anika | Microsoft Teams - Google Chrome").found);
    CHECK(!WebCallFromTitle(L"YouTube - Google Chrome").found);
}

TEST(IsMeetCode_Shape) {
    CHECK(IsMeetCode(L"abc-defg-hij"));
    CHECK(!IsMeetCode(L"abc-defg-hi"));
    CHECK(!IsMeetCode(L"ABC-DEFG-HIJ"));
    CHECK(!IsMeetCode(L"abcdefghijkl"));
}

TEST(CallTitle_NamesTheMeetingOrTheApp) {
    CHECK_EQ(CallTitle(L"Teams", L"Weekly sync", L"30 Sep 2026 14:02"), L"Weekly sync (Teams)");
    CHECK_EQ(CallTitle(L"Zoom", L"", L"30 Sep 2026 14:02"), L"Zoom call 30 Sep 2026 14:02");
    CHECK_EQ(CallTitle(L"Zoom", L"   ", L"30 Sep 2026 14:02"), L"Zoom call 30 Sep 2026 14:02");
    CHECK_EQ(CallTitle(L"Teams", L"A\tB\nC", L"x"), L"A B C (Teams)");
    std::wstring longTitle = CallTitle(L"Teams", std::wstring(300, L'x'), L"x");
    CHECK(longTitle.size() == 120 + 3 + 8);  // cut, "...", " (Teams)"
    CHECK(longTitle.ends_with(L"... (Teams)"));
}

TEST(CallTracker_StartsAfterBeingSeenTwice) {
    CallTracker tracker;
    CHECK(tracker.Update({L"Teams"}, 1000).empty());
    CHECK(!tracker.IsActive(L"Teams"));
    auto events = tracker.Update({L"Teams"}, 3000);
    CHECK_EQ(events.size(), size_t(1));
    CHECK(events[0].app == L"Teams" && events[0].started);
    CHECK(tracker.IsActive(L"Teams"));
    CHECK(tracker.Active() == Apps({L"Teams"}));
    CHECK(tracker.Update({L"Teams"}, 5000).empty());  // no repeat
}

TEST(CallTracker_BriefMicrophoneUseIsNotACall) {
    CallTracker tracker;
    CHECK(tracker.Update({L"Zoom"}, 1000).empty());
    CHECK(tracker.Update({}, 3000).empty());  // gone before it counted: no start, no end
    CHECK(tracker.Update({L"Zoom"}, 5000).empty());  // seen afresh: the wait starts again
    CHECK(!tracker.IsActive(L"Zoom"));
}

TEST(CallTracker_EndsOnlyAfterBeingGoneAWhile) {
    CallTracker tracker;
    tracker.Update({L"Teams"}, 0);
    tracker.Update({L"Teams"}, 2000);
    CHECK(tracker.Update({}, 4000).empty());  // e.g. switching headsets
    CHECK(tracker.Update({}, 10000).empty());
    CHECK(tracker.IsActive(L"Teams"));
    CHECK(tracker.Update({L"Teams"}, 12000).empty());  // back: still the same call
    CHECK(tracker.Update({}, 14000).empty());
    auto events = tracker.Update({}, 22000);
    CHECK_EQ(events.size(), size_t(1));
    CHECK(events[0].app == L"Teams" && !events[0].started);
    CHECK(!tracker.IsActive(L"Teams"));
    CHECK(tracker.Update({}, 24000).empty());  // ended once
}

TEST(CallTracker_TracksAppsSeparately) {
    CallTracker tracker;
    tracker.Update({L"Teams"}, 0);
    tracker.Update({L"Teams", L"Zoom"}, 2000);
    auto events = tracker.Update({L"Teams", L"Zoom"}, 4000);
    CHECK_EQ(events.size(), size_t(1));
    CHECK(events[0].app == L"Zoom" && events[0].started);
    CHECK(tracker.Active() == Apps({L"Teams", L"Zoom"}));
    tracker.Clear();
    CHECK(tracker.Active().empty());
}
