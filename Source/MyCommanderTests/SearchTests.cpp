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

// --- Content search (SRC-004/005) -----------------------------------------------------

namespace {
mc::SearchOptions ContentOptions(const std::wstring& mask, const std::wstring& text, bool regex = false) {
    mc::SearchOptions options;
    options.nameMask = mask;
    std::wstring error;
    options.content = mc::ContentMatcher::Create(mc::ContentQuery{text, regex, false}, error);
    return options;
}
}  // namespace

TEST_CASE(SearchFiles_Content_FirstMatchingLineAndNoFolders_SRC004) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"sub" / L"todo-folder");
    test::WriteFileContent(root.Path() / L"a.txt", "first\n  // TODO one\nTODO two\n");
    test::WriteFileContent(root.Path() / L"sub" / L"b.txt", "nothing here\n");
    test::WriteFileContent(root.Path() / L"sub" / L"c.txt", "x\ny\nfix: todo later\n");
    SearchOutcome out = SearchFiles(root.Path(), ContentOptions(L"*", L"todo"));
    CHECK(out.results.size() == 2);
    for (const auto& r : out.results) {
        CHECK(!r.isDirectory);  // "todo-folder" matches the name but isn't a file
        if (r.fullPath.filename() == L"a.txt") CHECK(r.lineNumber == 2 && r.lineText == L"  // TODO one");
        if (r.fullPath.filename() == L"c.txt") CHECK(r.lineNumber == 3);
    }
    CHECK(out.filesSearched == 3);
}

TEST_CASE(SearchFiles_Content_MaskAndRegexCombine_SRC005) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"keep.txt", "version 2026-10\n");
    test::WriteFileContent(root.Path() / L"skip.log", "version 2026-10\n");
    SearchOutcome out = SearchFiles(root.Path(), ContentOptions(L"*.txt", L"\\d{4}-\\d{2}", true));
    CHECK(out.results.size() == 1 && ContainsName(out.results, L"keep.txt"));
}

TEST_CASE(SearchFiles_Content_BinarySkippedByDefault_SearchedWhenAsked_SRC004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"tool.exe", std::string("MZ\0\0 secret marker \0", 21));
    SearchOutcome skipped = SearchFiles(root.Path(), ContentOptions(L"*", L"marker"));
    CHECK(skipped.results.empty());
    CHECK(skipped.binarySkipped == 1);
    mc::SearchOptions all = ContentOptions(L"*", L"marker");
    all.skipBinary = false;
    all.encoding = mc::TextEncodingChoice::Ansi;
    SearchOutcome found = SearchFiles(root.Path(), all);
    CHECK(found.results.size() == 1);
}

TEST_CASE(SearchFiles_Content_Utf16FileIsNotBinary_SRC005) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"wide.txt", std::string("\xFF\xFEh\0i\0", 6));
    SearchOutcome out = SearchFiles(root.Path(), ContentOptions(L"*", L"hi"));
    CHECK(out.results.size() == 1);
}

TEST_CASE(SearchFiles_Content_LockedFileCountedAsUnreadable_SRC004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"locked.txt", "needle\n");
    test::WriteFileContent(root.Path() / L"open.txt", "needle\n");
    HANDLE lock = CreateFileW((root.Path() / L"locked.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(lock != INVALID_HANDLE_VALUE);
    SearchOutcome out = SearchFiles(root.Path(), ContentOptions(L"*", L"needle"));
    CloseHandle(lock);
    CHECK(out.results.size() == 1 && ContainsName(out.results, L"open.txt"));
    CHECK(out.unreadable == 1);
}

TEST_CASE(SearchFiles_Content_CancelStopsInsideTheWalk_SRC004) {
    test::TempDir root;
    for (int i = 0; i < 5; ++i)
        test::WriteFileContent(root.Path() / (L"f" + std::to_wstring(i) + L".txt"), "needle\n");
    int polls = 0;
    SearchOutcome out = SearchFiles(root.Path(), ContentOptions(L"*", L"needle", true), [&]() { return ++polls > 2; });
    CHECK(out.cancelled);
    CHECK(out.results.size() < 5);
}

TEST_CASE(SearchFiles_Content_ProgressReportsEachFile_SRC004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"a.txt", "x\n");
    test::WriteFileContent(root.Path() / L"b.txt", "y\n");
    std::vector<std::wstring> seen;
    SearchFiles(root.Path(), ContentOptions(L"*", L"z"), {},
                [&](size_t, const fs::path& current) { seen.push_back(current.filename().wstring()); });
    CHECK(seen.size() == 2);
}
