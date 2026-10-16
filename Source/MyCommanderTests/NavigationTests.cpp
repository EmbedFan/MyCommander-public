#include "TestFramework.h"

#include "Navigation.h"
#include "TextWidth.h"

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

// --- HitTestPanel (UI-012/FEA-0005) -------------------------------------------
// consoleWidth=80 -> leftWidth=40 (left panel interior: columns 1-39),
// rightX=41 (right panel interior: columns 41-78); border columns are 0,
// 40 (the divider), and 79. visibleRows=10, topOffset=1 (FEA-0005's
// always-present menu-bar row, the real production value) -> entry rows
// are console rows 4-13 (row 4 = TopIndex()+0: menu bar(0) + border(1) +
// path line(2) + volume line(3)).

TEST_CASE(HitTestPanel_LeftPanelFirstRow_ReturnsRowOffsetZero) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/4, /*topOffset=*/1);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->isLeftPanel);
        CHECK(hit->rowOffset == 0);
    }
}

TEST_CASE(HitTestPanel_LeftPanelThirdRow_ReturnsRowOffsetTwo) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/6, /*topOffset=*/1);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(hit->isLeftPanel);
        CHECK(hit->rowOffset == 2);
    }
}

TEST_CASE(HitTestPanel_RightPanel_ReturnsIsLeftPanelFalse) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/50, /*mouseY=*/4, /*topOffset=*/1);
    CHECK(hit.has_value());
    if (hit) {
        CHECK(!hit->isLeftPanel);
        CHECK(hit->rowOffset == 0);
    }
}

TEST_CASE(HitTestPanel_LeftBorderColumn_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/0, /*mouseY=*/4, /*topOffset=*/1).has_value());
}

TEST_CASE(HitTestPanel_DividerColumn_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/40, /*mouseY=*/4, /*topOffset=*/1).has_value());
}

TEST_CASE(HitTestPanel_RightBorderColumn_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/79, /*mouseY=*/4, /*topOffset=*/1).has_value());
}

TEST_CASE(HitTestPanel_AboveEntriesArea_IsAMiss) {
    // Rows 0-3 are the menu bar, top border, path line, and UI-003 volume line.
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/0, /*topOffset=*/1).has_value());
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/3, /*topOffset=*/1).has_value());
}

TEST_CASE(HitTestPanel_LastVisibleRow_IsAHit) {
    auto hit = HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/13, /*topOffset=*/1);
    CHECK(hit.has_value());
    if (hit) CHECK(hit->rowOffset == 9);
}

TEST_CASE(HitTestPanel_OneRowBelowVisibleArea_IsAMiss) {
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/14, /*topOffset=*/1).has_value());
}

TEST_CASE(HitTestPanel_ZeroTopOffset_MatchesTheNoMenuBarGeometry) {
    // A regression guard for topOffset's own arithmetic, independent of
    // FEA-0005's real production value (1) above: with no reservation at
    // all, entries start right after border+path+volume (row 3).
    auto hit = HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/3, /*topOffset=*/0);
    CHECK(hit.has_value());
    if (hit) CHECK(hit->rowOffset == 0);
    CHECK(!HitTestPanel(80, 10, /*mouseX=*/5, /*mouseY=*/2, /*topOffset=*/0).has_value());
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

// --- PanelLayout / FormatOtherPanelLabel (UI-013 Part A) ------------------------

TEST_CASE(PanelLayout_DualPane_MatchesTheOriginalSplitAtEveryWidth_UI013) {
    for (int w = 60; w <= 300; ++w) {
        SHORT width = static_cast<SHORT>(w);
        for (bool leftActive : {true, false}) {
            mc::PanelLayout layout = mc::ComputePanelLayout(width, leftActive, false);
            SHORT leftWidth = static_cast<SHORT>(width / 2);              // DrawFrame's formula before UI-013
            SHORT rightWidth = static_cast<SHORT>(width - leftWidth - 1);
            CHECK(layout.showLeft && layout.showRight);
            CHECK(layout.leftX == 1 && layout.leftWidth == leftWidth - 1);
            CHECK(layout.dividerX == leftWidth);
            CHECK(layout.rightX == leftWidth + 1 && layout.rightWidth == rightWidth - 1);
            CHECK(layout.rightX + layout.rightWidth == width - 1);  // ends at the right border
        }
    }
}

TEST_CASE(PanelLayout_SinglePanel_ShowsOnlyTheActiveSideFullWidth_UI013) {
    mc::PanelLayout left = mc::ComputePanelLayout(80, /*leftActive=*/true, /*singlePanel=*/true);
    CHECK(left.showLeft && !left.showRight && left.dividerX == -1);
    CHECK(left.leftX == 1 && left.leftWidth == 78);
    mc::PanelLayout right = mc::ComputePanelLayout(60, /*leftActive=*/false, /*singlePanel=*/true);
    CHECK(!right.showLeft && right.showRight && right.dividerX == -1);
    CHECK(right.rightX == 1 && right.rightWidth == 58);
}

TEST_CASE(HitTestPanel_SinglePanel_HitsOnlyTheShownSide_UI013) {
    for (bool leftActive : {true, false}) {
        mc::PanelLayout layout = mc::ComputePanelLayout(80, leftActive, true);
        for (SHORT x = 1; x <= 78; ++x) {
            auto hit = HitTestPanel(layout, 10, x, /*mouseY=*/4, /*topOffset=*/1);
            CHECK(hit.has_value() && hit->isLeftPanel == leftActive);
        }
        CHECK(!HitTestPanel(layout, 10, 0, 4, 1).has_value());   // left border
        CHECK(!HitTestPanel(layout, 10, 79, 4, 1).has_value());  // right border
    }
}

TEST_CASE(HitTestPanel_LayoutOverload_AgreesWithTheWidthOverload_UI013) {
    mc::PanelLayout layout = mc::ComputePanelLayout(101, true, false);
    for (SHORT x = 0; x < 101; ++x) {
        auto a = HitTestPanel(101, 10, x, 5, 1);
        auto b = HitTestPanel(layout, 10, x, 5, 1);
        CHECK(a.has_value() == b.has_value());
        if (a && b) CHECK(a->isLeftPanel == b->isLeftPanel && a->rowOffset == b->rowOffset);
    }
}

TEST_CASE(FormatOtherPanelLabel_FitsTruncatesAndGivesUp_UI013) {
    CHECK(mc::FormatOtherPanelLabel(L"D:\\Backup", 40) == L" Other panel: D:\\Backup ");
    std::wstring longPath = L"C:\\Users\\someone\\Documents\\Projects\\MyCommander\\Source";
    std::wstring cut = mc::FormatOtherPanelLabel(longPath, 40);
    CHECK(mc::StringDisplayWidth(cut) <= 40 && cut.find(L" Other panel: ...") == 0 && cut.find(L"Source ") != std::wstring::npos);
    std::wstring wide = mc::FormatOtherPanelLabel(L"C:\\\u6587\u66f8\u6587\u66f8\u6587\u66f8\u6587\u66f8\u6587\u66f8\u6587\u66f8", 30);
    CHECK(!wide.empty() && mc::StringDisplayWidth(wide) <= 30);  // full-width characters count double (UI-011)
    CHECK(mc::FormatOtherPanelLabel(L"D:\\Backup", 18).empty());  // no room: no label
}

// --- UI-017: Brief view geometry --------------------------------------------

TEST_CASE(ComputeBriefLayout_NarrowNames_ClampsColumnWidthToTwelve_UI017) {
    mc::BriefLayout layout = mc::ComputeBriefLayout(10, 100, 5);
    CHECK(layout.columnWidth == 12);
    CHECK(layout.columns == 100 / 12);
}

TEST_CASE(ComputeBriefLayout_LongNames_ClampsColumnWidthToHalfTheWidth_UI017) {
    mc::BriefLayout layout = mc::ComputeBriefLayout(10, 80, 90);
    CHECK(layout.columnWidth == 40);  // 80/2, since 90 would exceed half the panel's width
    CHECK(layout.columns == 2);
}

TEST_CASE(ComputeBriefLayout_MediumNames_UsesTheNameWidthDirectly_UI017) {
    mc::BriefLayout layout = mc::ComputeBriefLayout(10, 100, 20);
    CHECK(layout.columnWidth == 20);
    CHECK(layout.columns == 5);
}

TEST_CASE(ComputeBriefLayout_NeverDegeneratesOnZeroWidth_UI017) {
    mc::BriefLayout layout = mc::ComputeBriefLayout(10, 0, 0);
    CHECK(layout.columns >= 1);
    CHECK(layout.columnWidth >= 12);
}

TEST_CASE(BriefEntryAt_IsColumnMajor_UI017) {
    // 3 rows per column: column 0 is indices 0,1,2; column 1 is 3,4,5; ...
    CHECK(mc::BriefEntryAt(0, 3, 0, 0) == 0);
    CHECK(mc::BriefEntryAt(0, 3, 0, 2) == 2);
    CHECK(mc::BriefEntryAt(0, 3, 1, 0) == 3);
    CHECK(mc::BriefEntryAt(0, 3, 2, 1) == 7);
}

TEST_CASE(BriefEntryAt_OffsetsByFirstVisible_UI017) {
    CHECK(mc::BriefEntryAt(30, 3, 1, 1) == 30 + 3 + 1);
}

TEST_CASE(BriefFirstVisible_SnapsDownToThePageContainingTheCursor_UI017) {
    // 2 columns x 3 rows = a 6-entry page.
    CHECK(mc::BriefFirstVisible(0, 2, 3) == 0);
    CHECK(mc::BriefFirstVisible(5, 2, 3) == 0);
    CHECK(mc::BriefFirstVisible(6, 2, 3) == 6);
    CHECK(mc::BriefFirstVisible(11, 2, 3) == 6);
    CHECK(mc::BriefFirstVisible(12, 2, 3) == 12);
}
