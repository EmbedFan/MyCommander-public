#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <string>

namespace mc {

// UI-011: how many terminal columns a single UTF-16 code unit occupies,
// using the OS's own Unicode character-type database (GetStringTypeW)
// rather than a hand-maintained East-Asian-width table that would drift
// from whatever Unicode version the running Windows build actually knows
// about. CJK ideographs, fullwidth forms, and hiragana/katakana take 2
// columns; everything else — including halfwidth forms, ASCII, combining
// marks, and lone surrogate halves — takes 1. Combining marks and
// supplementary-plane characters (most emoji, encoded as surrogate pairs)
// can't be composed into a single cell through WriteConsoleOutputW's
// CHAR_INFO grid (one UTF-16 code unit per cell, no grapheme clustering) —
// a limitation of this console API, not something fixable at this layer —
// so they render as their own single-width cell rather than combining
// with or double-wide next to a neighbor.
inline int CharDisplayWidth(wchar_t ch) {
    WORD type = 0;
    if (!GetStringTypeW(CT_CTYPE3, &ch, 1, &type)) return 1;
    if (type & C3_HALFWIDTH) return 1;
    if (type & (C3_FULLWIDTH | C3_IDEOGRAPH | C3_KATAKANA | C3_HIRAGANA)) return 2;
    return 1;
}

// Sum of CharDisplayWidth over every code unit in `s` — the on-screen
// column count, as opposed to std::wstring::size()'s UTF-16 code unit
// count (the two differ for any full-width character).
inline int StringDisplayWidth(const std::wstring& s) {
    int total = 0;
    for (wchar_t c : s) total += CharDisplayWidth(c);
    return total;
}

// Trims `s` to at most `width` display columns — stopping before a wide
// character would be split in half rather than truncating mid-glyph — then
// pads the remainder on the right with spaces so the result is exactly
// `width` columns wide. The one place column-aligned UI text (panel rows,
// dialog boxes, status/hint lines) should go through instead of raw
// std::wstring::size()/substr(), which count UTF-16 code units, not
// terminal columns, and so misalign as soon as a full-width character
// appears anywhere in the string.
inline std::wstring PadToDisplayWidth(const std::wstring& s, size_t width) {
    std::wstring result;
    result.reserve(width);
    size_t used = 0;
    for (wchar_t c : s) {
        int charWidth = CharDisplayWidth(c);
        if (used + static_cast<size_t>(charWidth) > width) break;
        result.push_back(c);
        used += static_cast<size_t>(charWidth);
    }
    if (used < width) result.append(width - used, L' ');
    return result;
}

// Same trim rule as PadToDisplayWidth, but centers the (possibly trimmed)
// text within `width` columns instead of left-aligning it — used for
// dialog titles.
inline std::wstring CenterToDisplayWidth(const std::wstring& s, size_t width) {
    std::wstring trimmed;
    size_t used = 0;
    for (wchar_t c : s) {
        int charWidth = CharDisplayWidth(c);
        if (used + static_cast<size_t>(charWidth) > width) break;
        trimmed.push_back(c);
        used += static_cast<size_t>(charWidth);
    }
    size_t remaining = width - used;
    size_t left = remaining / 2;
    size_t right = remaining - left;
    return std::wstring(left, L' ') + trimmed + std::wstring(right, L' ');
}

}  // namespace mc
