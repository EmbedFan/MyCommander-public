#include "TestFramework.h"

#include "HistorySearch.h"

#include <windows.h>

using namespace mc;

namespace {
const std::vector<std::wstring> kHistory = {L"dir C:\\temp", L"robocopy a b /MIR", L"git status", L"ROBOCOPY c d",
                                            L"echo \u0151ri"};
}

TEST_CASE(HistorySearch_NewestFirstAndStepping_CLI008) {
    auto m = FindHistoryMatch(kHistory, L"robo", 4);
    CHECK(m.has_value() && *m == 3);
    m = FindHistoryMatch(kHistory, L"robo", *m - 1);  // Alt+R again: the next older one
    CHECK(m.has_value() && *m == 1);
    m = FindHistoryMatch(kHistory, L"robo", *m - 1);  // 0 is "dir ..." : no more
    CHECK(!m.has_value());
}

TEST_CASE(HistorySearch_CaseInsensitiveAndUnicode_CLI008) {
    auto m = FindHistoryMatch(kHistory, L"GIT", 4);
    CHECK(m.has_value() && *m == 2);
    m = FindHistoryMatch(kHistory, L"\u0150R", 4);  // upper-case O-double-acute matches the lower-case one
    CHECK(m.has_value() && *m == 4);
}

TEST_CASE(HistorySearch_EmptyTextMatchesStartEntry_CLI008) {
    auto m = FindHistoryMatch(kHistory, L"", 4);
    CHECK(m.has_value() && *m == 4);
    m = FindHistoryMatch(kHistory, L"", 100);  // clamped
    CHECK(m.has_value() && *m == 4);
    CHECK(!FindHistoryMatch({}, L"", 0).has_value());
    CHECK(!FindHistoryMatch(kHistory, L"nothing like this", 4).has_value());
}

TEST_CASE(HistorySearch_SubstringNotPrefix_CLI008) {
    auto m = FindHistoryMatch(kHistory, L"status", 4);
    CHECK(m.has_value() && *m == 2);
    m = FindHistoryMatch(kHistory, L"temp", 4);
    CHECK(m.has_value() && *m == 0);
}

TEST_CASE(HistorySearch_ClassifyKeys_CLI008) {
    CHECK(ClassifySearchKey('R', false, false, true, 0) == SearchKeyAction::StepOlder);
    CHECK(ClassifySearchKey(VK_RETURN, false, false, false, L'\r') == SearchKeyAction::Accept);
    CHECK(ClassifySearchKey(VK_ESCAPE, false, false, false, 27) == SearchKeyAction::Cancel);
    CHECK(ClassifySearchKey(VK_BACK, false, false, false, 8) == SearchKeyAction::Backspace);
    CHECK(ClassifySearchKey('A', false, false, false, L'a') == SearchKeyAction::Insert);
    CHECK(ClassifySearchKey('A', false, true, false, L'A') == SearchKeyAction::Insert);
    CHECK(ClassifySearchKey(VK_UP, false, false, false, 0) == SearchKeyAction::Other);
    CHECK(ClassifySearchKey(VK_F5, false, false, false, 0) == SearchKeyAction::Other);
    CHECK(ClassifySearchKey('R', true, false, false, 0) == SearchKeyAction::Other);  // Ctrl+R keeps its refresh meaning
    CHECK(ClassifySearchKey('A', true, false, false, 1) == SearchKeyAction::Other);
}
