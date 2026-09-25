#pragma once

// ACC-002/UI-010: the single source of truth for every Win32 console text
// attribute (foreground/background color pair) the app draws with.
// Previously each of main.cpp/Dialog.cpp/Help.cpp/Viewer.cpp defined its
// own near-identical copies of these constants independently — which is
// exactly how the same low-contrast kAttrStatusBar pattern (light gray on
// green, ~2.8:1, below WCAG AA's 4.5:1 minimum for normal text) ended up
// silently duplicated into three different files rather than caught once.
// Consolidating here means every one of them is defined, and can be
// audited/tested, in exactly one place. See ColorContrastTests.cpp for the
// automated audit (every named palette below, against the console's real
// default 16-color palette, via ColorContrast.h).
//
// UI-010: the constants every draw call site actually reads (kAttrNormal
// etc., just below) are the *currently selected theme* — mutable module
// state, not true compile-time constants, even though they keep their
// original `k`-prefixed names. Renaming all ~90 call sites across
// main.cpp/Dialog.cpp/Help.cpp/Viewer.cpp to a `g`-prefixed name (this
// project's usual convention for mutable module state, e.g. main.cpp's
// `gVisibleColumns`) would be the "clean" choice, but the sheer number of
// call sites makes that a much larger, riskier mechanical change for zero
// behavioral benefit — so the naming inconsistency is accepted
// deliberately here rather than fixed. `ApplyThemePalette` (bottom of this
// file) is the only place meant to reassign them, called once from
// `wmain` right after `Config` loads (main.cpp), the same "resolved once
// at startup, no live-reload path" treatment UI-005's column settings
// already got.
//
// Deliberately declared at global scope (not inside `namespace mc`) so
// every existing call site — whether inside `namespace mc { ... }`
// (Dialog.cpp/Help.cpp/Viewer.cpp) or at global scope (main.cpp) — keeps
// resolving the same unqualified names with no `using` declarations and no
// call-site changes needed.

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

// UI-010: every theme-dependent color bundled as one unit, so a theme can
// be defined, selected, and audited (ColorContrastTests.cpp) as a whole
// rather than as scattered independent constants.
struct ThemePalette {
    WORD attrNormal;
    WORD attrDir;
    WORD attrSelected;
    WORD attrHeaderActive;
    WORD attrHeaderInactive;
    WORD attrStatusBar;
    WORD attrCommandLine;
    WORD attrBroken;
    WORD attrBrokenBanner;
    WORD attrBorder;
    WORD attrTitle;
    WORD attrBody;
    WORD attrSection;
    WORD attrKey;
};

// The original, always-default palette (unchanged from ACC-002's fixes).
constexpr ThemePalette kDefaultThemePalette = {
    /* attrNormal        */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,  // light gray on black, ~11.6:1
    /* attrDir           */ FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // bright green on black, ~15.3:1
    /* attrSelected      */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // bright yellow, ~19.6:1 on black
    /* attrHeaderActive  */ BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
        FOREGROUND_INTENSITY,  // white on dark blue, ~16.0:1
    /* attrHeaderInactive*/ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE,  // light gray on black, ~11.6:1
    /* attrStatusBar     */ BACKGROUND_GREEN | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
        FOREGROUND_INTENSITY,  // bright white on green, ~5.1:1 (ACC-002's fixed floor for this background)
    /* attrCommandLine   */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,  // white on black, ~21:1
    /* attrBroken        */ FOREGROUND_RED | FOREGROUND_INTENSITY,  // bright red on black, ~5.3:1
    /* attrBrokenBanner  */ BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
        FOREGROUND_INTENSITY,  // white on dark red, ~11.0:1
    /* attrBorder        */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,  // white, ~21:1
    /* attrTitle         */ BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // bright yellow on dark blue, ~14.9:1
    /* attrBody          */ BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
        FOREGROUND_INTENSITY,  // white on dark blue, ~16.0:1
    /* attrSection       */ FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // bright green on black, ~15.3:1
    /* attrKey           */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // bright yellow on black, ~19.6:1
};

// UI-010: a high-contrast palette. Everywhere the default palette settled
// for a merely-AA-passing pairing (light gray text, or a colored
// background whose own luminance caps how much contrast any foreground
// choice could reach), this instead uses the maximum achievable pairing —
// bright white text, black background — dropping the green status-bar
// background entirely, since green's own luminance caps that pairing at
// ~5:1 no matter the foreground. Backgrounds that were already excellent
// (dialog chrome's dark blue, ~16:1; the broken-location banner's dark
// red, ~11:1) are kept, since they're already well past any accessibility
// guideline and the color itself carries real meaning (a landmark for
// "this is a dialog", an alarm for "this location is broken"). attrBroken
// stays red-on-black (~5.3:1, the physical ceiling for a red-only
// foreground against black without shifting to a different hue) — its
// real accessibility guarantee is the `<BROKEN>` text tag ACC-001 added,
// not its color, the same reasoning ACC-001 established generally.
constexpr ThemePalette kHighContrastThemePalette = {
    /* attrNormal        */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,  // white on black, ~21:1
    /* attrDir           */ FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // unchanged, already ~15.3:1
    /* attrSelected      */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // unchanged, already ~19.6:1
    /* attrHeaderActive  */ BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
        FOREGROUND_INTENSITY,  // unchanged, already ~16.0:1
    /* attrHeaderInactive*/ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,  // white on black, ~21:1
    /* attrStatusBar     */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,  // white on black (no green bg), ~21:1
    /* attrCommandLine   */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,  // unchanged, already ~21:1
    /* attrBroken        */ FOREGROUND_RED | FOREGROUND_INTENSITY,  // unchanged — see comment above
    /* attrBrokenBanner  */ BACKGROUND_RED | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
        FOREGROUND_INTENSITY,  // unchanged, already ~11.0:1
    /* attrBorder        */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE | FOREGROUND_INTENSITY,  // unchanged, already ~21:1
    /* attrTitle         */ BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // unchanged, already ~14.9:1
    /* attrBody          */ BACKGROUND_BLUE | FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE |
        FOREGROUND_INTENSITY,  // unchanged, already ~16.0:1
    /* attrSection       */ FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // unchanged, already ~15.3:1
    /* attrKey           */ FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_INTENSITY,  // unchanged, already ~19.6:1
};

// The live, currently-selected colors — see this file's own top comment
// for why these keep their original `k`-prefixed names despite no longer
// being true compile-time constants.
inline WORD kAttrNormal = kDefaultThemePalette.attrNormal;
inline WORD kAttrDir = kDefaultThemePalette.attrDir;
inline WORD kAttrSelected = kDefaultThemePalette.attrSelected;
inline WORD kAttrHeaderActive = kDefaultThemePalette.attrHeaderActive;
inline WORD kAttrHeader = kDefaultThemePalette.attrHeaderActive;  // plain alias — see below
inline WORD kAttrHeaderInactive = kDefaultThemePalette.attrHeaderInactive;
inline WORD kAttrStatusBar = kDefaultThemePalette.attrStatusBar;
inline WORD kAttrCommandLine = kDefaultThemePalette.attrCommandLine;
inline WORD kAttrBroken = kDefaultThemePalette.attrBroken;
inline WORD kAttrBrokenBanner = kDefaultThemePalette.attrBrokenBanner;
inline WORD kAttrBorder = kDefaultThemePalette.attrBorder;
inline WORD kAttrTitle = kDefaultThemePalette.attrTitle;
inline WORD kAttrBody = kDefaultThemePalette.attrBody;
inline WORD kAttrSection = kDefaultThemePalette.attrSection;
inline WORD kAttrKey = kDefaultThemePalette.attrKey;

// Mask of just the three foreground color bits (no intensity, no
// background) — used to recolor a row for the cursor highlight while
// keeping its underlying hue (e.g. a selected or broken entry stays
// yellow/red, just brightened and given the cursor's background). A
// structural bitmask, not itself a color choice, so it stays a true
// constant regardless of theme.
constexpr WORD kColorMask = FOREGROUND_RED | FOREGROUND_GREEN | FOREGROUND_BLUE;

// UI-010: overwrites every live kAttr* global above with `palette`'s
// values — called exactly once, from wmain right after Config loads
// (main.cpp), before the input loop starts.
inline void ApplyThemePalette(const ThemePalette& palette) {
    kAttrNormal = palette.attrNormal;
    kAttrDir = palette.attrDir;
    kAttrSelected = palette.attrSelected;
    kAttrHeaderActive = palette.attrHeaderActive;
    kAttrHeader = palette.attrHeaderActive;
    kAttrHeaderInactive = palette.attrHeaderInactive;
    kAttrStatusBar = palette.attrStatusBar;
    kAttrCommandLine = palette.attrCommandLine;
    kAttrBroken = palette.attrBroken;
    kAttrBrokenBanner = palette.attrBrokenBanner;
    kAttrBorder = palette.attrBorder;
    kAttrTitle = palette.attrTitle;
    kAttrBody = palette.attrBody;
    kAttrSection = palette.attrSection;
    kAttrKey = palette.attrKey;
}
