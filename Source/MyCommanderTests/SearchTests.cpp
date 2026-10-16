#include "TestFixtures.h"
#include "TestFramework.h"

#include "PathUtil.h"
#include "Search.h"

#include <algorithm>

namespace fs = std::filesystem;

using mc::SearchFiles;
using mc::SearchOutcome;
using mc::SearchResult;
using mc::SearchOptions;
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

// --- SRC-010: largest files -------------------------------------------------------

namespace {

SearchResult Sized(const wchar_t* path, uint64_t size) {
    SearchResult r{fs::path(path), false, size};
    return r;
}

std::vector<std::wstring> Names(const std::vector<SearchResult>& results) {
    std::vector<std::wstring> out;
    for (const auto& r : results) out.push_back(r.fullPath.filename().wstring());
    return out;
}

}  // namespace

TEST_CASE(OfferTopN_KeepsTheLargestAndDropsTheRest_SRC010) {
    std::vector<SearchResult> heap;
    for (uint64_t size : {5, 90, 10, 70, 30, 100, 1}) mc::OfferTopN(heap, Sized(L"C:\\f", size), 3);
    CHECK(heap.size() == 3);
    std::vector<uint64_t> kept;
    for (const auto& r : heap) kept.push_back(r.sizeBytes);
    std::sort(kept.begin(), kept.end());
    CHECK((kept == std::vector<uint64_t>{70, 90, 100}));
}

TEST_CASE(OfferTopN_TiesAtTheCutOffKeepTheLexicallySmallerPath_SRC010) {
    std::vector<SearchResult> heap;
    mc::OfferTopN(heap, Sized(L"C:\\b.txt", 50), 1);
    mc::OfferTopN(heap, Sized(L"C:\\a.txt", 50), 1);  // same size, smaller path: replaces
    mc::OfferTopN(heap, Sized(L"C:\\c.txt", 50), 1);  // same size, larger path: rejected
    CHECK(heap.size() == 1);
    CHECK(heap[0].fullPath == fs::path(L"C:\\a.txt"));
}

TEST_CASE(OfferTopN_CountLargerThanInputAndCountZero_SRC010) {
    std::vector<SearchResult> heap;
    mc::OfferTopN(heap, Sized(L"C:\\x", 1), 10);
    mc::OfferTopN(heap, Sized(L"C:\\y", 2), 10);
    CHECK(heap.size() == 2);

    std::vector<SearchResult> none;
    mc::OfferTopN(none, Sized(L"C:\\z", 3), 0);
    CHECK(none.empty());
}

TEST_CASE(SortLargestFirst_OrdersBySizeThenPath_SRC010) {
    std::vector<SearchResult> results = {Sized(L"C:\\b", 10), Sized(L"C:\\a", 10), Sized(L"C:\\c", 99)};
    mc::SortLargestFirst(results);
    CHECK((Names(results) == std::vector<std::wstring>{L"c", L"a", L"b"}));
}

TEST_CASE(ParseLargestCount_AcceptsOneToTenThousandOnly_SRC010) {
    size_t n = 0;
    CHECK(mc::ParseLargestCount(L"1", n) && n == 1);
    CHECK(mc::ParseLargestCount(L"10000", n) && n == 10000);
    CHECK(!mc::ParseLargestCount(L"0", n));
    CHECK(!mc::ParseLargestCount(L"10001", n));
    CHECK(!mc::ParseLargestCount(L"", n));
    CHECK(!mc::ParseLargestCount(L"12x", n));
    CHECK(!mc::ParseLargestCount(L"-5", n));
}

TEST_CASE(ParseMinimumSize_BlankIsZeroAndDigitsOnly_SRC010) {
    uint64_t bytes = 99;
    CHECK(mc::ParseMinimumSize(L"", bytes) && bytes == 0);
    CHECK(mc::ParseMinimumSize(L"4096", bytes) && bytes == 4096);
    CHECK(!mc::ParseMinimumSize(L"4k", bytes));
    CHECK(!mc::ParseMinimumSize(L"1234567890123456789012", bytes));
}

TEST_CASE(SearchFiles_Largest_RanksRealFilesAndStaysBounded_SRC010) {
    test::TempDir root;
    for (int i = 1; i <= 200; ++i) {
        wchar_t name[32];
        swprintf_s(name, L"f%03d.bin", i);
        test::WriteFileContent(root.Path() / name, std::string(static_cast<size_t>(i), 'x'));
    }
    SearchOptions options;
    options.nameMask = L"*";
    options.largestCount = 10;
    SearchOutcome out = SearchFiles(root.Path(), options);
    CHECK(out.results.size() == 10);                      // bounded by N, not by the 200 files
    CHECK(!out.truncated);                                // ranking mode has no cap to hit
    CHECK(out.results.front().sizeBytes == 200);
    CHECK(out.results.back().sizeBytes == 191);
    CHECK(out.filesSearched == 200);
}

TEST_CASE(SearchFiles_Largest_MinimumSizeAndSubfoldersAndHidden_SRC010) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"sub");
    test::WriteFileContent(root.Path() / L"small.txt", std::string(10, 'x'));
    test::WriteFileContent(root.Path() / L"big.txt", std::string(5000, 'x'));
    test::WriteFileContent(root.Path() / L"sub" / L"nested.txt", std::string(7000, 'x'));
    fs::path hidden = root.Path() / L"hidden.txt";
    test::WriteFileContent(hidden, std::string(9000, 'x'));
    SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN);

    SearchOptions base;
    base.nameMask = L"*";
    base.largestCount = 100;

    SearchOptions minimum = base;
    minimum.minSize = 1000;
    minimum.includeHidden = true;
    SearchOutcome withHidden = SearchFiles(root.Path(), minimum);
    CHECK((Names(withHidden.results) == std::vector<std::wstring>{L"hidden.txt", L"nested.txt", L"big.txt"}));

    SearchOptions noHidden = minimum;
    noHidden.includeHidden = false;
    SearchOutcome visible = SearchFiles(root.Path(), noHidden);
    CHECK((Names(visible.results) == std::vector<std::wstring>{L"nested.txt", L"big.txt"}));

    SearchOptions flat = minimum;
    flat.recursive = false;
    SearchOutcome top = SearchFiles(root.Path(), flat);
    CHECK((Names(top.results) == std::vector<std::wstring>{L"hidden.txt", L"big.txt"}));
}

TEST_CASE(SearchFiles_Largest_JunctionIsNotEnteredOrRanked_SRC010) {
    test::TempDir root;
    test::TempDir elsewhere;
    test::WriteFileContent(elsewhere.Path() / L"behind.bin", std::string(50000, 'x'));
    test::WriteFileContent(root.Path() / L"own.bin", std::string(100, 'x'));
    if (!test::CreateJunction(root.Path() / L"link", elsewhere.Path())) {
        std::printf("note: junction creation unavailable here; skipping the junction check\n");
        return;
    }
    SearchOptions options;
    options.nameMask = L"*";
    options.largestCount = 10;
    SearchOutcome out = SearchFiles(root.Path(), options);
    CHECK((Names(out.results) == std::vector<std::wstring>{L"own.bin"}));  // behind.bin is not reached
}

TEST_CASE(SearchFiles_Largest_CancelReturnsThePartialTopN_SRC010) {
    test::TempDir root;
    for (int i = 1; i <= 50; ++i) {
        wchar_t name[32];
        swprintf_s(name, L"c%02d.bin", i);
        test::WriteFileContent(root.Path() / name, std::string(static_cast<size_t>(i), 'x'));
    }
    SearchOptions options;
    options.nameMask = L"*";
    options.largestCount = 5;
    int polls = 0;
    SearchOutcome out = SearchFiles(root.Path(), options, [&] { return ++polls > 10; });
    CHECK(out.cancelled);
    CHECK(out.results.size() <= 5);
}
