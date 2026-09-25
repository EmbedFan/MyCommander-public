#include "TestFramework.h"

#include "TextWidth.h"

using mc::CenterToDisplayWidth;
using mc::CharDisplayWidth;
using mc::PadToDisplayWidth;
using mc::StringDisplayWidth;

TEST_CASE(CharDisplayWidth_AsciiIsOneColumn) {
    CHECK(CharDisplayWidth(L'A') == 1);
    CHECK(CharDisplayWidth(L' ') == 1);
    CHECK(CharDisplayWidth(L'0') == 1);
}

TEST_CASE(CharDisplayWidth_BoxDrawingFrameGlyphsAreOneColumn_UI014) {
    constexpr wchar_t kFrameGlyphs[] = {
        L'\x2500', L'\x2502', L'\x250C', L'\x2510', L'\x2514', L'\x2518',
        L'\x251C', L'\x2524', L'\x252C', L'\x2534', L'\x253C',
    };
    for (wchar_t glyph : kFrameGlyphs) CHECK(CharDisplayWidth(glyph) == 1);
}

TEST_CASE(CharDisplayWidth_CjkIdeographIsTwoColumns) {
    CHECK(CharDisplayWidth(L'\x4E2D') == 2);  // 中
    CHECK(CharDisplayWidth(L'\x6587') == 2);  // 文
}

TEST_CASE(CharDisplayWidth_HiraganaAndKatakanaAreTwoColumns) {
    CHECK(CharDisplayWidth(L'\x3042') == 2);  // hiragana "a"
    CHECK(CharDisplayWidth(L'\x30A2') == 2);  // katakana "a"
}

TEST_CASE(CharDisplayWidth_HalfwidthKatakanaIsOneColumn) {
    CHECK(CharDisplayWidth(L'\xFF71') == 1);  // halfwidth katakana "a"
}

TEST_CASE(CharDisplayWidth_FullwidthLatinIsTwoColumns) {
    CHECK(CharDisplayWidth(L'\xFF21') == 2);  // fullwidth "A"
}

TEST_CASE(StringDisplayWidth_MixesNarrowAndWideCorrectly) {
    // "AB" + two CJK ideographs (4 columns) + "C" = 2 + 4 + 1 = 7
    std::wstring s = L"AB\x4E2D\x6587" L"C";
    CHECK(StringDisplayWidth(s) == 7);
}

TEST_CASE(PadToDisplayWidth_PadsAsciiWithTrailingSpaces) {
    std::wstring result = PadToDisplayWidth(L"ab", 5);
    CHECK(result == L"ab   ");
    CHECK(StringDisplayWidth(result) == 5);
}

TEST_CASE(PadToDisplayWidth_TrimsPlainAsciiToExactWidth) {
    CHECK(PadToDisplayWidth(L"abcdef", 3) == L"abc");
}

TEST_CASE(PadToDisplayWidth_NeverSplitsAWideCharacterInHalf) {
    // Each ideograph is 2 columns; budget of 3 only fits one (2 columns),
    // leaving 1 column of padding rather than emitting half a glyph.
    std::wstring s = L"\x4E2D\x6587";  // 2 ideographs, 4 columns total
    std::wstring result = PadToDisplayWidth(s, 3);
    CHECK(result == L"\x4E2D ");
    CHECK(StringDisplayWidth(result) == 3);
}

TEST_CASE(PadToDisplayWidth_ExactWideWidthFitsWithNoPadding) {
    std::wstring s = L"\x4E2D\x6587";  // 4 columns
    CHECK(PadToDisplayWidth(s, 4) == s);
}

TEST_CASE(CenterToDisplayWidth_CentersWithExtraSpaceOnRight) {
    std::wstring result = CenterToDisplayWidth(L"ab", 5);
    CHECK(result == L" ab  ");
    CHECK(StringDisplayWidth(result) == 5);
}

TEST_CASE(CenterToDisplayWidth_TrimsOversizedTextToWidth) {
    std::wstring result = CenterToDisplayWidth(L"abcdef", 3);
    CHECK(StringDisplayWidth(result) == 3);
}
