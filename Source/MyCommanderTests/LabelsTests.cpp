#include "TestFixtures.h"
#include "TestFramework.h"

#include "Labels.h"

namespace fs = std::filesystem;

using mc::kLabelCount;
using mc::LabelEntry;
using mc::LabelStore;
using mc::ParseLabelsFile;

TEST_CASE(Labels_SetGetClear_LIST009) {
    LabelStore store;
    CHECK(store.Get(L"C:\\work\\a.txt") == 0);
    CHECK(store.Set(L"C:\\work\\a.txt", 3));
    CHECK(store.Get(L"C:\\work\\a.txt") == 3);
    CHECK(store.Set(L"C:\\work\\a.txt", 5));  // change
    CHECK(store.Get(L"C:\\work\\a.txt") == 5);
    CHECK(store.Size() == 1);
    CHECK(store.Set(L"C:\\work\\a.txt", 0));  // clear
    CHECK(store.Get(L"C:\\work\\a.txt") == 0 && store.Empty());

    CHECK(!store.Set(L"C:\\work\\b.txt", kLabelCount + 1));  // out of range
    CHECK(!store.Set(L"C:\\work\\b.txt", -1));
    CHECK(!store.Set(fs::path(), 1));
    CHECK(store.Empty());
}

TEST_CASE(Labels_LookupIgnoresCaseSeparatorsAndTrailingBackslash_LIST009) {
    LabelStore store;
    store.Set(L"C:\\Work\\Reports", 2);
    CHECK(store.Get(L"c:/work/reports") == 2);
    CHECK(store.Get(L"C:\\WORK\\REPORTS\\") == 2);
    CHECK(store.Get(L"C:\\work\\reports2") == 0);  // a longer name is not the same entry
}

TEST_CASE(Labels_CapRefusesANewPathButAllowsChangesAndClears_LIST009) {
    LabelStore store(2);
    CHECK(store.Set(L"C:\\1", 1));
    CHECK(store.Set(L"C:\\2", 1));
    CHECK(!store.Set(L"C:\\3", 1));   // would exceed the cap
    CHECK(store.Set(L"C:\\2", 4));    // changing an existing one is fine
    CHECK(store.Set(L"C:\\1", 0));    // so is clearing
    CHECK(store.Set(L"C:\\3", 1));    // now there is room
}

TEST_CASE(Labels_MoveCarriesFileAndEverythingBeneathAFolder_LIST009) {
    LabelStore store;
    store.Set(L"C:\\src\\folder", 1);
    store.Set(L"C:\\src\\folder\\inner\\deep.txt", 2);
    store.Set(L"C:\\src\\folderX\\other.txt", 3);  // a sibling with a longer name: must not move
    store.Set(L"C:\\src\\keep.txt", 4);

    CHECK(store.Move(L"C:\\src\\folder", L"D:\\dst\\renamed") == 2);
    CHECK(store.Get(L"D:\\dst\\renamed") == 1);
    CHECK(store.Get(L"D:\\dst\\renamed\\inner\\deep.txt") == 2);
    CHECK(store.Get(L"C:\\src\\folder") == 0);
    CHECK(store.Get(L"C:\\src\\folder\\inner\\deep.txt") == 0);
    CHECK(store.Get(L"C:\\src\\folderX\\other.txt") == 3);
    CHECK(store.Get(L"C:\\src\\keep.txt") == 4);

    CHECK(store.Move(L"C:\\src\\keep.txt", L"C:\\src\\KEEP-renamed.txt") == 1);
    CHECK(store.Get(L"C:\\src\\keep-renamed.txt") == 4);
    CHECK(store.Move(L"C:\\nothing\\here", L"C:\\anywhere") == 0);  // nothing labelled: a no-op
}

TEST_CASE(Labels_MoveReplacesAnExistingLabelAtTheDestination_LIST009) {
    LabelStore store;
    store.Set(L"C:\\a.txt", 1);
    store.Set(L"C:\\b.txt", 5);
    CHECK(store.Move(L"C:\\a.txt", L"C:\\b.txt") == 1);
    CHECK(store.Get(L"C:\\b.txt") == 1);
    CHECK(store.Size() == 1);
}

TEST_CASE(Labels_RemoveUnderDropsTheSubtreeOnly_LIST009) {
    LabelStore store;
    store.Set(L"C:\\a\\x.txt", 1);
    store.Set(L"C:\\a", 2);
    store.Set(L"C:\\a\\b\\y.txt", 3);
    store.Set(L"C:\\ab\\z.txt", 4);
    CHECK(store.HasAnyUnder(L"C:\\a"));
    CHECK(store.RemoveUnder(L"C:\\a") == 3);
    CHECK(!store.HasAnyUnder(L"C:\\a"));
    CHECK(store.Get(L"C:\\ab\\z.txt") == 4);
    CHECK(!store.HasAnyUnder(L"C:\\a\\nothing"));
}

TEST_CASE(Labels_RemoveIfUsesTheCallersPredicate_LIST009) {
    LabelStore store;
    store.Set(L"C:\\keep.txt", 1);
    store.Set(L"C:\\gone.txt", 2);
    size_t removed = store.RemoveIf([](const fs::path& p) { return p.filename() == L"gone.txt"; });
    CHECK(removed == 1);
    CHECK(store.Get(L"C:\\keep.txt") == 1 && store.Get(L"C:\\gone.txt") == 0);
}

TEST_CASE(Labels_ParseFormatRoundTripWithUnicodeAndEquals_LIST009) {
    std::vector<LabelEntry> entries = {
        {L"C:\\w\\report.docx", 1},
        {L"D:\\m\u00e9dia\\\u65e5\u672c", 4},
        {L"\\\\server\\share\\a=b.txt", 6},  // '=' in a path: only the first '=' splits
    };
    auto parsed = ParseLabelsFile(mc::FormatLabelsFile(entries));
    CHECK(parsed.warnings.empty());
    CHECK(parsed.entries.size() == 3);
    if (parsed.entries.size() == 3) {
        for (size_t i = 0; i < 3; ++i) {
            CHECK(parsed.entries[i].path == entries[i].path);
            CHECK(parsed.entries[i].label == entries[i].label);
        }
    }
}

TEST_CASE(Labels_ParseSkipsBadLinesWithAWarning_LastDuplicateWins_LIST009) {
    std::wstring text =
        L"# a comment\r\n"
        L"\r\n"
        L"1 = C:\\ok.txt\r\n"
        L"9 = C:\\out-of-range.txt\r\n"
        L"x = C:\\not-a-number.txt\r\n"
        L"3 =\r\n"
        L"no equals sign\r\n"
        L"2 = C:\\dup.txt\r\n"
        L"5 = c:\\DUP.txt\r\n";
    auto parsed = ParseLabelsFile(text);
    CHECK(parsed.warnings.size() == 4);  // out of range, not a number, empty path, no '='

    LabelStore store;
    store.Replace(parsed.entries);
    CHECK(store.Get(L"C:\\ok.txt") == 1);
    CHECK(store.Get(L"C:\\dup.txt") == 5);  // the last one wins
    CHECK(store.Size() == 2);
}

TEST_CASE(Labels_FileRoundTripAndMissingFile_LIST009) {
    test::TempDir dir;
    fs::path file = dir.Path() / L"sub" / L"labels.conf";
    CHECK(mc::LoadLabelsFrom(file).entries.empty());  // missing: the expected first run
    CHECK(mc::LoadLabelsFrom(file).warnings.empty());

    std::vector<LabelEntry> entries = {{L"C:\\a.txt", 2}, {L"D:\\b", 6}};
    CHECK(mc::SaveLabelsTo(file, entries));
    auto loaded = mc::LoadLabelsFrom(file);
    CHECK(loaded.entries.size() == 2);
    CHECK(!mc::SaveLabelsTo(fs::path(), entries));
}
