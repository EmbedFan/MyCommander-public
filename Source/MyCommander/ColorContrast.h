#pragma once

// ACC-002: pure, header-only WCAG 2.x contrast-ratio math for a Win32
// console text attribute WORD, kept independent of Console/Theme so it's
// unit-testable (matching TextWidth.h's precedent for other header-only
// display-math helpers). See ColorContrastTests.cpp for both the math's own
// correctness tests and the actual audit of every constant in Theme.h.

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

namespace mc {

struct Rgb8 {
    uint8_t r = 0;
    uint8_t g = 0;
    uint8_t b = 0;
};

// The 16-color palette conhost.exe (and cmd.exe/PowerShell hosted inside
// it) ships as its default — the "default theme" ACC-002 asks to be
// readable. A user's own console host settings can remap these, but this
// is the reference this app's own color choices are authored and audited
// against. Indexed by the low 4 bits of a FOREGROUND_* or BACKGROUND_*
// nibble (bit0=blue, bit1=green, bit2=red, bit3=intensity) — note index 7
// ("light gray") and index 8 ("dark gray") are their own fixed colors, not
// a simple doubling of the darker/brighter pattern the other 14 follow.
inline Rgb8 ConsoleFourBitToRgb(unsigned index) {
    static constexpr Rgb8 kPalette[16] = {
        {0, 0, 0},       {0, 0, 128},     {0, 128, 0},     {0, 128, 128},
        {128, 0, 0},     {128, 0, 128},   {128, 128, 0},   {192, 192, 192},
        {128, 128, 128}, {0, 0, 255},     {0, 255, 0},     {0, 255, 255},
        {255, 0, 0},     {255, 0, 255},   {255, 255, 0},   {255, 255, 255},
    };
    return kPalette[index & 0x0F];
}

inline unsigned ConsoleForegroundIndex(WORD attr) { return attr & 0x0F; }
inline unsigned ConsoleBackgroundIndex(WORD attr) { return (attr >> 4) & 0x0F; }

inline double SrgbChannelToLinear(uint8_t channel) {
    double v = channel / 255.0;
    return (v <= 0.03928) ? (v / 12.92) : std::pow((v + 0.055) / 1.055, 2.4);
}

// WCAG 2.x relative luminance (0.0 = black, 1.0 = white).
inline double RelativeLuminance(Rgb8 color) {
    return 0.2126 * SrgbChannelToLinear(color.r) + 0.7152 * SrgbChannelToLinear(color.g) +
          0.0722 * SrgbChannelToLinear(color.b);
}

// WCAG 2.x contrast ratio between two colors — always in [1.0, 21.0],
// order-independent (the lighter color is always the numerator).
inline double ContrastRatio(Rgb8 a, Rgb8 b) {
    double la = RelativeLuminance(a);
    double lb = RelativeLuminance(b);
    double lighter = std::max(la, lb);
    double darker = std::min(la, lb);
    return (lighter + 0.05) / (darker + 0.05);
}

// The contrast ratio between a Win32 console text attribute's foreground
// and background color, under the default palette above.
inline double ConsoleAttrContrastRatio(WORD attr) {
    return ContrastRatio(ConsoleFourBitToRgb(ConsoleForegroundIndex(attr)),
                         ConsoleFourBitToRgb(ConsoleBackgroundIndex(attr)));
}

// WCAG AA's minimum contrast ratio for normal-size text — the bar this
// app's console text (never reliably "large" by WCAG's point-size
// definition) is held to.
constexpr double kWcagAaNormalTextMinContrast = 4.5;

} // namespace mc
