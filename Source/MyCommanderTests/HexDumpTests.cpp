#include "TestFramework.h"

#include "HexDump.h"

#include <initializer_list>
#include <sstream>
#include <string>
#include <vector>

using mc::FindBytePattern;
using mc::FormatHexRow;
using mc::HexRowColumns;
using mc::HexRowStart;
using mc::LineIndexForByte;
using mc::NextHexItemSetting;
using mc::ParseHexPattern;

namespace {

std::string Bytes(std::initializer_list<unsigned> values) {
    std::string out;
    for (unsigned v : values) out.push_back(static_cast<char>(static_cast<unsigned char>(v)));
    return out;
}

std::vector<uint8_t> Pattern(std::initializer_list<unsigned> values) {
    std::vector<uint8_t> out;
    for (unsigned v : values) out.push_back(static_cast<uint8_t>(v));
    return out;
}

} // namespace

// --- Widths (VEE-005) -------------------------------------------------------

TEST_CASE(HexDump_NextHexItemSetting_CyclesThroughEightSixteenThirtyTwoAndFit) {
    CHECK(NextHexItemSetting(8) == 16);
    CHECK(NextHexItemSetting(16) == 32);
    CHECK(NextHexItemSetting(32) == mc::kHexFitSetting);
    CHECK(NextHexItemSetting(mc::kHexFitSetting) == 8);
}

TEST_CASE(HexDump_NextHexItemSetting_UnknownValueReturnsDefault) {
    CHECK(NextHexItemSetting(7) == 8);
    CHECK(NextHexItemSetting(64) == 8);
}

TEST_CASE(HexDump_ResolveHexItems_FixedCountIgnoresWidth) {
    CHECK(mc::ResolveHexItems(32, 60) == 32);
    CHECK(mc::ResolveHexItems(8, 300) == 8);
}

TEST_CASE(HexDump_ResolveHexItems_FitPicksLargestCountThatFits) {
    CHECK(mc::ResolveHexItems(mc::kHexFitSetting, 60) == 8);   // 16 needs 83 columns
    CHECK(mc::ResolveHexItems(mc::kHexFitSetting, 90) == 16);  // 32 needs 147 columns
    CHECK(mc::ResolveHexItems(mc::kHexFitSetting, 150) == 32);
}

TEST_CASE(HexDump_ResolveHexItems_FitBelowAnyFitFallsBackToEight) {
    CHECK(mc::ResolveHexItems(mc::kHexFitSetting, 50) == 8);
}

TEST_CASE(HexDump_HexRowColumns_MatchesLayoutFormula) {
    CHECK(HexRowColumns(8) == 51);
    CHECK(HexRowColumns(16) == 83);
    CHECK(HexRowColumns(32) == 147);
}

// --- FormatHexRow -------------------------------------------------------------

TEST_CASE(HexDump_FormatHexRow_FullEightByteRowExactString) {
    std::string data = "Hello Wo";
    std::wstring row = FormatHexRow(0x1230, reinterpret_cast<const uint8_t*>(data.data()), 8, 8, 0, 1000);
    CHECK(row == L"0000000000001230  48 65 6C 6C 6F 20 57 6F  Hello Wo");
    CHECK(row.size() == HexRowColumns(8));
}

TEST_CASE(HexDump_FormatHexRow_PartialRowKeepsAsciiColumnAligned) {
    std::string data = Bytes({0x41, 0x0A, 0x42});
    std::wstring row = FormatHexRow(0, reinterpret_cast<const uint8_t*>(data.data()), 3, 8, 0, 1000);
    CHECK(row.size() == HexRowColumns(8));
    // ASCII column starts after 16 offset digits, 2 spaces, 8 byte slots of 3 columns, and a separator.
    CHECK(row.substr(43) == L"A.B     ");
}

TEST_CASE(HexDump_FormatHexRow_NonPrintableBytesShownAsDot) {
    std::string data = Bytes({0x20, 0x7E, 0x00, 0x7F});
    std::wstring row = FormatHexRow(0, reinterpret_cast<const uint8_t*>(data.data()), 4, 4, 0, 1000);
    CHECK(row.size() == HexRowColumns(4));
    CHECK(row.substr(HexRowColumns(4) - 4) == L" ~..");
}

TEST_CASE(HexDump_FormatHexRow_OffsetAbove4GbFormatsAllSixteenDigits) {
    std::string data = "A";
    std::wstring row = FormatHexRow(0x123456789ABCDEF0ULL, reinterpret_cast<const uint8_t*>(data.data()), 1, 8, 0, 1000);
    CHECK(row.substr(0, 16) == L"123456789ABCDEF0");
}

TEST_CASE(HexDump_FormatHexRow_PanSliceMatchesFullRow) {
    std::string data = "Hello Wo";
    const uint8_t* bytes = reinterpret_cast<const uint8_t*>(data.data());
    std::wstring full = FormatHexRow(0x1230, bytes, 8, 8, 0, 1000);
    std::wstring slice = FormatHexRow(0x1230, bytes, 8, 8, 10, 20);
    CHECK(slice.size() == 20);
    CHECK(slice == full.substr(10, 20));
}

TEST_CASE(HexDump_FormatHexRow_PanPastEndReturnsEmpty) {
    std::string data = "Hello Wo";
    std::wstring slice = FormatHexRow(0, reinterpret_cast<const uint8_t*>(data.data()), 8, 8, 500, 20);
    CHECK(slice.empty());
}

// --- HexRowStart ----------------------------------------------------------------

TEST_CASE(HexDump_HexRowStart_RoundsDownForEveryWidth) {
    CHECK(HexRowStart(0x123A, 8) == 0x1238);
    CHECK(HexRowStart(0x123A, 16) == 0x1230);
    CHECK(HexRowStart(0x123A, 32) == 0x1220);
    CHECK(HexRowStart(0x123A, 32) == 0x1220);
}

TEST_CASE(HexDump_HexRowStart_AlreadyAlignedIsUnchanged) {
    CHECK(HexRowStart(0x1230, 16) == 0x1230);
    CHECK(HexRowStart(0, 32) == 0);
}

// --- Scroll limits -------------------------------------------------------------

TEST_CASE(HexDump_HexMaxAnchor_EmptyOrShortFileIsZero) {
    CHECK(mc::HexMaxAnchor(0, 8, 5) == 0);
    CHECK(mc::HexMaxAnchor(10, 8, 20) == 0);
}

TEST_CASE(HexDump_HexMaxAnchor_LastFullScreenIsRowAligned) {
    // 100 bytes at 8 per row is 13 rows; a 5-row screen tops out at row 8.
    CHECK(mc::HexMaxAnchor(100, 8, 5) == 64);
    CHECK(mc::HexMaxAnchor(100, 8, 5) % 8 == 0);
}

TEST_CASE(HexDump_HexMaxPan_ZeroWhenRowFitsAndRemainderOtherwise) {
    CHECK(mc::HexMaxPan(8, 80) == 0);
    CHECK(mc::HexMaxPan(8, 30) == 21);    // 51 - 30
    CHECK(mc::HexMaxPan(32, 80) == 67);   // 147 - 80
}

// --- LineIndexForByte -----------------------------------------------------------

TEST_CASE(HexDump_LineIndexForByte_MapsLineStartsAndInteriorBytes) {
    std::vector<uint64_t> starts = {0, 10, 20};
    CHECK(LineIndexForByte(starts, 0) == 0);
    CHECK(LineIndexForByte(starts, 9) == 0);
    CHECK(LineIndexForByte(starts, 10) == 1);
    CHECK(LineIndexForByte(starts, 15) == 1);
    CHECK(LineIndexForByte(starts, 20) == 2);
    CHECK(LineIndexForByte(starts, 999) == 2);
}

TEST_CASE(HexDump_LineIndexForByte_CrlfBytesBelongToTheirLine) {
    // "ab\r\n" then "cd\r\n": the \r and \n bytes sit inside line 0's range.
    std::vector<uint64_t> starts = {0, 4};
    CHECK(LineIndexForByte(starts, 2) == 0);
    CHECK(LineIndexForByte(starts, 3) == 0);
    CHECK(LineIndexForByte(starts, 4) == 1);
}

TEST_CASE(HexDump_LineIndexForByte_EmptyIndexAndBytesBeforeFirstLineReturnZero) {
    std::vector<uint64_t> empty;
    CHECK(LineIndexForByte(empty, 100) == 0);
    std::vector<uint64_t> offsetStart = {5, 10};
    CHECK(LineIndexForByte(offsetStart, 2) == 0);
}

// --- ParseHexPattern -----------------------------------------------------------

TEST_CASE(HexDump_ParseHexPattern_SpacedAndUnspacedAndCaseInsensitive) {
    auto spaced = ParseHexPattern(L"4D 5A");
    CHECK(spaced.has_value());
    if (spaced) CHECK(*spaced == Pattern({0x4D, 0x5A}));

    auto lower = ParseHexPattern(L"4d5a");
    CHECK(lower.has_value());
    if (lower) CHECK(*lower == Pattern({0x4D, 0x5A}));

    auto tabbed = ParseHexPattern(L" 0A\t");
    CHECK(tabbed.has_value());
    if (tabbed) CHECK(*tabbed == Pattern({0x0A}));
}

TEST_CASE(HexDump_ParseHexPattern_RejectsOddDigitCount) {
    CHECK(!ParseHexPattern(L"4D5").has_value());
}

TEST_CASE(HexDump_ParseHexPattern_RejectsNonHexCharacter) {
    CHECK(!ParseHexPattern(L"4G").has_value());
    CHECK(!ParseHexPattern(L"hello").has_value());
}

TEST_CASE(HexDump_ParseHexPattern_RejectsEmptyOrWhitespaceOnly) {
    CHECK(!ParseHexPattern(L"").has_value());
    CHECK(!ParseHexPattern(L"   ").has_value());
}

// --- FindBytePattern -------------------------------------------------------------

TEST_CASE(HexDump_FindBytePattern_MatchAtStart) {
    std::string data = Bytes({0x41, 0x42, 0x43, 0x44, 0x45, 0x46});
    std::istringstream in(data, std::ios::binary);
    auto hit = FindBytePattern(in, data.size(), Pattern({0x41, 0x42}), 0, false);
    CHECK(hit.has_value());
    if (hit) CHECK(*hit == 0);
}

TEST_CASE(HexDump_FindBytePattern_MatchAtEnd) {
    std::string data = Bytes({0x41, 0x42, 0x43, 0x44, 0x45, 0x46});
    std::istringstream in(data, std::ios::binary);
    auto hit = FindBytePattern(in, data.size(), Pattern({0x45, 0x46}), 0, false);
    CHECK(hit.has_value());
    if (hit) CHECK(*hit == 4);
}

TEST_CASE(HexDump_FindBytePattern_NotFoundReturnsNullopt) {
    std::string data = Bytes({0x41, 0x42, 0x43});
    std::istringstream in(data, std::ios::binary);
    CHECK(!FindBytePattern(in, data.size(), Pattern({0x58}), 0, false).has_value());
}

TEST_CASE(HexDump_FindBytePattern_WrapsFromPositionPastLastMatch) {
    std::string data = Bytes({'x', 'x', 'A', 'B', 'x', 'x', 'A', 'B'});
    std::vector<uint8_t> ab = Pattern({'A', 'B'});
    {
        std::istringstream in(data, std::ios::binary);
        auto hit = FindBytePattern(in, data.size(), ab, 3, false);
        CHECK(hit.has_value());
        if (hit) CHECK(*hit == 6);
    }
    {
        std::istringstream in(data, std::ios::binary);
        auto hit = FindBytePattern(in, data.size(), ab, 7, false);
        CHECK(hit.has_value());
        if (hit) CHECK(*hit == 2);  // no match at or after 7, so wraps to the first one
    }
}

TEST_CASE(HexDump_FindBytePattern_MatchSpanningChunkBoundary) {
    // A 64 KiB chunk ends at byte 65536; this 4-byte pattern starts at 65534,
    // so its bytes straddle the boundary.
    std::string data(200000, 'x');
    data[65534] = static_cast<char>(0xDE);
    data[65535] = static_cast<char>(0xAD);
    data[65536] = static_cast<char>(0xBE);
    data[65537] = static_cast<char>(0xEF);
    std::vector<uint8_t> pattern = Pattern({0xDE, 0xAD, 0xBE, 0xEF});
    {
        std::istringstream in(data, std::ios::binary);
        auto hit = FindBytePattern(in, data.size(), pattern, 0, false);
        CHECK(hit.has_value());
        if (hit) CHECK(*hit == 65534);
    }
    {
        std::istringstream in(data, std::ios::binary);
        auto hit = FindBytePattern(in, data.size(), pattern, 65535, false);
        CHECK(hit.has_value());
        if (hit) CHECK(*hit == 65534);  // found by the wrap pass
    }
}

TEST_CASE(HexDump_FindBytePattern_AsciiCaseFoldingOnlyAffectsLetters) {
    std::string data = "hello ABC";
    std::vector<uint8_t> lowerPattern = Pattern({'a', 'b', 'c'});
    {
        std::istringstream in(data, std::ios::binary);
        auto hit = FindBytePattern(in, data.size(), lowerPattern, 0, true);
        CHECK(hit.has_value());
        if (hit) CHECK(*hit == 6);
    }
    {
        std::istringstream in(data, std::ios::binary);
        CHECK(!FindBytePattern(in, data.size(), lowerPattern, 0, false).has_value());
    }
}

TEST_CASE(HexDump_FindBytePattern_EmptyPatternAndShortFileReturnNullopt) {
    std::string data = "A";
    std::istringstream in(data, std::ios::binary);
    CHECK(!FindBytePattern(in, data.size(), {}, 0, false).has_value());
    CHECK(!FindBytePattern(in, data.size(), Pattern({'A', 'B'}), 0, false).has_value());
}

TEST_CASE(HexDump_FindBytePattern_ZeroLengthFileReturnsNullopt) {
    std::string data;
    std::istringstream in(data, std::ios::binary);
    CHECK(!FindBytePattern(in, 0, Pattern({'A'}), 0, false).has_value());
}
