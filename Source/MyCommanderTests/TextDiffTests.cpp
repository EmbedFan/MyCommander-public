#include "TestFixtures.h"
#include "TestFramework.h"

#include "TextDiff.h"
#include "TextFile.h"

#include <string>
#include <vector>

using mc::ChangedSpans;
using mc::ChooseFilesToCompare;
using mc::ComparePanelState;
using mc::ComputeLineDiff;
using mc::DiffOptions;
using mc::DiffRow;
using mc::DiffStatus;
using mc::RowKind;
using mc::WhitespaceMode;

namespace {

using Lines = std::vector<std::wstring>;

// "S", "R", "A", "C" per row, for compact expectations.
std::wstring Kinds(const mc::DiffResult& r) {
    std::wstring out;
    for (const DiffRow& row : r.rows)
        out += row.kind == RowKind::Same ? L'S' : row.kind == RowKind::Removed ? L'R' : row.kind == RowKind::Added ? L'A' : L'C';
    return out;
}

// Every left and right line appears exactly once, in order.
bool CoversBothFiles(const mc::DiffResult& r, size_t leftCount, size_t rightCount) {
    size_t nextLeft = 0, nextRight = 0;
    for (const DiffRow& row : r.rows) {
        if (row.left && *row.left != nextLeft++) return false;
        if (row.right && *row.right != nextRight++) return false;
    }
    return nextLeft == leftCount && nextRight == rightCount;
}

}  // namespace

TEST_CASE(TextDiff_EmptyAndIdentical_SRC008) {
    auto r = ComputeLineDiff({}, {}, {});
    CHECK(r.status == DiffStatus::Ok && r.rows.empty() && r.Identical());
    r = ComputeLineDiff({}, Lines{L"a", L"b"}, {});
    CHECK(Kinds(r) == L"AA" && r.added == 2 && !r.Identical());
    r = ComputeLineDiff(Lines{L"a"}, {}, {});
    CHECK(Kinds(r) == L"R" && r.removed == 1);
    r = ComputeLineDiff(Lines{L"a", L"b", L"c"}, Lines{L"a", L"b", L"c"}, {});
    CHECK(Kinds(r) == L"SSS" && r.Identical());
}

TEST_CASE(TextDiff_InsertDeleteAtStartMiddleEnd_SRC008) {
    Lines base{L"1", L"2", L"3", L"4"};
    auto r = ComputeLineDiff(base, Lines{L"0", L"1", L"2", L"3", L"4"}, {});
    CHECK(Kinds(r) == L"ASSSS");
    r = ComputeLineDiff(base, Lines{L"1", L"2", L"x", L"3", L"4"}, {});
    CHECK(Kinds(r) == L"SSASS" && r.rows[2].right == std::optional<size_t>(2));
    r = ComputeLineDiff(base, Lines{L"1", L"2", L"3", L"4", L"5"}, {});
    CHECK(Kinds(r) == L"SSSSA");
    r = ComputeLineDiff(base, Lines{L"2", L"3", L"4"}, {});
    CHECK(Kinds(r) == L"RSSS");
    r = ComputeLineDiff(base, Lines{L"1", L"4"}, {});
    CHECK(Kinds(r) == L"SRRS" && r.removed == 2);
    CHECK(CoversBothFiles(r, 4, 2));
}

TEST_CASE(TextDiff_ChangedLinesArePairedSideBySide_SRC008) {
    auto r = ComputeLineDiff(Lines{L"a", L"old", L"z"}, Lines{L"a", L"new", L"z"}, {});
    CHECK(Kinds(r) == L"SCS" && r.changed == 1);
    CHECK(r.rows[1].left == std::optional<size_t>(1) && r.rows[1].right == std::optional<size_t>(1));
    // Three lines replaced by one: one Changed pair, two Removed.
    r = ComputeLineDiff(Lines{L"a", L"x1", L"x2", L"x3", L"z"}, Lines{L"a", L"y", L"z"}, {});
    CHECK(Kinds(r) == L"SCRRS" && r.changed == 1 && r.removed == 2);
    // Several separate blocks.
    r = ComputeLineDiff(Lines{L"1", L"2", L"3", L"4", L"5", L"6"}, Lines{L"1", L"two", L"3", L"4", L"5", L"6", L"7"}, {});
    CHECK(Kinds(r) == L"SCSSSSA");
    CHECK(CoversBothFiles(r, 6, 7));
}

TEST_CASE(TextDiff_IgnoreCaseAndWhitespace_SRC008) {
    Lines a{L"Hello World", L"\u00C1RV\u00CDZ", L"tab\there  "}, b{L"hello world", L"\u00E1rv\u00EDz", L"tab here"};
    CHECK(!ComputeLineDiff(a, b, {}).Identical());
    DiffOptions caseOnly;
    caseOnly.ignoreCase = true;
    auto r = ComputeLineDiff(a, b, caseOnly);
    CHECK(Kinds(r) == L"SSC");  // non-ASCII letters fold too (TextCase.h)
    DiffOptions all;
    all.ignoreCase = true;
    all.whitespace = WhitespaceMode::All;
    CHECK(ComputeLineDiff(a, b, all).Identical());
    DiffOptions trailing;
    trailing.whitespace = WhitespaceMode::Trailing;
    CHECK(ComputeLineDiff(Lines{L"x  ", L"y\t"}, Lines{L"x", L"y"}, trailing).Identical());
    CHECK(!ComputeLineDiff(Lines{L"a b"}, Lines{L"ab"}, trailing).Identical());
    CHECK(mc::NormalizeForDiff(L" A b ", all) == L"ab");
}

TEST_CASE(TextDiff_CrlfAndLfFilesCompareEqual_SRC008) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"crlf.txt", "one\r\ntwo\r\n");
    test::WriteFileContent(dir.Path() / L"lf.txt", "one\ntwo\n");
    auto read = [](const std::filesystem::path& p) {
        Lines lines;
        mc::ForEachTextLine(p, mc::TextEncodingChoice::Auto, [&](size_t, const std::wstring& l) {
            lines.push_back(l);
            return true;
        });
        return lines;
    };
    CHECK(ComputeLineDiff(read(dir.Path() / L"crlf.txt"), read(dir.Path() / L"lf.txt"), {}).Identical());
}

TEST_CASE(TextDiff_LargeFileWithOneChangeIsFast_SRC008) {
    Lines a, b;
    for (int i = 0; i < 200000; ++i) a.push_back(L"line " + std::to_wstring(i));
    b = a;
    b[123456] = L"changed";
    auto r = ComputeLineDiff(a, b, {});
    CHECK(r.status == DiffStatus::Ok && r.changed == 1 && r.rows.size() == 200000);
    CHECK(r.rows[123456].kind == RowKind::Changed);
}

TEST_CASE(TextDiff_TooDifferentAndCancel_SRC008) {
    Lines a, b;
    for (int i = 0; i < 3000; ++i) {
        a.push_back(L"a" + std::to_wstring(i));
        b.push_back(i % 2 ? L"b" + std::to_wstring(i) : L"a" + std::to_wstring(i));
    }
    auto r = ComputeLineDiff(a, b, {}, {}, {}, /*maxEdits=*/100);
    CHECK(r.status == DiffStatus::TooLarge);
    r = ComputeLineDiff(a, b, {}, [] { return true; });
    CHECK(r.status == DiffStatus::Cancelled);
    r = ComputeLineDiff(a, b, {});
    CHECK(r.status == DiffStatus::Ok && r.changed == 1500 && CoversBothFiles(r, 3000, 3000));
}

TEST_CASE(TextDiff_NextAndPreviousDifference_SRC008) {
    auto r = ComputeLineDiff(Lines{L"1", L"2", L"3", L"4", L"5", L"6"}, Lines{L"x", L"2", L"3", L"y", L"z", L"6"}, {});
    CHECK(Kinds(r) == L"CSSCCS");
    CHECK(mc::NextDifference(r.rows, 0) == std::optional<size_t>(3));  // from inside block 1 to block 2
    CHECK(mc::NextDifference(r.rows, 1) == std::optional<size_t>(3));
    CHECK(!mc::NextDifference(r.rows, 3).has_value());
    CHECK(mc::PreviousDifference(r.rows, 5) == std::optional<size_t>(3));
    CHECK(mc::PreviousDifference(r.rows, 3) == std::optional<size_t>(0));
    CHECK(!mc::PreviousDifference(r.rows, 0).has_value());
}

TEST_CASE(TextDiff_ChangedSpansInsideALine_SRC008) {
    auto spans = ChangedSpans(L"retries = 3", L"retries = 5", {});
    CHECK(spans.left.size() == 1 && spans.left[0] == std::make_pair(size_t{10}, size_t{1}));
    CHECK(spans.right.size() == 1 && spans.right[0] == std::make_pair(size_t{10}, size_t{1}));
    spans = ChangedSpans(L"abc", L"abXc", {});
    CHECK(spans.left.empty() && spans.right.size() == 1 && spans.right[0].first == 2);
    std::wstring longLine(3000, L'a');
    spans = ChangedSpans(longLine, longLine + L"b", {});
    CHECK(spans.left.size() == 1 && spans.left[0].second == 3000);  // over the cap: whole line
    DiffOptions ws;
    ws.whitespace = WhitespaceMode::All;
    spans = ChangedSpans(L"a b", L"a  b", ws);
    CHECK(spans.left.empty() && spans.right.empty());  // ignored whitespace isn't highlighted
}

TEST_CASE(TextDiff_FirstDifferenceOffset_SRC008) {
    test::TempDir dir;
    auto p = [&](const wchar_t* n) { return dir.Path() / n; };
    std::string big(200000, 'q');
    test::WriteFileContent(p(L"a"), big);
    test::WriteFileContent(p(L"b"), big);
    auto r = mc::FirstDifferenceOffset(p(L"a"), p(L"b"));
    CHECK(r.ok && r.identical && r.leftSize == 200000);
    std::string other = big;
    other[150000] = 'x';
    test::WriteFileContent(p(L"c"), other);
    r = mc::FirstDifferenceOffset(p(L"a"), p(L"c"));
    CHECK(r.ok && !r.identical && r.firstDifference == 150000);
    test::WriteFileContent(p(L"d"), big.substr(0, 1000));
    r = mc::FirstDifferenceOffset(p(L"d"), p(L"a"));
    CHECK(r.ok && !r.identical && r.firstDifference == 1000 && r.rightSize == 200000);
    test::WriteFileContent(p(L"e"), "x");
    r = mc::FirstDifferenceOffset(p(L"e"), p(L"a"));
    CHECK(r.ok && r.firstDifference == 0);
    r = mc::FirstDifferenceOffset(p(L"missing"), p(L"a"));
    CHECK(!r.ok && !r.error.empty());
    r = mc::FirstDifferenceOffset(p(L"a"), p(L"b"), [] { return true; });
    CHECK(r.cancelled);
}

TEST_CASE(TextDiff_ChooseFilesToCompare_SRC008) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.Path() / L"L");
    std::filesystem::create_directories(dir.Path() / L"R");
    ComparePanelState left{dir.Path() / L"L", {}, 0, L"cursor.txt", {L"cursor.txt", L"a.txt", L"b.txt", L"same.txt"}};
    ComparePanelState right{dir.Path() / L"R", {}, 0, L"other.txt", {L"other.txt", L"CURSOR.TXT", L"r1.txt"}};

    // 1. Two selected in the active panel (here the right one), panel order.
    auto two = right;
    two.selectedFiles = {L"other.txt", L"r1.txt"};
    auto c = ChooseFilesToCompare(left, two, /*leftActive=*/false);
    CHECK(c.pair && c.pair->first == right.folder / L"other.txt" && c.pair->second == right.folder / L"r1.txt");
    // More than two: explained.
    auto three = left;
    three.selectedFiles = {L"a.txt", L"b.txt", L"cursor.txt"};
    c = ChooseFilesToCompare(three, right, true);
    CHECK(!c.pair && c.problem.find(L"3 files") == 0);
    // 2. One selected in each panel: left | right, whichever is active.
    auto oneL = left, oneR = right;
    oneL.selectedFiles = {L"a.txt"};
    oneR.selectedFiles = {L"r1.txt"};
    c = ChooseFilesToCompare(oneL, oneR, false);
    CHECK(c.pair && c.pair->first == left.folder / L"a.txt" && c.pair->second == right.folder / L"r1.txt");
    // 3. The cursor file and the same name in the other panel (case-insensitive).
    c = ChooseFilesToCompare(left, right, true);
    CHECK(c.pair && c.pair->first == left.folder / L"cursor.txt" && c.pair->second == right.folder / L"CURSOR.TXT");
    // ...from the right panel, the left panel's file is still shown on the left.
    auto rightActive = right;
    rightActive.cursorFile = L"CURSOR.TXT";
    c = ChooseFilesToCompare(left, rightActive, false);
    CHECK(c.pair && c.pair->first == left.folder / L"cursor.txt" && c.pair->second == right.folder / L"CURSOR.TXT");
    // 4. No same name: the other panel's cursor file.
    auto noSame = left;
    noSame.cursorFile = L"a.txt";
    c = ChooseFilesToCompare(noSame, right, true);
    CHECK(c.pair && c.pair->first == left.folder / L"a.txt" && c.pair->second == right.folder / L"other.txt");
    // Nothing usable.
    auto onDir = left;
    onDir.cursorFile.clear();
    c = ChooseFilesToCompare(onDir, right, true);
    CHECK(!c.pair && !c.problem.empty());
    auto noOther = right;
    noOther.cursorFile.clear();
    noOther.fileNames.clear();
    c = ChooseFilesToCompare(noSame, noOther, true);
    CHECK(!c.pair && c.problem.find(L"a.txt") != std::wstring::npos);
    // The same file on both sides (both panels in one folder).
    test::WriteFileContent(dir.Path() / L"L" / L"cursor.txt", "x");
    auto mirror = left;
    c = ChooseFilesToCompare(left, mirror, true);
    CHECK(!c.pair && c.problem.find(L"same file") != std::wstring::npos);
}

TEST_CASE(TextDiff_BuildExternalDiffCommand_SRC008) {
    std::filesystem::path l = L"C:\\a\\left file.txt", r = L"D:\\b\\right.txt";
    auto c = mc::BuildExternalDiffCommand(L"  \"C:\\Program Files\\WinMerge\\WinMergeU.exe\" /u \"%1\" \"%2\"  ", l, r);
    CHECK(c && c->program == L"C:\\Program Files\\WinMerge\\WinMergeU.exe");
    CHECK(c && c->arguments == L"/u \"C:\\a\\left file.txt\" \"D:\\b\\right.txt\"");
    c = mc::BuildExternalDiffCommand(L"meld.exe", l, r);  // no placeholders: both paths appended, quoted
    CHECK(c && c->program == L"meld.exe" && c->arguments == L"\"C:\\a\\left file.txt\" \"D:\\b\\right.txt\"");
    c = mc::BuildExternalDiffCommand(L"tool.exe %2 %1", l, r);  // order as written, verbatim
    CHECK(c && c->arguments == L"D:\\b\\right.txt C:\\a\\left file.txt");
    CHECK(!mc::BuildExternalDiffCommand(L"   ", l, r));
    CHECK(!mc::BuildExternalDiffCommand(L"\"unterminated", l, r));
}
