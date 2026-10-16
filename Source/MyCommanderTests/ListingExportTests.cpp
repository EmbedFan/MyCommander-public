#include "TestFixtures.h"
#include "TestFramework.h"

#include "ListingExport.h"
#include "Utf8TextFile.h"

#include <windows.h>

namespace fs = std::filesystem;
using namespace mc;

TEST_CASE(ListingExport_CsvQuoting_LIST012) {
    CHECK(CsvField(L"plain.txt") == L"plain.txt");
    CHECK(CsvField(L"a,b") == L"\"a,b\"");
    CHECK(CsvField(L"say \"hi\"") == L"\"say \"\"hi\"\"\"");
    CHECK(CsvField(L"two\nlines") == L"\"two\nlines\"");
    CHECK(CsvField(L" lead") == L"\" lead\"");
    CHECK(CsvField(L"trail ") == L"\"trail \"");
    CHECK(CsvField(L"\u00c1rv\u00edz\u0171r\u0151.txt") == L"\u00c1rv\u00edz\u0171r\u0151.txt");
    CHECK(CsvField(L"") == L"");
}

TEST_CASE(ListingExport_CsvFormulaGuard_LIST012) {
    CHECK(CsvField(L"=SUM(A1)") == L"'=SUM(A1)");
    CHECK(CsvField(L"+1") == L"'+1");
    CHECK(CsvField(L"-1") == L"'-1");
    CHECK(CsvField(L"@cmd") == L"'@cmd");
    CHECK(CsvField(L"=a,b") == L"\"'=a,b\"");  // guard first, then quoting
    CHECK(CsvField(L"a=b") == L"a=b");         // only a leading character counts
}

TEST_CASE(ListingExport_TextFieldReplacesTabsAndBreaks_LIST012) {
    CHECK(TextField(L"a\tb\r\nc") == L"a b  c");
    CHECK(TextField(L"plain") == L"plain");
}

TEST_CASE(ListingExport_TypeTimeAttributes_LIST012) {
    CHECK(ListingTypeText(L"a.txt", false) == L"txt");
    CHECK(ListingTypeText(L"archive.tar.gz", false) == L"gz");
    CHECK(ListingTypeText(L".gitignore", false) == L"");
    CHECK(ListingTypeText(L"noext", false) == L"");
    CHECK(ListingTypeText(L"folder.d", true) == L"<DIR>");
    CHECK(ListingAttributeLetters(0) == L"----");
    CHECK(ListingAttributeLetters(FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_ARCHIVE) == L"R--A");
    CHECK(ListingAttributeLetters(FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM) == L"-HS-");
    CHECK(FormatListingTime(0) == L"");
    // 2020-01-02 03:04:05 UTC; the local rendering has the same shape whatever the zone.
    const std::wstring t = FormatListingTime(132224018450000000ULL);
    CHECK(t.size() == 19);
    CHECK(t[4] == L'-' && t[7] == L'-' && t[10] == L' ' && t[13] == L':' && t[16] == L':');
}

TEST_CASE(ListingExport_FormatListing_HeaderAndRows_LIST012) {
    std::vector<ListingRow> rows;
    rows.push_back(MakeListingRow(L"a,b.txt", L"C:\\d\\a,b.txt", false, 12, FILE_ATTRIBUTE_ARCHIVE, 0, 3));
    rows.push_back(MakeListingRow(L"sub", L"C:\\d\\sub", true, 999, 0, 0, 0));
    const std::wstring csv = FormatListing(rows, ListingFormat::Csv);
    CHECK(csv == L"Name,Type,Size,Modified,Attributes,Label,Path\r\n"
                 L"\"a,b.txt\",txt,12,,---A,3,\"C:\\d\\a,b.txt\"\r\n"
                 L"sub,<DIR>,,,----,,C:\\d\\sub\r\n");
    const std::wstring txt = FormatListing(rows, ListingFormat::Text);
    CHECK(txt.find(L"Name\tType\tSize\tModified\tAttributes\tLabel\tPath\r\n") == 0);
    CHECK(txt.find(L"a,b.txt\ttxt\t12\t\t---A\t3\tC:\\d\\a,b.txt\r\n") != std::wstring::npos);
}

TEST_CASE(ListingExport_DefaultFileName_LIST012) {
    CHECK(DefaultListingFileName(L"Photos", 2026, 10, 5, 9, 7, ListingFormat::Csv) == L"listing-Photos-20261005-0907.csv");
    CHECK(DefaultListingFileName(L"a:b", 2026, 1, 2, 3, 4, ListingFormat::Text) == L"listing-a_b-20260102-0304.txt");
    CHECK(DefaultListingFileName(L"", 2026, 1, 2, 3, 4, ListingFormat::Csv) == L"listing-drive-20260102-0304.csv");
}

TEST_CASE(ListingExport_OutputInsideFolderRefused_LIST012) {
    test::TempDir dir;
    const fs::path folder = dir.Path() / L"exported";
    fs::create_directories(folder / L"sub");
    CHECK(IsExportOutputInsideFolder(folder, folder / L"out.csv"));
    CHECK(IsExportOutputInsideFolder(folder, folder / L"sub" / L"out.csv"));
    CHECK(!IsExportOutputInsideFolder(folder, dir.Path() / L"out.csv"));
    CHECK(!IsExportOutputInsideFolder(folder, dir.Path() / L"exported2" / L"out.csv"));
}

TEST_CASE(ListingExport_GatherPanelRows_SelectionAndWhole_LIST012) {
    std::vector<Entry> entries(3);
    entries[0].name = L"..";
    entries[0].isDirectory = true;
    entries[1].name = L"one.txt";
    entries[1].sizeBytes = 5;
    entries[1].selected = true;
    entries[2].name = L"two.txt";
    entries[2].sizeBytes = 7;
    const fs::path folder = L"C:\\x";
    auto labelOf = [](const fs::path& p) { return p.filename() == L"one.txt" ? 2 : 0; };
    auto all = GatherPanelRows(entries, folder, false, labelOf);
    CHECK(all.size() == 2);  // ".." excluded
    CHECK(all[0].name == L"one.txt");
    CHECK(all[0].label == 2);
    CHECK(all[0].fullPath == L"C:\\x\\one.txt");
    auto sel = GatherPanelRows(entries, folder, true, labelOf);
    CHECK(sel.size() == 1);
    CHECK(sel[0].sizeBytes == 5);
}

TEST_CASE(ListingExport_GatherSubtreeRows_RelativeNames_LIST012) {
    test::TempDir dir;
    const fs::path root = dir.Path() / L"tree";
    fs::create_directories(root / L"sub" / L"deep");
    test::WriteFileContent(root / L"a.bin", std::string(10, 'a'));
    test::WriteFileContent(root / L"sub" / L"b.bin", std::string(20, 'b'));
    test::WriteFileContent(root / L"sub" / L"deep" / L"c.bin", std::string(30, 'c'));
    ListingGather g = GatherSubtreeRows(root, {});
    CHECK(!g.cancelled);
    CHECK(!g.truncated);
    CHECK(g.rows.size() == 5);  // 3 files + 2 folders
    bool foundDeep = false, foundSub = false;
    for (const auto& r : g.rows) {
        if (r.name == L"sub\\deep\\c.bin") {
            foundDeep = true;
            CHECK(r.sizeBytes == 30);
            CHECK(r.typeText == L"bin");
            CHECK(!r.modified.empty());
            CHECK(r.fullPath.rfind(L"\\\\?\\", 0) != 0);  // shown without the long-path prefix
            CHECK(r.fullPath == (root / L"sub" / L"deep" / L"c.bin").wstring());
        }
        if (r.name == L"sub") {
            foundSub = true;
            CHECK(r.typeText == L"<DIR>");
        }
    }
    if (!(foundDeep && foundSub))
        for (const auto& r : g.rows) wprintf(L"  row: [%ls] %ls\n", r.name.c_str(), r.fullPath.c_str());
    CHECK(foundDeep && foundSub);
}

TEST_CASE(ListingExport_WriteBomOption_LIST012) {
    test::TempDir dir;
    const fs::path withBom = dir.Path() / L"a.csv";
    const fs::path without = dir.Path() / L"b.txt";
    CHECK(WriteUtf8TextFileAtomic(withBom, L"\u00c1rv\u00edz", true));
    CHECK(WriteUtf8TextFileAtomic(without, L"\u00c1rv\u00edz"));
    const std::string a = test::ReadFileContent(withBom);
    const std::string b = test::ReadFileContent(without);
    CHECK(a.size() == b.size() + 3);
    CHECK(a.compare(0, 3, "\xEF\xBB\xBF") == 0);
    CHECK(b.compare(0, 3, "\xEF\xBB\xBF") != 0);
    CHECK(ReadUtf8TextFile(withBom) == L"\u00c1rv\u00edz");
}
