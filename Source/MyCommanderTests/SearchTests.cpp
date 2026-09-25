#include "TestFixtures.h"
#include "TestFramework.h"

#include "PathUtil.h"
#include "Search.h"

#include <algorithm>

namespace fs = std::filesystem;

using mc::SearchFiles;
using mc::SearchOutcome;
using mc::SearchResult;
using mc::WildcardMatch;

namespace {

bool ContainsName(const std::vector<SearchResult>& results, const std::wstring& name) {
    return std::any_of(results.begin(), results.end(),
                       [&](const SearchResult& r) { return r.fullPath.filename() == name; });
}

}  // namespace

TEST_CASE(WildcardMatch_NoWildcardIsSubstringCaseInsensitive) {
    CHECK(WildcardMatch(L"app.LOG.txt", L"log"));
    CHECK(WildcardMatch(L"README.md", L"readme"));
    CHECK(!WildcardMatch(L"README.md", L"license"));
}

TEST_CASE(WildcardMatch_StarMatchesAnyRunOfCharacters) {
    CHECK(WildcardMatch(L"report.txt", L"*.txt"));
    CHECK(WildcardMatch(L"report.txt", L"report.*"));
    CHECK(WildcardMatch(L"a.b.c", L"a*c"));
    CHECK(!WildcardMatch(L"report.csv", L"*.txt"));
}

TEST_CASE(WildcardMatch_QuestionMatchesExactlyOneCharacter) {
    CHECK(WildcardMatch(L"a.txt", L"?.txt"));
    CHECK(!WildcardMatch(L"ab.txt", L"?.txt"));
    CHECK(WildcardMatch(L"ab.txt", L"??.txt"));
}

TEST_CASE(WildcardMatch_EmptyPatternMatchesEverything) {
    CHECK(WildcardMatch(L"anything", L""));
    CHECK(WildcardMatch(L"", L""));
}

TEST_CASE(SearchFiles_FindsMatchingFilesRecursively) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"sub" / L"deeper");
    test::WriteFileContent(root.Path() / L"top.log", "x");
    test::WriteFileContent(root.Path() / L"sub" / L"middle.log", "x");
    test::WriteFileContent(root.Path() / L"sub" / L"deeper" / L"bottom.log", "x");
    test::WriteFileContent(root.Path() / L"sub" / L"unrelated.txt", "x");

    SearchOutcome outcome = SearchFiles(root.Path(), L"*.log");

    CHECK(!outcome.cancelled);
    CHECK(!outcome.truncated);
    CHECK(outcome.results.size() == 3);
    CHECK(ContainsName(outcome.results, L"top.log"));
    CHECK(ContainsName(outcome.results, L"middle.log"));
    CHECK(ContainsName(outcome.results, L"bottom.log"));
    CHECK(!ContainsName(outcome.results, L"unrelated.txt"));
}

TEST_CASE(SearchFiles_MatchesDirectoriesToo) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"cache_data");
    test::WriteFileContent(root.Path() / L"cache_data" / L"inside.txt", "x");

    SearchOutcome outcome = SearchFiles(root.Path(), L"cache*");

    CHECK(outcome.results.size() == 1);
    if (outcome.results.size() == 1) {
        CHECK(outcome.results[0].isDirectory);
        CHECK(outcome.results[0].fullPath.filename() == L"cache_data");
    }
}

TEST_CASE(SearchFiles_EmptyPatternReturnsNoResults) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"a.txt", "x");

    SearchOutcome outcome = SearchFiles(root.Path(), L"");

    CHECK(outcome.results.empty());
    CHECK(!outcome.cancelled);
}

TEST_CASE(SearchFiles_RespectsMaxResultsCapAndSetsTruncated) {
    test::TempDir root;
    for (int i = 0; i < 10; ++i) {
        test::WriteFileContent(root.Path() / (L"match_" + std::to_wstring(i) + L".txt"), "x");
    }

    SearchOutcome outcome = SearchFiles(root.Path(), L"match_", /*cancelPoll=*/{}, /*maxResults=*/3);

    CHECK(outcome.truncated);
    CHECK(outcome.results.size() == 3);
}

TEST_CASE(SearchFiles_CancelPollStopsWalkEarly) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"sub1");
    fs::create_directories(root.Path() / L"sub2");
    test::WriteFileContent(root.Path() / L"sub1" / L"a.txt", "x");
    test::WriteFileContent(root.Path() / L"sub2" / L"b.txt", "x");

    int callCount = 0;
    auto cancelAfterFirstDirectory = [&]() { return ++callCount > 1; };

    SearchOutcome outcome = SearchFiles(root.Path(), L"*.txt", cancelAfterFirstDirectory);

    CHECK(outcome.cancelled);
}

TEST_CASE(SearchFiles_DoesNotDescendIntoReparsePointButReportsItIfMatching) {
    // The junction's real target lives outside the search root entirely, so
    // the only way SearchFiles could see "secret.txt" is by following the
    // junction — which it must not do (SEC-006).
    test::TempDir root;
    test::TempDir target;
    fs::path linkDir = root.Path() / L"searchlink";
    test::WriteFileContent(target.Path() / L"secret.txt", "x");

    if (!test::CreateJunction(linkDir, target.Path())) {
        wprintf(L"  (skipped: could not create an NTFS junction on this system)\n");
        return;
    }

    SearchOutcome outcome = SearchFiles(root.Path(), L"*");

    CHECK(ContainsName(outcome.results, L"searchlink"));
    CHECK(!ContainsName(outcome.results, L"secret.txt"));
}
