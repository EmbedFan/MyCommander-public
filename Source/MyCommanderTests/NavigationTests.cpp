#include "TestFramework.h"

#include "Navigation.h"

using mc::ClassifyNavigationKey;
using mc::ClickableRegion;
using mc::HitTestPanel;
using mc::MatchClickRegion;
using mc::NavIntent;
using mc::NavKeyContext;
using mc::PanelHit;

// --- Tab (TST-005) -----------------------------------------------------------

TEST_CASE(ClassifyNavigationKey_Tab_AlwaysSwitchesPanel) {
    CHECK(ClassifyNavigationKey(VK_TAB, NavKeyContext{}) == NavIntent::SwitchActivePanel);
    NavKeyContext ctx;
    ctx.ctrlHeld = true;
    ctx.altHeld = true;
    CHECK(ClassifyNavigationKey(VK_TAB, ctx) == NavIntent::SwitchActivePanel);
}

// --- Alt+Left/Right panel history --------------------------------------------

TEST_CASE(ClassifyNavigationKey_AltLeft_WithEmptyCommandLine_GoesBack) {
    NavKeyContext ctx;
    ctx.altHeld = true;
    ctx.commandLineEmpty = true;
    CHECK(ClassifyNavigationKey(VK_LEFT, ctx) == NavIntent::PanelHistoryBack);
}

TEST_CASE(ClassifyNavigationKey_Left_WithoutAlt_DoesNothing) {
    NavKeyContext ctx;
    ctx.altHeld = false;
    ctx.commandLineEmpty = true;
    CHECK(ClassifyNavigationKey(VK_LEFT, ctx) == NavIntent::None);
}

TEST_CASE(ClassifyNavigationKey_AltLeft_WithNonEmptyCommandLine_DoesNothing) {
    // Alt+Left/Right is reserved for panel location history only while the
    // command line is empty — mid-typed text takes priority.
    NavKeyContext ctx;
    ctx.altHeld = true;
    ctx.commandLineEmpty = false;
    CHECK(ClassifyNavigationKey(VK_LEFT, ctx) == NavIntent::None);
}

TEST_CASE(ClassifyNavigationKey_AltRight_WithEmptyCommandLine_GoesForward) {
    NavKeyContext ctx;
    ctx.altHeld = true;
    ctx.commandLineEmpty = true;
    CHECK(ClassifyNavigationKey(VK_RIGHT, ctx) == NavIntent::PanelHistoryForward);
}

// --- Up/Down: command-history browsing vs. cursor movement ------------------

TEST_CASE(ClassifyNavigationKey_Up_WithoutCtrl_MovesCursor) {
    NavKeyContext ctx;
    ctx.ctrlHeld = false;
    ctx.hasCommandHistory = true;  // must not matter when Ctrl isn't held
    CHECK(ClassifyNavigationKey(VK_UP, ctx) == NavIntent::CursorUp);
}

TEST_CASE(ClassifyNavigationKey_CtrlUp_WithHistory_BrowsesHistory) {
    NavKeyContext ctx;
    ctx.ctrlHeld = true;
    ctx.hasCommandHistory = true;
    CHECK(ClassifyNavigationKey(VK_UP, ctx) == NavIntent::CommandHistoryUp);
}

TEST_CASE(ClassifyNavigationKey_CtrlUp_WithNoHistory_IsNoOp) {
    // Matches the original inline code exactly: `if (ctrlHeld &&
    // !commandHistory.empty()) {...} else if (!ctrlHeld) {...}` falls
    // through to neither branch when Ctrl is held but there's no history —
    // it does NOT fall back to moving the cursor.
    NavKeyContext ctx;
    ctx.ctrlHeld = true;
    ctx.hasCommandHistory = false;
    CHECK(ClassifyNavigationKey(VK_UP, ctx) == NavIntent::None);
}

TEST_CASE(ClassifyNavigationKey_Down_WithoutCtrl_MovesCursor) {
    NavKeyContext ctx;
    ctx.ctrlHeld = false;
    ctx.isBrowsingHistory = true;  // must not matter when Ctrl isn't held
    CHECK(ClassifyNavigationKey(VK_DOWN, ctx) == NavIntent::CursorDown);
}

TEST_CASE(ClassifyNavigationKey_CtrlDown_WhileBrowsingHistory_ContinuesBrowsing) {
    NavKeyContext ctx;
    ctx.ctrlHeld = true;
    ctx.isBrowsingHistory = true;
    CHECK(ClassifyNavigationKey(VK_DOWN, ctx) == NavIntent::CommandHistoryDown);
}

TEST_CASE(ClassifyNavigationKey_CtrlDown_NotBrowsingHistory_IsNoOp) {
    NavKeyContext ctx;
    ctx.ctrlHeld = true;
    ctx.isBrowsingHistory = false;
    CHECK(ClassifyNavigationKey(VK_DOWN, ctx) == NavIntent::None);
}

// --- Page Up/Down, Home/End: unconditional cursor movement ------------------

TEST_CASE(ClassifyNavigationKey_PageKeys_AlwaysMoveCursorRegardlessOfModifiers) {
    NavKeyContext ctx;
    ctx.ctrlHeld = true;
    ctx.altHeld = true;
    CHECK(ClassifyNavigationKey(VK_PRIOR, ctx) == NavIntent::CursorPageUp);
    CHECK(ClassifyNavigationKey(VK_NEXT, ctx) == NavIntent::CursorPageDown);
    CHECK(ClassifyNavigationKey(VK_HOME, ctx) == NavIntent::CursorToFirst);
    CHECK(ClassifyNavigationKey(VK_END, ctx) == NavIntent::CursorToLast);
}

// --- Backspace: command-line edit vs. parent directory -----------------------

TEST_CASE(ClassifyNavigationKey_Backspace_WithCommandLineText_ClearsAChar) {
    NavKeyContext ctx;
    ctx.commandLineEmpty = false;
    CHECK(ClassifyNavigationKey(VK_BACK, ctx) == NavIntent::ClearCommandLineChar);
}

TEST_CASE(ClassifyNavigationKey_Backspace_WithEmptyCommandLine_GoesToParent) {
    NavKeyContext ctx;
    ctx.commandLineEmpty = true;
    CHECK(ClassifyNavigationKey(VK_BACK, ctx) == NavIntent::GoToParentDirectory);
}

// --- Non-navigation keys ------------------------------------------------------

TEST_CASE(ClassifyNavigationKey_UnrelatedKeys_ReturnNone) {
    CHECK(ClassifyNavigationKey(VK_F1, NavKeyContext{}) == NavIntent::None);
    CHECK(ClassifyNavigationKey(VK_RETURN, NavKeyContext{}) == NavIntent::None);
    CHECK(ClassifyNavigationKey(VK_INSERT, NavKeyContext{}) == NavIntent::None);
    CHECK(ClassifyNavigationKey(L'A', NavKeyContext{}) == NavIntent::None);
}

// --- HitTestPanel (UI-012) ---------------------------------------------------
// consoleWidth=80 -> leftWidth=40 (left panel interior: columns 1-39),
// rightX=41 (right panel interior: columns 41-78); border columns are 0,
// 40 (the divider), and 79. visibleRows=10 -> entry rows are console rows
// 3-12 (row 3 = TopIndex()+0).

TEST_CASE(HitTestPanel_LeftPanelFirstRow_ReturnsRowOffsetZero) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/3);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->isLeftPanel);
        CHECK(hit->rowOffset == 0);
    }
}

TEST_CASE(HitTestPanel_LeftPanelThirdRow_ReturnsRowOffsetTwo) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/5);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->isLeftPanel);
        CHECK(hit->rowOffset == 2);
    }
}

TEST_CASE(HitTestPanel_RightPanel_ReturnsIsLeftPanelFalse) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/50, /*mouseY=*/3);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(!hit->isLeftPanel);
        CHECK(hit->rowOffset == 0);
    }
}

TEST_CASE(HitTestPanel_LeftBorderColumn_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/0, /*mouseY=*/3).has_value());
}

TEST_CASE(HitTestPanel_DividerColumn_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/40, /*mouseY=*/3).has_value());
}

TEST_CASE(HitTestPanel_RightBorderColumn_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/79, /*mouseY=*/3).has_value());
}

TEST_CASE(HitTestPanel_AboveEntriesArea_IsAMiss) {
    // Rows 0-2 are the top border, path line, and UI-003 volume line.
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/0).has_value());
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/2).has_value());
}

TEST_CASE(HitTestPanel_LastVisibleRow_IsAHit) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/12);
    CHECK(hit.has_value());
    if (hit) CHECK(hit->rowOffset == 9);
}

TEST_CASE(HitTestPanel_OneRowBelowVisibleArea_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/13).has_value());
}

// --- MatchClickRegion (IS-0003) -----------------------------------------------

TEST_CASE(MatchClickRegion_InsideARegion_ReturnsItsIndex) {
    std::vector<ClickableRegion> regions = {{5, 1, 10}, {5, 12, 20}};
    CHECK(MatchClickRegion(regions, 3, 5) == 0);
    CHECK(MatchClickRegion(regions, 15, 5) == 1);
}

TEST_CASE(MatchClickRegion_WrongRow_IsAMiss) {
    std::vector<ClickableRegion> regions = {{5, 1, 10}};
    CHECK(MatchClickRegion(regions, 3, 6) == -1);
}

TEST_CASE(MatchClickRegion_BetweenTwoRegions_IsAMiss) {
    std::vector<ClickableRegion> regions = {{5, 1, 10}, {5, 12, 20}};
    CHECK(MatchClickRegion(regions, 10, 5) == -1);
    CHECK(MatchClickRegion(regions, 11, 5) == -1);
}

TEST_CASE(MatchClickRegion_StartColumnIsInclusive_EndColumnIsExclusive) {
    std::vector<ClickableRegion> regions = {{5, 1, 10}};
    CHECK(MatchClickRegion(regions, 1, 5) == 0);
    CHECK(MatchClickRegion(regions, 9, 5) == 0);
    CHECK(MatchClickRegion(regions, 10, 5) == -1);
    CHECK(MatchClickRegion(regions, 0, 5) == -1);
}

TEST_CASE(MatchClickRegion_EmptyRegions_IsAMiss) {
    CHECK(MatchClickRegion({}, 5, 5) == -1);
}
