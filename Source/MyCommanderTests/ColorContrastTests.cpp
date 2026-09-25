#include "TestFramework.h"

#include "ColorContrast.h"
#include "Theme.h"

using mc::ConsoleAttrContrastRatio;
using mc::ContrastRatio;
using mc::kWcagAaNormalTextMinContrast;
using mc::RelativeLuminance;
using mc::Rgb8;

// --- Math correctness (independent of the app's own color choices) ------

TEST_CASE(RelativeLuminance_BlackIsZero_WhiteIsOne) {
    CHECK(RelativeLuminance(Rgb8{0, 0, 0}) == 0.0);
    double white = RelativeLuminance(Rgb8{255, 255, 255});
    CHECK(white > 0.9999 && white < 1.0001);  // floating-point sum of the three WCAG coefficients, not exact
}

TEST_CASE(ContrastRatio_SameColorIsOne) {
    CHECK(ContrastRatio(Rgb8{128, 64, 200}, Rgb8{128, 64, 200}) == 1.0);
}

TEST_CASE(ContrastRatio_BlackOnWhiteIsMaximum_21to1) {
    double ratio = ContrastRatio(Rgb8{0, 0, 0}, Rgb8{255, 255, 255});
    CHECK(ratio > 20.9 && ratio < 21.1);
}

TEST_CASE(ContrastRatio_IsOrderIndependent) {
    Rgb8 a{10, 200, 30};
    Rgb8 b{240, 20, 90};
    CHECK(ContrastRatio(a, b) == ContrastRatio(b, a));
}

TEST_CASE(ConsoleAttrContrastRatio_KnownWcagFailingPair) {
    // FOREGROUND_RED|GREEN|BLUE (light gray, index 7) on BACKGROUND_GREEN
    // (dark green, index 2) — the exact pattern this app's kAttrStatusBar
    // used to be before the ACC-002 fix. Documents *why* it needed fixing:
    // this computes to well under WCAG AA's 4.5:1 minimum.
    WORD lightGrayOnDarkGreen = BACKGROUND_GREEN | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;
    double ratio = ConsoleAttrContrastRatio(lightGrayOnDarkGreen);
    CHECK(ratio < kWcagAaNormalTextMinContrast);
    CHECK(ratio > 2.5 && ratio < 3.0);
}

// --- ACC-002/UI-010: every color pair every named theme actually defines ---

namespace {
struct NamedAttr {
    const wchar_t* name;
    WORD attr;
};

// Every field of a ThemePalette, named — used to audit each named theme
// (kDefaultThemePalette, kHighContrastThemePalette) as data, rather than
// through the mutable "live" kAttr* globals, which only ever reflect
// whichever theme a test happens to have applied (or, in this test binary,
// whichever the static-initializer defaults to) — auditing the palettes
// themselves means every theme is checked regardless of which one (if any)
// is currently live. Kept as an explicit list (rather than, say,
// reflecting over ThemePalette) so a contributor adding a new field to the
// struct is forced to also add a line here — the whole point of this test
// is that a new low-contrast pair can't slip into any theme unaudited the
// way kAttrStatusBar's did originally.
std::vector<NamedAttr> PaletteEntries(const ThemePalette& p) {
    return {
        {L"attrNormal", p.attrNormal},
        {L"attrDir", p.attrDir},
        {L"attrSelected", p.attrSelected},
        {L"attrHeaderActive", p.attrHeaderActive},
        {L"attrHeaderInactive", p.attrHeaderInactive},
        {L"attrStatusBar", p.attrStatusBar},
        {L"attrCommandLine", p.attrCommandLine},
        {L"attrBroken", p.attrBroken},
        {L"attrBrokenBanner", p.attrBrokenBanner},
        {L"attrBorder", p.attrBorder},
        {L"attrTitle", p.attrTitle},
        {L"attrBody", p.attrBody},
        {L"attrSection", p.attrSection},
        {L"attrKey", p.attrKey},
    };
}

struct NamedPalette {
    const wchar_t* name;
    ThemePalette palette;
};

const NamedPalette kAllThemes[] = {
    {L"Default", kDefaultThemePalette},
    {L"HighContrast", kHighContrastThemePalette},
};
}  // namespace

TEST_CASE(Theme_EveryFieldOfEveryNamedTheme_MeetsWcagAaNormalTextContrast_ACC002_UI010) {
    for (const auto& theme : kAllThemes) {
        for (const auto& entry : PaletteEntries(theme.palette)) {
            double ratio = ConsoleAttrContrastRatio(entry.attr);
            if (ratio < kWcagAaNormalTextMinContrast) {
                // FAIL rather than CHECK so a future regression names
                // exactly which theme/field fell below the bar and by how
                // much, instead of every failing iteration reporting the
                // same bare "ratio >= kWcagAaNormalTextMinContrast"
                // expression text.
                FAIL(std::wstring(theme.name) + L"." + entry.name + L" only has " + std::to_wstring(ratio) +
                    L":1 contrast, below the " + std::to_wstring(kWcagAaNormalTextMinContrast) +
                    L":1 WCAG AA minimum");
            } else {
                CHECK(ratio >= kWcagAaNormalTextMinContrast);
            }
        }
    }
}

// UI-010: proves the high-contrast theme is a genuine improvement over the
// default, not just a relabeling — the three fields it actually changes
// (attrNormal/attrHeaderInactive: light gray -> bright white; attrStatusBar:
// drops its green background) should each land close to the 21:1
// theoretical maximum, clearing the default theme's own ratio for the same
// field by a wide margin.
TEST_CASE(HighContrastTheme_ImprovedFields_AreSubstantiallyHigherThanDefault_UI010) {
    constexpr double kNearMaxContrast = 20.0;

    double defaultNormal = ConsoleAttrContrastRatio(kDefaultThemePalette.attrNormal);
    double hcNormal = ConsoleAttrContrastRatio(kHighContrastThemePalette.attrNormal);
    CHECK(hcNormal >= kNearMaxContrast);
    CHECK(hcNormal > defaultNormal);

    double defaultHeaderInactive = ConsoleAttrContrastRatio(kDefaultThemePalette.attrHeaderInactive);
    double hcHeaderInactive = ConsoleAttrContrastRatio(kHighContrastThemePalette.attrHeaderInactive);
    CHECK(hcHeaderInactive >= kNearMaxContrast);
    CHECK(hcHeaderInactive > defaultHeaderInactive);

    double defaultStatusBar = ConsoleAttrContrastRatio(kDefaultThemePalette.attrStatusBar);
    double hcStatusBar = ConsoleAttrContrastRatio(kHighContrastThemePalette.attrStatusBar);
    CHECK(hcStatusBar >= kNearMaxContrast);
    CHECK(hcStatusBar > defaultStatusBar);
}

// The cursor-row highlight in main.cpp's DrawPanel doesn't draw with a
// ThemePalette field directly for every case — attrSelected/attrDir/etc.
// get their color bits masked and recombined with BACKGROUND_BLUE (the
// cursor highlight) at the call site. Exercise those actual combinations
// here too, for every named theme, since they're exactly as visible to a
// user as anything in the palette table above and were the source of the
// one gap that table alone can't see (attrBroken + BACKGROUND_BLUE,
// ~4.0:1 for the default theme's red — fixed in main.cpp by special-casing
// that one combination to attrBrokenBanner instead, for every theme).
TEST_CASE(CursorRowHighlight_EveryColorMaskedOntoBackgroundBlue_MeetsWcagAa_ACC002_UI010) {
    for (const auto& theme : kAllThemes) {
        WORD colorsSeenAtCursorRow[] = {theme.palette.attrNormal, theme.palette.attrDir, theme.palette.attrSelected};
        for (WORD baseAttr : colorsSeenAtCursorRow) {
            WORD cursorAttr = static_cast<WORD>((baseAttr & kColorMask) | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
            double ratio = ConsoleAttrContrastRatio(cursorAttr);
            CHECK(ratio >= kWcagAaNormalTextMinContrast);
        }
        // attrBroken is deliberately excluded above: main.cpp's DrawPanel
        // never actually forms `(attrBroken & kColorMask) | ... |
        // BACKGROUND_BLUE` for a real row regardless of theme — it
        // substitutes attrBrokenBanner instead, already covered by the
        // table-driven test above. This documents that exclusion is
        // intentional, not an oversight: recompute the same masked
        // combination to confirm it would still fail if anyone
        // reintroduced it.
        WORD wouldBeBrokenCursorAttr =
            static_cast<WORD>((theme.palette.attrBroken & kColorMask) | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
        CHECK(ConsoleAttrContrastRatio(wouldBeBrokenCursorAttr) < kWcagAaNormalTextMinContrast);
    }
}
