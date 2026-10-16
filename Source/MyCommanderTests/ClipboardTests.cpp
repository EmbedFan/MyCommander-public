#include "TestFramework.h"

#include "Clipboard.h"

using mc::ClipboardFileDetail;
using mc::ClipboardNameFormat;
using mc::FormatNamesForClipboard;

TEST_CASE(Clipboard_FormatNames_Names_OneNamePerLine_SEL008) {
    std::vector<ClipboardFileDetail> items = {{L"a.txt", L"c:\\x\\a.txt", 10, L"2026-10-13 10:00", false},
                                              {L"b.txt", L"c:\\x\\b.txt", 20, L"2026-10-13 10:01", false}};
    std::wstring out = FormatNamesForClipboard(items, ClipboardNameFormat::Names);
    CHECK(out == L"a.txt\r\nb.txt\r\n");
}

TEST_CASE(Clipboard_FormatNames_FullPaths_UsesFullPath_SEL008) {
    std::vector<ClipboardFileDetail> items = {{L"a.txt", L"c:\\x\\a.txt", 10, L"2026-10-13 10:00", false}};
    std::wstring out = FormatNamesForClipboard(items, ClipboardNameFormat::FullPaths);
    CHECK(out == L"c:\\x\\a.txt\r\n");
}

TEST_CASE(Clipboard_FormatNames_FullPathsWithDetails_TabSeparatesSizeAndDate_SEL008) {
    std::vector<ClipboardFileDetail> items = {{L"a.txt", L"c:\\x\\a.txt", 10, L"2026-10-13 10:00", false}};
    std::wstring out = FormatNamesForClipboard(items, ClipboardNameFormat::FullPathsWithDetails);
    CHECK(out == L"c:\\x\\a.txt\t10\t2026-10-13 10:00\r\n");
}

TEST_CASE(Clipboard_FormatNames_FullPathsWithDetails_DirectoryShowsDirTag_SEL008) {
    std::vector<ClipboardFileDetail> items = {{L"sub", L"c:\\x\\sub", 0, L"2026-10-13 10:00", true}};
    std::wstring out = FormatNamesForClipboard(items, ClipboardNameFormat::FullPathsWithDetails);
    CHECK(out == L"c:\\x\\sub\t<DIR>\t2026-10-13 10:00\r\n");
}

TEST_CASE(Clipboard_FormatNames_Empty_ProducesEmptyString_SEL008) {
    std::vector<ClipboardFileDetail> items;
    CHECK(FormatNamesForClipboard(items, ClipboardNameFormat::Names).empty());
}
