#include "TestFixtures.h"
#include "TestFramework.h"

#include "PathUtil.h"
#include "TextFile.h"

#include <fstream>

namespace fs = std::filesystem;

using mc::IndexTextFileForViewing;
using mc::LineCount;
using mc::ReadIndexedLine;
using mc::TextEncoding;
using mc::TextFileIndex;
using mc::ToLongPath;

namespace {

// Opens `path` for ReadIndexedLine and reads every line of `index` into a
// vector, matching the shape of the pre-indexing LoadTextFileForViewing
// tests this file used to have.
std::vector<std::wstring> ReadAllLines(const fs::path& path, const TextFileIndex& index) {
    std::ifstream in(ToLongPath(path), std::ios::binary);
    std::vector<std::wstring> lines;
    for (size_t i = 0; i < LineCount(index); ++i) {
        lines.push_back(ReadIndexedLine(in, index, i));
    }
    return lines;
}

} // namespace

TEST_CASE(IndexTextFile_PlainAscii_SplitsLinesWithoutTrailingEmpty) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "line1\nline2\nline3");

    TextFileIndex index = IndexTextFileForViewing(file);
    std::vector<std::wstring> lines = ReadAllLines(file, index);

    CHECK(index.ok);
    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0] == L"line1");
        CHECK(lines[1] == L"line2");
        CHECK(lines[2] == L"line3");
    }
}

TEST_CASE(IndexTextFile_HandlesCrLfWithoutExtraTrailingLine) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "a\r\nb\r\n");

    TextFileIndex index = IndexTextFileForViewing(file);
    std::vector<std::wstring> lines = ReadAllLines(file, index);

    CHECK(index.ok);
    CHECK(lines.size() == 2);
    if (lines.size() == 2) {
        CHECK(lines[0] == L"a");
        CHECK(lines[1] == L"b");
    }
}

TEST_CASE(IndexTextFile_Utf8Bom_StripsBomAndDecodesNonAscii) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    // UTF-8 BOM followed by "h" + U+00E9 (e-acute, 2-byte UTF-8: 0xC3 0xA9) + "llo".
    std::string bytes = "\xEF\xBB\xBF" "h\xC3\xA9llo";
    test::WriteFileContent(file, bytes);

    TextFileIndex index = IndexTextFileForViewing(file);
    std::vector<std::wstring> lines = ReadAllLines(file, index);

    std::wstring expected = L"h";
    expected.push_back(static_cast<wchar_t>(0x00E9));
    expected += L"llo";

    CHECK(index.ok);
    CHECK(index.contentStart == 3);
    CHECK(lines.size() == 1);
    if (lines.size() == 1) {
        CHECK(lines[0] == expected);
    }
}

TEST_CASE(IndexTextFile_Utf16LeBom_Decodes) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    // UTF-16LE BOM (FF FE) followed by "hi\n" as UTF-16LE code units.
    std::string bytes;
    bytes += '\xFF';
    bytes += '\xFE';
    bytes += '\x68';
    bytes += '\x00';  // 'h'
    bytes += '\x69';
    bytes += '\x00';  // 'i'
    bytes += '\x0A';
    bytes += '\x00';  // '\n'
    test::WriteFileContent(file, bytes);

    TextFileIndex index = IndexTextFileForViewing(file);
    std::vector<std::wstring> lines = ReadAllLines(file, index);

    CHECK(index.ok);
    CHECK(index.encoding == TextEncoding::Utf16Le);
    CHECK(lines.size() == 1);
    if (lines.size() == 1) {
        CHECK(lines[0] == L"hi");
    }
}

TEST_CASE(IndexTextFile_Utf16LeBom_MultipleLinesAndCrLf) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    auto appendUtf16 = [](std::string& out, const std::wstring& s) {
        for (wchar_t ch : s) {
            out += static_cast<char>(ch & 0xFF);
            out += static_cast<char>((ch >> 8) & 0xFF);
        }
    };
    std::string bytes;
    bytes += '\xFF';
    bytes += '\xFE';
    appendUtf16(bytes, L"first\r\nsecond\nthird");
    test::WriteFileContent(file, bytes);

    TextFileIndex index = IndexTextFileForViewing(file);
    std::vector<std::wstring> lines = ReadAllLines(file, index);

    CHECK(lines.size() == 3);
    if (lines.size() == 3) {
        CHECK(lines[0] == L"first");
        CHECK(lines[1] == L"second");
        CHECK(lines[2] == L"third");
    }
}

TEST_CASE(IndexTextFile_InvalidUtf8_FallsBackToByteWidening) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    // 0x80 alone is an invalid UTF-8 lead/continuation byte with no context.
    std::string bytes = "ab";
    bytes += '\x80';
    bytes += "cd";
    test::WriteFileContent(file, bytes);

    TextFileIndex index = IndexTextFileForViewing(file);
    std::vector<std::wstring> lines = ReadAllLines(file, index);

    std::wstring expected = L"ab";
    expected.push_back(static_cast<wchar_t>(0x80));
    expected += L"cd";

    CHECK(index.ok);
    CHECK(lines.size() == 1);
    if (lines.size() == 1) {
        CHECK(lines[0] == expected);
    }
}

TEST_CASE(IndexTextFile_ExpandsTabsToNextStop) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "a\tb");

    TextFileIndex index = IndexTextFileForViewing(file, /*tabWidth=*/4);
    std::vector<std::wstring> lines = ReadAllLines(file, index);

    CHECK(index.ok);
    CHECK(lines.size() == 1);
    if (lines.size() == 1) {
        CHECK(lines[0] == L"a   b"); // 'a' then 3 spaces to the next 4-column stop, then 'b'
    }
}

TEST_CASE(IndexTextFile_NonexistentFile_ReturnsError) {
    test::TempDir root;
    TextFileIndex index = IndexTextFileForViewing(root.Path() / L"missing.txt");

    CHECK(!index.ok);
    CHECK(!index.error.empty());
    CHECK(LineCount(index) == 0);
}

TEST_CASE(IndexTextFile_EmptyFile_ReturnsNoLines) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "");

    TextFileIndex index = IndexTextFileForViewing(file);

    CHECK(index.ok);
    CHECK(LineCount(index) == 0);
}

// VEE-004: large-file handling is now genuine streaming/paging, not a
// read cap — a file far bigger than the old 8 MB read cap must still index
// and yield every line, and only the specific line asked for is ever
// decoded.
TEST_CASE(IndexTextFile_LargeFile_NoCapEveryLineIsReachable_VEE004) {
    test::TempDir root;
    fs::path file = root.Path() / L"big.txt";

    std::string content;
    const int lineCount = 200000; // ~2 MB of content, comfortably exceeding a small chunk size several times over
    content.reserve(static_cast<size_t>(lineCount) * 10);
    for (int i = 0; i < lineCount; ++i) {
        content += "line" + std::to_string(i) + "\n";
    }
    test::WriteFileContent(file, content);

    TextFileIndex index = IndexTextFileForViewing(file);

    CHECK(index.ok);
    CHECK(LineCount(index) == static_cast<size_t>(lineCount));

    std::ifstream in(ToLongPath(file), std::ios::binary);
    CHECK(ReadIndexedLine(in, index, 0) == L"line0");
    CHECK(ReadIndexedLine(in, index, static_cast<size_t>(lineCount) - 1) ==
          (L"line" + std::to_wstring(lineCount - 1)));
    // Random access into the middle, out of order, exercises seeking back and forth on the same
    // open handle rather than only ever reading forward.
    CHECK(ReadIndexedLine(in, index, 12345) == L"line12345");
    CHECK(ReadIndexedLine(in, index, 42) == L"line42");
}

TEST_CASE(ReadIndexedLine_OutOfRangeReturnsEmptyString) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "only\n");

    TextFileIndex index = IndexTextFileForViewing(file);
    std::ifstream in(ToLongPath(file), std::ios::binary);

    CHECK(ReadIndexedLine(in, index, 5) == L"");
}

// --- ForEachTextLine (SRC-004/005: Find files' content reader) ---------------------

namespace {
std::vector<std::wstring> ReadLines(const fs::path& path, mc::TextEncodingChoice encoding) {
    std::vector<std::wstring> lines;
    mc::ForEachTextLine(path, encoding, [&](size_t number, const std::wstring& line) {
        if (number != lines.size() + 1) lines.push_back(L"<bad line number>");
        lines.push_back(line);
        return true;
    });
    return lines;
}
}  // namespace

TEST_CASE(ForEachTextLine_Utf8WithAndWithoutBom_CrLfAndLf_SRC004) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"plain.txt", "one\r\ntwo\nlast");
    test::WriteFileContent(dir.Path() / L"bom.txt", "\xEF\xBB\xBF\xC3\xA1rv\xC3\xADz\nok\n");
    CHECK((ReadLines(dir.Path() / L"plain.txt", mc::TextEncodingChoice::Auto) ==
           std::vector<std::wstring>{L"one", L"two", L"last"}));
    CHECK((ReadLines(dir.Path() / L"bom.txt", mc::TextEncodingChoice::Auto) ==
           std::vector<std::wstring>{L"\x00E1rv\x00EDz", L"ok"}));
}

TEST_CASE(ForEachTextLine_Utf16LeAndBe_BomAndForced_SRC005) {
    test::TempDir dir;
    // "hi\r\nyo" as UTF-16LE with BOM, and as UTF-16BE with and without BOM.
    test::WriteFileContent(dir.Path() / L"le.txt", std::string("\xFF\xFEh\0i\0\r\0\n\0y\0o\0", 14));
    test::WriteFileContent(dir.Path() / L"be.txt", std::string("\xFE\xFF\0h\0i\0\r\0\n\0y\0o", 14));
    test::WriteFileContent(dir.Path() / L"be-nobom.txt", std::string("\0h\0i\0\n\0y\0o", 10));
    std::vector<std::wstring> expected{L"hi", L"yo"};
    CHECK(ReadLines(dir.Path() / L"le.txt", mc::TextEncodingChoice::Auto) == expected);
    CHECK(ReadLines(dir.Path() / L"le.txt", mc::TextEncodingChoice::Utf16Le) == expected);
    CHECK(ReadLines(dir.Path() / L"be.txt", mc::TextEncodingChoice::Auto) == expected);
    CHECK(ReadLines(dir.Path() / L"be-nobom.txt", mc::TextEncodingChoice::Utf16Be) == expected);
}

TEST_CASE(ForEachTextLine_ExplicitUtf8VersusAnsi_SRC005) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"utf8.txt", "\xC3\xA1");  // "á" in UTF-8
    CHECK((ReadLines(dir.Path() / L"utf8.txt", mc::TextEncodingChoice::Utf8) == std::vector<std::wstring>{L"\x00E1"}));
    // Read as ANSI the same two bytes are two characters, whatever the code page.
    auto ansi = ReadLines(dir.Path() / L"utf8.txt", mc::TextEncodingChoice::Ansi);
    CHECK(ansi.size() == 1 && ansi[0].size() == 2);
}

TEST_CASE(ForEachTextLine_LinesAcrossChunkBoundary_AndEarlyStop_SRC004) {
    test::TempDir dir;
    std::string big;
    for (int i = 1; i <= 20000; ++i) big += "line " + std::to_string(i) + "\n";  // > 64 KB chunk
    test::WriteFileContent(dir.Path() / L"big.txt", big);
    size_t count = 0;
    bool allRight = true;
    mc::ForEachTextLine(dir.Path() / L"big.txt", mc::TextEncodingChoice::Auto, [&](size_t n, const std::wstring& line) {
        ++count;
        if (line != L"line " + std::to_wstring(n)) allRight = false;
        return true;
    });
    CHECK(count == 20000);
    CHECK(allRight);
    size_t stoppedAt = 0;
    mc::ForEachTextLine(dir.Path() / L"big.txt", mc::TextEncodingChoice::Auto, [&](size_t n, const std::wstring&) {
        stoppedAt = n;
        return n < 3;
    });
    CHECK(stoppedAt == 3);
}

TEST_CASE(ForEachTextLine_MissingFileReturnsFalse_ProbeSeesHead_SRC004) {
    test::TempDir dir;
    CHECK(!mc::ForEachTextLine(dir.Path() / L"nope.txt", mc::TextEncodingChoice::Auto,
                               [](size_t, const std::wstring&) { return true; }));
    test::WriteFileContent(dir.Path() / L"bin.dat", std::string("MZ\x90\0\x03", 5));
    bool probed = false;
    size_t lines = 0;
    mc::ForEachTextLine(
        dir.Path() / L"bin.dat", mc::TextEncodingChoice::Auto,
        [&](size_t, const std::wstring&) {
            ++lines;
            return true;
        },
        [&](const mc::TextFileProbe& probe) {
            probed = probe.head.size() == 5 && !probe.utf16Bom;
            return false;  // "binary: skip"
        });
    CHECK(probed);
    CHECK(lines == 0);
}
