#include "TestFixtures.h"
#include "TestFramework.h"

#include "RecentLocations.h"

namespace fs = std::filesystem;

using mc::ParseRecentFile;
using mc::RecentLocations;

namespace {

std::vector<std::wstring> Names(const RecentLocations& list) {
    std::vector<std::wstring> out;
    for (const auto& p : list.Entries()) out.push_back(p.wstring());
    return out;
}

}  // namespace

TEST_CASE(RecentLocations_NewestFirstAndRevisitMovesToFront_NAV017) {
    RecentLocations list;
    list.Note(L"C:\\a");
    list.Note(L"C:\\b");
    list.Note(L"C:\\c");
    CHECK((Names(list) == std::vector<std::wstring>{L"C:\\c", L"C:\\b", L"C:\\a"}));

    list.Note(L"C:\\a");  // a revisit: moves to the front, no duplicate
    CHECK((Names(list) == std::vector<std::wstring>{L"C:\\a", L"C:\\c", L"C:\\b"}));
}

TEST_CASE(RecentLocations_ComparisonIgnoresCaseSeparatorsAndTrailingBackslash_NAV017) {
    RecentLocations list;
    list.Note(L"C:\\Projects\\MyCommander");
    list.Note(L"c:/projects/mycommander/");
    CHECK(list.Entries().size() == 1);
    CHECK(list.Entries()[0].wstring() == L"c:/projects/mycommander/");  // the newest spelling wins

    list.Note(L"C:\\");
    list.Note(L"c:\\");  // a drive root keeps its one separator and still matches itself
    CHECK(list.Entries().size() == 2);
}

TEST_CASE(RecentLocations_CapDropsTheOldestAndEmptyIsIgnored_NAV017) {
    RecentLocations list(3);
    for (const wchar_t* p : {L"C:\\1", L"C:\\2", L"C:\\3", L"C:\\4"}) list.Note(p);
    CHECK((Names(list) == std::vector<std::wstring>{L"C:\\4", L"C:\\3", L"C:\\2"}));
    list.Note(fs::path());
    CHECK(list.Entries().size() == 3);
}

TEST_CASE(RecentLocations_RemoveAndClear_NAV017) {
    RecentLocations list;
    list.Note(L"C:\\a");
    list.Note(L"C:\\b");
    CHECK(list.Remove(L"c:\\A\\"));
    CHECK(!list.Remove(L"C:\\a"));
    CHECK((Names(list) == std::vector<std::wstring>{L"C:\\b"}));
    list.Clear();
    CHECK(list.Entries().empty());
}

TEST_CASE(RecentLocations_ParseAndFormatRoundTrip_NAV017) {
    RecentLocations list;
    list.Note(L"C:\\a");
    list.Note(L"D:\\\u00e1rv\u00edzt\u0171r\u0151 \u65e5\u672c");
    list.Note(L"\\\\server\\share\\team");
    std::vector<fs::path> back = ParseRecentFile(mc::FormatRecentFile(list.Entries()));
    CHECK(back == list.Entries());
}

TEST_CASE(RecentLocations_ParseIgnoresCommentsBlankLinesDuplicatesAndCrLf_NAV017) {
    std::wstring text = L"# comment\r\n\r\nC:\\a\r\n  C:\\b  \r\nc:\\A\r\n#C:\\hidden\r\nC:\\c";
    std::vector<fs::path> parsed = ParseRecentFile(text);
    CHECK(parsed.size() == 3);
    CHECK(parsed[0].wstring() == L"C:\\a");  // the first (newest) spelling of a duplicate stays
    CHECK(parsed[1].wstring() == L"C:\\b");
    CHECK(parsed[2].wstring() == L"C:\\c");

    CHECK(ParseRecentFile(L"C:\\1\nC:\\2\nC:\\3\nC:\\4\n", 2).size() == 2);  // trimmed to the cap
    CHECK(ParseRecentFile(L"").empty());
}

TEST_CASE(RecentLocations_FileRoundTripAndMissingFile_NAV017) {
    test::TempDir dir;
    fs::path file = dir.Path() / L"nested" / L"recent-folders.txt";
    CHECK(mc::LoadRecentFoldersFrom(file).empty());  // missing: the expected first run

    std::vector<fs::path> saved = {L"C:\\newest", L"D:\\older"};
    CHECK(mc::SaveRecentFoldersTo(file, saved));     // creates the folder, atomic write
    CHECK(mc::LoadRecentFoldersFrom(file) == saved);
    CHECK(!mc::SaveRecentFoldersTo(fs::path(), saved));
    CHECK(mc::LoadRecentFoldersFrom(fs::path()).empty());
}
