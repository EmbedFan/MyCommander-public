#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <optional>
#include <vector>

namespace mc {

// TST-005: what a navigation-relevant key press should do, pulled out of
// wmain's own key-dispatch switch (main.cpp) so the non-obvious parts of it
// — in particular the Ctrl+Up/Down vs. plain Up/Down disambiguation between
// browsing command history and moving the panel cursor, and Alt+Left/Right
// only firing when the command line is empty — are independently,
// repeatably testable without a live console/input loop. Deliberately
// covers only genuinely navigational keys (Tab, panel history, command
// history browsing, cursor movement, Backspace); Enter, selection keys,
// function-key actions, and ordinary command-line text entry stay in
// wmain's own switch, since those are action dispatch or text editing, not
// navigation.
enum class NavIntent {
    None,               // not a navigation key, or a navigation key whose guard wasn't met (a no-op, matching the
                        // original inline code exactly — e.g. Ctrl+Up with no command history to browse)
    SwitchActivePanel,  // Tab
    PanelHistoryBack,   // Alt+Left
    PanelHistoryForward,// Alt+Right
    CommandHistoryUp,   // Ctrl+Up, browsing command history
    CommandHistoryDown, // Ctrl+Down, browsing command history
    CursorUp,           // Up
    CursorDown,         // Down
    CursorPageUp,       // Page Up
    CursorPageDown,     // Page Down
    CursorToFirst,      // Home
    CursorToLast,       // End
    ClearCommandLineChar,  // Backspace, command line has text
    GoToParentDirectory,   // Backspace, command line empty
};

// The pieces of wmain's own state needed to disambiguate a navigation key —
// nothing else (no Panel/Console dependency), so this stays pure.
struct NavKeyContext {
    bool ctrlHeld = false;
    bool altHeld = false;
    bool commandLineEmpty = true;
    bool hasCommandHistory = false;  // !commandHistory.empty()
    bool isBrowsingHistory = false;  // historyIndex != -1
};

NavIntent ClassifyNavigationKey(WORD virtualKeyCode, const NavKeyContext& ctx);

// UI-012: which panel, and which visible row within it, a mouse event's
// console-cell coordinates landed on — a pure function of the console's
// width and the active panel-content geometry (no Console/Panel
// dependency), for the same testability reasons as ClassifyNavigationKey
// above. Column ranges mirror main.cpp's DrawFrame layout (the left border
// at column 0, the divider at consoleWidth/2, the right border at
// consoleWidth-1) and rowOffset's origin mirrors DrawPanel's own row
// layout (path line, then the UI-003 volume line, then entries starting
// at row 3) — kept in sync by hand with those two functions since neither
// currently exposes its geometry as reusable data; a future layout change
// needs the same edit here.
struct PanelHit {
    bool isLeftPanel = false;
    int rowOffset = 0;  // 0-based, relative to the panel's own TopIndex()
};

// `visibleRows` is the panel content's row count (main.cpp's
// ComputeVisibleRows(console)) — passed in rather than derived here so
// this function needs only plain values, not a live Console.
std::optional<PanelHit> HitTestPanel(SHORT consoleWidth, int visibleRows, SHORT mouseX, SHORT mouseY);

// IS-0003: a single row-bounded, column-range clickable area -- generic
// enough to be shared by both the bottom hint bar's per-command segments
// (main.cpp) and a dialog's "[X]Label" option segments (Dialog.cpp), rather
// than each having its own near-duplicate "which labeled column-range did
// this click land in" implementation. `endCol` is exclusive.
struct ClickableRegion {
    SHORT row = 0;
    SHORT startCol = 0;
    SHORT endCol = 0;
};

// Returns the index of the first region in `regions` containing
// (mouseX, mouseY), or -1 if none matches.
int MatchClickRegion(const std::vector<ClickableRegion>& regions, SHORT mouseX, SHORT mouseY);

}  // namespace mc
