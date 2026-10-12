#include "TestFramework.h"

#include "MenuBar.h"

#include <algorithm>

using mc::BuildMenus;
using mc::CloseMenu;
using mc::DropdownLayout;
using mc::FindItemByMnemonic;
using mc::FindTopMenuByMnemonic;
using mc::LayOutMenuBarRow;
using mc::LayOutOpenDropdown;
using mc::MenuBarState;
using mc::MenuItemKind;
using mc::MoveHighlight;
using mc::MoveToAdjacentTopMenu;
using mc::OpenMenu;
using mc::PlacedMenuEntry;
using mc::TopMenu;

// --- BuildMenus (FEA-0005) -----------------------------------------------

TEST_CASE(BuildMenus_ReturnsExactlySixMenusInRequestOrder_FEA0005) {
    const auto& menus = BuildMenus();
    CHECK(menus.size() == 6);
    if (menus.size() == 6) {
        CHECK(menus[0].label == L"File");
        CHECK(menus[1].label == L"Selection");
        CHECK(menus[2].label == L"Commands");
        CHECK(menus[3].label == L"Network");
        CHECK(menus[4].label == L"View");
        CHECK(menus[5].label == L"Settings");
    }
}

TEST_CASE(BuildMenus_FileMenu_HasEveryRequestedItem_FEA0005) {
    const auto& menus = BuildMenus();
    // FEA-0007: "Pack Files"/"Unpack Package" are no longer built-in stubs --
    // a loaded ZIP plugin registers its own items instead (see
    // BuildMenus_FileMenu_HasNoBuiltinPackStub_FEA0007 below).
    const std::vector<std::wstring> expected = {L"Modify Attribs",
                                                L"Compare contents", L"Assign File to App",
                                                L"Used Disk Area",  L"Group Rename",   L"Generate Check Code",
                                                L"Exit"};
    CHECK(menus[0].items.size() == expected.size());
    for (const auto& label : expected) {
        bool found = std::any_of(menus[0].items.begin(), menus[0].items.end(),
                                 [&](const auto& item) { return item.label == label; });
        CHECK(found);
    }
}

// IS-0009: Exit replays F10 (SimulateKeyPress's existing mechanism), the
// same way every other "this already has a key" item already does --
// MENU-002.
TEST_CASE(BuildMenus_FileMenu_HasNoBuiltinPackStub_FEA0007) {
    const auto& menus = BuildMenus();
    bool foundPack = std::any_of(menus[0].items.begin(), menus[0].items.end(),
                                 [&](const auto& item) { return item.label == L"Pack Files"; });
    CHECK(!foundPack);
    bool foundUnpack = std::any_of(menus[0].items.begin(), menus[0].items.end(),
                                   [&](const auto& item) { return item.label == L"Unpack Package"; });
    CHECK(!foundUnpack);
}

TEST_CASE(BuildMenus_FileMenu_AssignFileToAppIsTheManager_FEA0009) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[0], L'A');
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[0].items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Assign File to App");
        CHECK(item.kind == MenuItemKind::AssignFileToApp);  // no longer a "not yet implemented" stub
    }
}

TEST_CASE(BuildMenus_FileMenu_ExitIsSimulatedKeyF10_IS0009) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[0], L'X');
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[0].items[static_cast<size_t>(idx)];
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == VK_F10);
    }
}

TEST_CASE(BuildMenus_SelectionMenu_CompareFoldersIsShiftF2_SRC006) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[1], L'O');
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[1].items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Compare Folders");
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == VK_F2);
        CHECK(item.action.shiftHeld && !item.action.ctrlHeld && !item.action.altHeld);
    }
}

TEST_CASE(BuildMenus_NetworkMenu_HasEveryRequestedItem_FEA0005) {
    const auto& menus = BuildMenus();
    // EXT-005: the two FTP items now come from the FTP plugin, not built-in stubs.
    const std::vector<std::wstring> expected = {L"Network Connection", L"Share Folder", L"Unshare Folder"};
    CHECK(menus[3].items.size() == expected.size());
    for (const auto& label : expected) {
        bool found = std::any_of(menus[3].items.begin(), menus[3].items.end(),
                                 [&](const auto& item) { return item.label == label; });
        CHECK(found);
    }
}

TEST_CASE(BuildMenus_EveryItemMnemonicIsUniqueWithinItsOwnMenu_FEA0005) {
    for (const auto& menu : BuildMenus()) {
        std::vector<wchar_t> seen;
        for (const auto& item : menu.items) {
            bool duplicate = std::find(seen.begin(), seen.end(), item.mnemonic) != seen.end();
            CHECK(!duplicate);
            seen.push_back(item.mnemonic);
        }
    }
}

TEST_CASE(BuildMenus_EveryTopMenuMnemonicIsUnique_FEA0005) {
    const auto& menus = BuildMenus();
    std::vector<wchar_t> seen;
    for (const auto& menu : menus) {
        bool duplicate = std::find(seen.begin(), seen.end(), menu.mnemonic) != seen.end();
        CHECK(!duplicate);
        seen.push_back(menu.mnemonic);
    }
}

TEST_CASE(BuildMenus_SettingsMnemonicIsE_NotS_FEA0005) {
    // Settings uses E (matching its own "Edit settings" item), since S is
    // Selection's own mnemonic; changed from the original T ("seTtings")
    // per a later polish request.
    const auto& menus = BuildMenus();
    CHECK(menus[5].label == L"Settings");
    CHECK(menus[5].mnemonic == L'E');
}

TEST_CASE(FindTopMenuByMnemonic_SettingsE_ResolvesEvenThoughOtherMenusHaveAnEItemToo_FEA0005) {
    // File's "Generate Check Code" and View's "Executable properties" both
    // use E as their own item mnemonic -- a regression guard that this
    // lookup (used by main.cpp's Alt+<letter> dispatch, which always tries
    // this before an open menu's own item mnemonics) still resolves
    // Settings correctly regardless.
    const auto& menus = BuildMenus();
    CHECK(FindTopMenuByMnemonic(menus, L'E') == 5);
}

TEST_CASE(IsMnemonicKey_OnlyLettersAndDigits_FunctionKeysNeverAliasAMenu) {
    // Regression: VK_F7 == 0x76 == 'v' used to open View on Alt+F7 instead
    // of reaching Find files; VK_F4 == 's' and VK_NUMPAD1 == 'a' alias the
    // same way.
    CHECK(mc::IsMnemonicKey('V'));
    CHECK(mc::IsMnemonicKey('A'));
    CHECK(mc::IsMnemonicKey('Z'));
    CHECK(mc::IsMnemonicKey('0'));
    CHECK(mc::IsMnemonicKey('9'));
    CHECK(!mc::IsMnemonicKey(VK_F7));
    CHECK(!mc::IsMnemonicKey(VK_F4));
    CHECK(!mc::IsMnemonicKey(VK_F1));
    CHECK(!mc::IsMnemonicKey(VK_F12));
    CHECK(!mc::IsMnemonicKey(VK_NUMPAD1));
    CHECK(!mc::IsMnemonicKey(VK_RETURN));
}

TEST_CASE(BuildMenus_ColorModeIsARealToggle_NotAStub_FEA0005) {
    // Confirmed decision: Color mode is a real, working toggle.
    const auto& menus = BuildMenus();
    const TopMenu& settings = menus[5];
    auto it = std::find_if(settings.items.begin(), settings.items.end(),
                           [](const auto& item) { return item.label == L"Color mode"; });
    CHECK(it != settings.items.end());
    if (it != settings.items.end()) CHECK(it->kind == MenuItemKind::ToggleColorMode);
}

TEST_CASE(BuildMenus_TreeIsARealItem_NotAStub_UI013) {
    // UI-013, Part B: "Tree" simulates Ctrl+T instead of showing "not yet implemented".
    const auto& menus = BuildMenus();
    const TopMenu& view = menus[4];
    auto it = std::find_if(view.items.begin(), view.items.end(),
                           [](const auto& item) { return item.label == L"Tree"; });
    CHECK(it != view.items.end());
    if (it != view.items.end()) {
        CHECK(it->kind == MenuItemKind::SimulatedKey);
        CHECK(it->action.virtualKeyCode == static_cast<WORD>(L'T'));
        CHECK(it->action.ctrlHeld);
    }
}

TEST_CASE(BuildMenus_SinglePanelIsCtrlF1_FullAndListStayStubs_UI013) {
    // UI-013, Part A: View > Single panel (P) simulates Ctrl+F1; "Full"/"List"
    // stay reserved for detailed/brief listings.
    const auto& menus = BuildMenus();
    const TopMenu& view = menus[4];
    int idx = FindItemByMnemonic(view, L'P');
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = view.items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Single panel");
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == VK_F1 && item.action.ctrlHeld && !item.action.altHeld);
    }
    for (const auto& item : view.items)
        if (item.label == L"Full" || item.label == L"List") CHECK(item.kind != MenuItemKind::SimulatedKey);
}

TEST_CASE(BuildMenus_FileGroupRenameIsCtrlM_NotAStub_FOP017) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[0], L'G');  // File
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[0].items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Group Rename");
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == static_cast<WORD>(L'M'));
        CHECK(item.action.ctrlHeld);
    }
}

TEST_CASE(BuildMenus_CommandsHasQueueDelete_AltF8_FOP014) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[2], L'E');  // Commands
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[2].items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Queue delete");
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == VK_F8);
        CHECK(item.action.altHeld && !item.action.shiftHeld && !item.action.ctrlHeld);
    }
}

TEST_CASE(BuildMenus_CommandsHasDirectoryHotlist_CtrlD_NAV007) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[2], L'H');  // Commands
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[2].items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Directory Hotlist");
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == static_cast<WORD>(L'D'));
        CHECK(item.action.ctrlHeld);
    }
}

TEST_CASE(BuildMenus_CommandsHasSynchronizeFolders_CtrlShiftF2_SRC007) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[2], L'S');  // Commands
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[2].items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Synchronize Folders");
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == VK_F2);
        CHECK(item.action.ctrlHeld && item.action.shiftHeld && !item.action.altHeld);
    }
}

// --- FindTopMenuByMnemonic / FindItemByMnemonic (FEA-0005) ----------------

TEST_CASE(FindTopMenuByMnemonic_MatchesCaseInsensitively_FEA0005) {
    const auto& menus = BuildMenus();
    CHECK(FindTopMenuByMnemonic(menus, L'F') == 0);
    CHECK(FindTopMenuByMnemonic(menus, L'f') == 0);
    CHECK(FindTopMenuByMnemonic(menus, L'E') == 5);
}

TEST_CASE(FindTopMenuByMnemonic_UnmatchedLetter_ReturnsNegativeOne_FEA0005) {
    CHECK(FindTopMenuByMnemonic(BuildMenus(), L'Z') == -1);
}

TEST_CASE(FindItemByMnemonic_MatchesWithinTheGivenMenu_FEA0005) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[0], L'M');  // File -> Modify Attribs
    CHECK(idx >= 0);
    if (idx >= 0) CHECK(menus[0].items[static_cast<size_t>(idx)].label == L"Modify Attribs");
}

// --- Menu state transitions (FEA-0005) ------------------------------------

TEST_CASE(OpenMenu_SetsActiveAndIndex_FirstItemHighlighted_FEA0005) {
    MenuBarState state = OpenMenu(2);
    CHECK(state.active);
    CHECK(state.openMenuIndex == 2);
    CHECK(state.highlightedItemIndex == 0);
}

TEST_CASE(CloseMenu_ReturnsFullyClosedState_FEA0005) {
    MenuBarState state = CloseMenu();
    CHECK(!state.active);
}

TEST_CASE(MoveToAdjacentTopMenu_WrapsPastTheLastMenu_FEA0005) {
    const auto& menus = BuildMenus();
    MenuBarState state = OpenMenu(5);  // Settings, the last one
    state = MoveToAdjacentTopMenu(state, menus, 1);
    CHECK(state.openMenuIndex == 0);  // wraps to File
    CHECK(state.highlightedItemIndex == 0);
}

TEST_CASE(MoveToAdjacentTopMenu_WrapsBeforeTheFirstMenu_FEA0005) {
    const auto& menus = BuildMenus();
    MenuBarState state = OpenMenu(0);  // File, the first one
    state = MoveToAdjacentTopMenu(state, menus, -1);
    CHECK(state.openMenuIndex == 5);  // wraps to Settings
}

TEST_CASE(MoveToAdjacentTopMenu_ResetsHighlightToFirstItem_FEA0005) {
    const auto& menus = BuildMenus();
    MenuBarState state = OpenMenu(0);
    state = MoveHighlight(state, menus, 1);  // highlight the second item
    state = MoveToAdjacentTopMenu(state, menus, 1);
    CHECK(state.highlightedItemIndex == 0);
}

TEST_CASE(MoveHighlight_WrapsPastTheLastItem_FEA0005) {
    const auto& menus = BuildMenus();
    MenuBarState state = OpenMenu(2);  // Commands
    const int count = static_cast<int>(menus[2].items.size());
    for (int i = 0; i < count; ++i) state = MoveHighlight(state, menus, 1);
    CHECK(state.highlightedItemIndex == 0);  // wrapped back to the first
}

TEST_CASE(MoveHighlight_WrapsBeforeTheFirstItem_FEA0005) {
    const auto& menus = BuildMenus();
    MenuBarState state = OpenMenu(2);
    state = MoveHighlight(state, menus, -1);
    CHECK(state.highlightedItemIndex == static_cast<int>(menus[2].items.size()) - 1);
}

TEST_CASE(MoveHighlight_WhileClosed_IsANoOp_FEA0005) {
    MenuBarState state;  // closed
    MenuBarState after = MoveHighlight(state, BuildMenus(), 1);
    CHECK(!after.active);
    CHECK(after.highlightedItemIndex == 0);
}

// --- LayOutMenuBarRow (FEA-0005) ------------------------------------------
// The six "[X]label" entries need ~69 columns total (bracket notation adds
// overhead LayOutHintBar's own plain-label segments don't have) -- past the
// documented 60-column minimum (UI-006), so at exactly that floor the last
// entry or two is dropped from the row's own layout, the same "defensive,
// some things aren't reachable by click in an extreme case" fallback
// LayOutHintBar's own maxRows cap already established. Unlike the hint
// bar's own actions, this never actually loses keyboard reachability: every
// menu's Alt+<mnemonic> (FindTopMenuByMnemonic) is matched independently of
// whatever LayOutMenuBarRow drew, so a dropped entry is still one Alt+key
// press away regardless.

TEST_CASE(LayOutMenuBarRow_PlacesAllSixEntriesAtAComfortableWidth_FEA0005) {
    auto placed = LayOutMenuBarRow(BuildMenus(), /*width=*/80);
    CHECK(placed.size() == 6);
}

TEST_CASE(LayOutMenuBarRow_AtTheDocumentedMinimumWidth_DropsTheEntriesThatDontFit_FEA0005) {
    // Not all six comfortably fit at 60 columns -- confirms the graceful
    // degradation is "fewer entries," never a crash or an out-of-range
    // write, and that whatever does fit still starts from File in order.
    auto placed = LayOutMenuBarRow(BuildMenus(), /*width=*/60);
    CHECK(placed.size() < 6);
    CHECK(!placed.empty());
    CHECK(placed[0].menuIndex == 0);
}

TEST_CASE(LayOutMenuBarRow_EntriesAreInOrderAndNeverOverlap_FEA0005) {
    auto placed = LayOutMenuBarRow(BuildMenus(), /*width=*/80);
    for (size_t i = 1; i < placed.size(); ++i) {
        CHECK(placed[i].menuIndex == static_cast<int>(i));
        CHECK(placed[i].col >= static_cast<SHORT>(placed[i - 1].col + placed[i - 1].width));
    }
}

TEST_CASE(LayOutMenuBarRow_NoEntryOverflowsTheGivenWidth_FEA0005) {
    auto placed = LayOutMenuBarRow(BuildMenus(), /*width=*/80);
    for (const auto& entry : placed) {
        CHECK(static_cast<int>(entry.col) + entry.width <= 80);
    }
}

TEST_CASE(LayOutMenuBarRow_NonPositiveWidth_ReturnsEmptyWithoutCrashing_FEA0005) {
    CHECK(LayOutMenuBarRow(BuildMenus(), 0).empty());
    CHECK(LayOutMenuBarRow(BuildMenus(), -5).empty());
}

// --- LayOutOpenDropdown (FEA-0005) ----------------------------------------

TEST_CASE(LayOutOpenDropdown_OneRowPerItem_FEA0005) {
    const auto& menus = BuildMenus();
    DropdownLayout layout = LayOutOpenDropdown(menus[0], /*topEntryCol=*/1, /*consoleWidth=*/80);
    CHECK(layout.itemRows.size() == menus[0].items.size());
    for (size_t i = 0; i < layout.itemRows.size(); ++i) {
        CHECK(layout.itemRows[i] == static_cast<SHORT>(1 + static_cast<SHORT>(i)));
    }
}

TEST_CASE(LayOutOpenDropdown_WidthFitsTheLongestItem_FEA0005) {
    const auto& menus = BuildMenus();
    DropdownLayout layout = LayOutOpenDropdown(menus[0], /*topEntryCol=*/1, /*consoleWidth=*/80);
    size_t longest = 0;
    for (const auto& item : menus[0].items) {
        longest = std::max(longest, item.label.size() + 3);  // "[X]" + label
    }
    CHECK(static_cast<size_t>(layout.width) >= longest);
}

TEST_CASE(LayOutOpenDropdown_ClipsLeftWhenTopEntryIsNearTheRightEdge_FEA0005) {
    const auto& menus = BuildMenus();
    // File's own top entry sitting almost at the console's right edge --
    // the dropdown must shift left rather than run off screen.
    DropdownLayout layout = LayOutOpenDropdown(menus[0], /*topEntryCol=*/75, /*consoleWidth=*/80);
    CHECK(static_cast<int>(layout.col) + layout.width <= 80);
}

TEST_CASE(LayOutOpenDropdown_NeverOverflowsARealisticMinimumWidth_FEA0005) {
    // Every menu's own dropdown must fit within the documented 60-column
    // minimum (UI-006) when opened at column 1 -- the narrowest/most
    // forgiving position.
    for (const auto& menu : BuildMenus()) {
        DropdownLayout layout = LayOutOpenDropdown(menu, /*topEntryCol=*/1, /*consoleWidth=*/60);
        CHECK(static_cast<int>(layout.col) + layout.width <= 60);
    }
}

TEST_CASE(BuildMenus_CompareContentsIsAltF3_NotAStub_SRC008) {
    const auto& menus = BuildMenus();
    int idx = FindItemByMnemonic(menus[0], L'C');  // File
    CHECK(idx >= 0);
    if (idx >= 0) {
        const auto& item = menus[0].items[static_cast<size_t>(idx)];
        CHECK(item.label == L"Compare contents");
        CHECK(item.kind == MenuItemKind::SimulatedKey);
        CHECK(item.action.virtualKeyCode == VK_F3 && item.action.altHeld && !item.action.ctrlHeld);
    }
}
