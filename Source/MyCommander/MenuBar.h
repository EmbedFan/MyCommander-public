#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include "HintBar.h"
#include "TextWidth.h"

#include <algorithm>
#include <cstdint>
#include <cwctype>
#include <string>
#include <vector>

namespace mc {

// FEA-0005: the always-visible top menu bar's static structure. Pure, no
// Console dependency — same "structure + layout as plain data, pure
// functions" shape HintBar.h/Navigation.h already established, so every
// piece of this is independently unit-testable.
//
// Most requested menu items name features MyCommander doesn't have yet
// (archive packing, FTP, disk-usage scanning, a tree view, and so on — the
// request itself says wiring those up is not part of this ask). Each item
// is one of four kinds:
//   - SimulatedKey: already exists under some other key today; selecting
//     the item just injects that same key press (main.cpp's SimulateKeyPress
//     — already built for IS-0003's hint-bar clicks, reused here rather
//     than a second, parallel action-dispatch table).
//   - ToggleColorMode: the one genuinely new (if trivial) piece of logic —
//     flips Config::colorTheme and saves it, main.cpp's own job.
//   - Stub: nothing to call yet; selecting it shows a plain "not yet
//     implemented" message and does nothing else.
//   - PluginCommand (FEA-0002): registered at runtime by a loaded MC
//     Plugin via PluginManager::HostAddMenuItem, never present in
//     BuildMenus()'s own static list — selecting it calls
//     PluginManager::DispatchMenuCommand(pluginId, pluginCommandId)
//     instead of SimulateKeyPress, since a plugin command has no existing
//     key press to replay.
//   - AssignFileToApp (FEA-0009 Phase 2): opens the file-actions manager
//     for the cursor file's extension -- dialog-driven, with no key of its
//     own to simulate.
//   - EditKeyBindings (KEY-005): opens keys.conf in the editor.
//   - CheckCode (FOP-021), DiskLabel (NAV-013), NetworkConnection (NAV-014),
//     ShareFolder/UnshareFolder (NAV-015): MENU-STUBS' own menu-only items --
//     dialog-driven like AssignFileToApp/EditKeyBindings, no key of their own
//     to simulate.
enum class MenuItemKind {
    SimulatedKey, ToggleColorMode, Stub, PluginCommand, AssignFileToApp, EditKeyBindings,
    CheckCode, DiskLabel, NetworkConnection, ShareFolder, UnshareFolder, FindEmptyFolders,
    RestoreFromBin, SecureDelete, ExportListing, CreateShortcut, RevealInExplorer,
};

// `label` is the item's full displayed word (not trimmed around the
// mnemonic) — rendered as "[" + mnemonic + "]" + label, e.g. "[F]ile",
// "[T]Settings" — so a mnemonic that isn't the label's own first letter
// (Settings' T, since S is Selection's) still renders unambiguously,
// unlike Dialog.h's DialogOption convention of trimming the leading letter
// out of the label (which only reads cleanly when the hotkey *is* the
// first letter).
struct MenuItem {
    std::wstring label;
    wchar_t mnemonic = 0;
    MenuItemKind kind = MenuItemKind::Stub;
    HintAction action;  // meaningful only when kind == SimulatedKey
    // FEA-0002: meaningful only when kind == PluginCommand -- which
    // plugin owns this item (so DispatchMenuCommand can find its
    // OnMenuAction) and the commandId that plugin chose at registration
    // time (opaque to the host, just handed back unchanged).
    std::wstring pluginId;
    uint32_t pluginCommandId = 0;
    // KEY-005: the item's current keyboard shortcut, shown right-aligned in
    // the dropdown ("Ctrl+M"). Filled in by main.cpp from the live key map
    // each time the menus are merged; empty = nothing shown.
    std::wstring keyText;
};

struct TopMenu {
    std::wstring label;
    wchar_t mnemonic = 0;
    std::vector<MenuItem> items;
};

// The six top menus in the request's own order, with every requested item
// present. Item mnemonics are assigned per-menu (checked for uniqueness
// only within their own dropdown, not globally — two different menus can
// each have their own "F" item) and hand-picked where a menu's own first
// letters collide (e.g. File's "Unpack Package" and "Used Disk Area" both
// start with U — the second uses D instead, from "Disk").
inline const std::vector<TopMenu>& BuildMenus() {
    auto real = [](const wchar_t* label, wchar_t mnemonic, WORD vk, bool ctrl = false, bool shift = false,
                    bool alt = false) {
        return MenuItem{label, mnemonic, MenuItemKind::SimulatedKey, HintAction{vk, ctrl, shift, alt}};
    };
    auto stub = [](const wchar_t* label, wchar_t mnemonic) {
        return MenuItem{label, mnemonic, MenuItemKind::Stub, HintAction{}};
    };

    static const std::vector<TopMenu> menus = {
        {L"File", L'F',
         {
             real(L"Modify Attribs", L'M', L'A', /*ctrl=*/true),
             // FEA-0007: the built-in "Pack Files"/"Unpack Package" stubs
             // are both removed entirely (not just left as stubs) -- a
             // loaded ZIP plugin registers its own "Pack file"/"Unpack
             // Package" items here via AddMenuItem instead. With no such
             // plugin installed, the File menu simply has no
             // packing/unpacking items at all, fewer stubs rather than dead
             // ones.
             // SRC-008: the text difference viewer (was a stub since FEA-0005).
             real(L"Compare contents", L'C', VK_F3, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             // FEA-0009 Phase 2: the file-actions manager (was a stub since FEA-0005).
             {L"Assign File to App", L'A', MenuItemKind::AssignFileToApp, HintAction{}},
             // FOP-020: recursive used-disk-area scan (was a stub since FEA-0005).
             real(L"Used Disk Area", L'D', VK_RETURN, /*ctrl=*/false, /*shift=*/true, /*alt=*/true),
             // FOP-017: the group rename (was a stub since FEA-0005).
             real(L"Group Rename", L'G', static_cast<WORD>(L'M'), /*ctrl=*/true),
             // FOP-022: undo the last reversible file operation (Ctrl+Z).
             real(L"Undo", L'U', static_cast<WORD>(L'Z'), /*ctrl=*/true),
             // FOP-025: paste files copied or cut in Windows Explorer (Alt+Shift+V).
             real(L"Paste from Explorer", L'P', static_cast<WORD>(L'V'), /*ctrl=*/false, /*shift=*/true, /*alt=*/true),
             // FOP-026: .lnk files for the selection, in this folder or on the desktop (menu only).
             {L"Create shortcut", L'H', MenuItemKind::CreateShortcut, HintAction{}},
             // FOP-024: overwrite then delete (menu only, always confirmed).
             {L"Secure delete", L'S', MenuItemKind::SecureDelete, HintAction{}},
             // FOP-021: generate/verify checksums (was a stub since FEA-0005); menu-only.
             {L"Generate Check Code", L'E', MenuItemKind::CheckCode, HintAction{}},
             // IS-0009: 'X' (not 'E' -- already Generate Check Code's own
             // mnemonic within this menu) matches Total Commander's own
             // File > Exit, Alt+X convention this app is modeled on.
             real(L"Exit", L'X', VK_F10),
         }},
        {L"Selection", L'S',
         {
             real(L"Select group +", L'S', VK_ADD),
             real(L"Unselect group -", L'U', VK_SUBTRACT),
             real(L"Toggle Selection", L'T', VK_INSERT),
             // SEL-008: copy names/paths/details to the clipboard (was a stub since FEA-0005).
             real(L"Copy Selected Names to Clipboard", L'C', static_cast<WORD>(L'C'), /*ctrl=*/true, /*shift=*/true),
             // SRC-006: compare the two panels' folders (was a stub since FEA-0005).
             real(L"Compare Folders", L'O', VK_F2, /*ctrl=*/false, /*shift=*/true),
             // LIST-009: colour labels (Ctrl+L cycles, Ctrl+Shift+L opens the chooser).
             real(L"Cycle colour label", L'L', static_cast<WORD>(L'L'), /*ctrl=*/true),
             real(L"Colour labels...", L'B', static_cast<WORD>(L'L'), /*ctrl=*/true, /*shift=*/true),
         }},
        {L"Commands", L'C',
         {
             real(L"Find", L'F', VK_F7, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             // NAV-013: show/change the active panel's volume label (was a stub since FEA-0005); menu-only.
             {L"Disk Label", L'D', MenuItemKind::DiskLabel, HintAction{}},
             real(L"Command Line", L'C', VK_F9),
             // FOP-014: queue the copy/move as a background job instead of blocking.
             real(L"Queue copy", L'Y', VK_F5, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             real(L"Queue move", L'V', VK_F6, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             // FOP-014: queued Recycle Bin delete (Alt+Shift+F8, permanent, stays key-only like Shift+F8).
             real(L"Queue delete", L'E', VK_F8, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             real(L"Job queue", L'J', static_cast<WORD>(L'Q'), /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             // NAV-007: the named favorite folders (Ctrl+D).
             real(L"Directory Hotlist", L'H', static_cast<WORD>(L'D'), /*ctrl=*/true),
             // SRC-009: list (and optionally remove) the empty folders below this one.
             {L"Find empty folders", L'M', MenuItemKind::FindEmptyFolders, HintAction{VK_F7, false, true, true}},
             // NAV-018: complete the path typed on the command line (Ctrl+Space).
             real(L"Complete path", L'P', VK_SPACE, /*ctrl=*/true),
             // CLI-008: search the command history by a fragment (Alt+R).
             real(L"History search", L'I', static_cast<WORD>(L'R'), /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             // NAV-017: the recently visited folders (Alt+F12).
             real(L"Recent folders", L'R', VK_F12, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             // SRC-010: the largest files below this folder (Ctrl+F12).
             real(L"Largest files", L'L', VK_F12, /*ctrl=*/true),
             // FOP-023: put items back from the Recycle Bin (menu only).
             {L"Restore from Recycle Bin", L'B', MenuItemKind::RestoreFromBin, HintAction{}},
             // LIST-012: write the listing (or the selection) to a CSV / text file (menu only).
             {L"Export listing", L'X', MenuItemKind::ExportListing, HintAction{}},
             // NAV-016: exchange the two panels (Ctrl+U).
             real(L"Swap panels", L'W', static_cast<WORD>(L'U'), /*ctrl=*/true),
             // SRC-007: synchronize the two panels' folders (preview + confirmation).
             real(L"Synchronize Folders", L'S', VK_F2, /*ctrl=*/true, /*shift=*/true),
         }},
        {L"Network", L'N',
         {
             // NAV-014/015 (was stubs since FEA-0005); all three menu-only.
             {L"Network Connection", L'N', MenuItemKind::NetworkConnection, HintAction{}},
             {L"Share Folder", L'S', MenuItemKind::ShareFolder, HintAction{}},
             {L"Unshare Folder", L'U', MenuItemKind::UnshareFolder, HintAction{}},
             // EXT-005 / FTP-001: the "FTP connection" (F) and "New FTP
             // connection" (W) stubs are removed -- the FTP plugin registers
             // both here via AddMenuItem, the same way FEA-0007 replaced the
             // File menu's Pack/Unpack stubs.
         }},
        {L"View", L'V',
         {
             // UI-017: Full (detailed, today's rows) / Brief (names-only columns);
             // were stubs since FEA-0005.
             real(L"Full", L'F', VK_F7, /*ctrl=*/true),
             real(L"List", L'L', VK_F8, /*ctrl=*/true),
             // UI-013, Part B: toggles the active panel between an
             // ordinary listing and a lazily-loaded directory tree.
             real(L"Tree", L'T', static_cast<WORD>(L'T'), /*ctrl=*/true),
             // UI-013, Part A: the active panel alone, full width. "Full"
             // and "List" above stay reserved for detailed/brief listings.
             real(L"Single panel", L'P', VK_F1, /*ctrl=*/true),
             real(L"Executable properties", L'E', VK_RETURN, /*ctrl=*/false, /*shift=*/false, /*alt=*/true),
             // NAV-019: open Windows Explorer on this folder with the cursor entry selected (menu only).
             {L"Show in Explorer", L'O', MenuItemKind::RevealInExplorer, HintAction{}},
             real(L"Order by Name", L'N', VK_F3, /*ctrl=*/true),
             // LIST-011: the extension sort *is* the type sort (the Type column shows the extension).
             real(L"Order by Extension (type)", L'X', VK_F4, /*ctrl=*/true),
             real(L"Order by Size", L'S', VK_F5, /*ctrl=*/true),
             // LIST-011: read-only, hidden, system, archive, reparse point groups.
             real(L"Order by Attributes", L'A', VK_F9, /*ctrl=*/true),
         }},
        // Settings' own top-level mnemonic is E, not T or S: S is
        // Selection's; E matches its own first (and most-used) item, "Edit
        // settings" — deliberately the same letter as one of this menu's
        // own items (harmless: main.cpp's Alt+<mnemonic> dispatch always
        // tries a top-menu switch before an *open* menu's own item
        // mnemonics, precisely so a collision like this — or File's
        // "Generate Check Code"/View's "Executable properties" both also
        // being E — can never shadow switching to Settings while a
        // different menu happens to be open).
        {L"Settings", L'E',
         {
             real(L"Edit settings", L'E', VK_F11, /*ctrl=*/true),
             // KEY-005: opens keys.conf (created with every default, commented out, on first use).
             {L"Edit key bindings", L'K', MenuItemKind::EditKeyBindings, HintAction{}},
             {L"Color mode", L'C', MenuItemKind::ToggleColorMode, HintAction{}},
             real(L"About", L'A', VK_F1),
         }},
    };
    return menus;
}

// -----------------------------------------------------------------------
// State: which menu (if any) is open, and which of its items is
// highlighted. `active == false` means the bar is fully closed — the row
// still draws (it's always visible), just with nothing highlighted.
struct MenuBarState {
    bool active = false;
    int openMenuIndex = 0;
    int highlightedItemIndex = 0;
    // IS-0023: true from the left press that opened (or switched) a menu until its release, so the release of the
    // opening click -- and any move or repeated press while the button is still down -- never closes the menu.
    bool mouseDownOnBar = false;
};

// Whether a raw virtual-key code can stand for a menu mnemonic at all. Only
// the letter ('A'..'Z') and digit ('0'..'9') virtual keys coincide with
// their own character; every other VK merely *aliases* some character when
// cast to wchar_t (VK_F7 == 0x76 == 'v', VK_F4 == 0x73 == 's', VK_NUMPAD1 ==
// 0x61 == 'a', ...), which would otherwise make Alt+F7 open the View menu
// instead of reaching its own Find files binding.
inline bool IsMnemonicKey(WORD vk) {
    return (vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9');
}

// Case-insensitive lookup of a top menu by its own mnemonic, or -1 if none
// match.
inline int FindTopMenuByMnemonic(const std::vector<TopMenu>& menus, wchar_t ch) {
    wchar_t lower = static_cast<wchar_t>(std::towlower(ch));
    for (size_t i = 0; i < menus.size(); ++i) {
        if (static_cast<wchar_t>(std::towlower(menus[i].mnemonic)) == lower) return static_cast<int>(i);
    }
    return -1;
}

// Case-insensitive lookup of an item within one open menu by its own
// mnemonic, or -1 if none match.
inline int FindItemByMnemonic(const TopMenu& menu, wchar_t ch) {
    wchar_t lower = static_cast<wchar_t>(std::towlower(ch));
    for (size_t i = 0; i < menu.items.size(); ++i) {
        if (static_cast<wchar_t>(std::towlower(menu.items[i].mnemonic)) == lower) return static_cast<int>(i);
    }
    return -1;
}

inline MenuBarState OpenMenu(int topIndex) {
    return MenuBarState{true, topIndex, 0};
}

inline MenuBarState CloseMenu() {
    return MenuBarState{};
}

// Wraps to the adjacent top menu (delta = +1/-1), keeping the bar open
// with the newly-open menu's first item highlighted — matches every
// standard menu bar's Left/Right behavior while a dropdown is showing.
inline MenuBarState MoveToAdjacentTopMenu(const MenuBarState& state, const std::vector<TopMenu>& menus, int delta) {
    if (menus.empty()) return state;
    int count = static_cast<int>(menus.size());
    int next = ((state.openMenuIndex + delta) % count + count) % count;
    return MenuBarState{true, next, 0};
}

// Wraps the highlighted item within the currently open menu (delta = +1/-1).
inline MenuBarState MoveHighlight(const MenuBarState& state, const std::vector<TopMenu>& menus, int delta) {
    if (!state.active || state.openMenuIndex < 0 || state.openMenuIndex >= static_cast<int>(menus.size())) {
        return state;
    }
    const auto& items = menus[static_cast<size_t>(state.openMenuIndex)].items;
    if (items.empty()) return state;
    int count = static_cast<int>(items.size());
    int next = ((state.highlightedItemIndex + delta) % count + count) % count;
    MenuBarState result = state;
    result.highlightedItemIndex = next;
    return result;
}

// -----------------------------------------------------------------------
// Layout: where each top entry sits on the bar's own row (row 0), and
// where an open dropdown's box/items sit beneath it. Console-free, exact
// analog of HintBar.h's LayOutHintBar/PlacedHintSegment.

struct PlacedMenuEntry {
    int menuIndex = 0;
    SHORT col = 0;       // display column of the entry's own "[" character
    SHORT width = 0;      // display width of "[X]" + label together
};

// Never wraps to a second row (unlike LayOutHintBar) — an entry that
// doesn't fit is simply omitted from this row's own layout instead, the
// same "extreme case, some things aren't mouse-reachable" fallback
// LayOutHintBar's own maxRows cap already established. This does happen in
// practice at the documented 60-column minimum (UI-006): the six
// "[X]label" entries need roughly 69 columns together, so one or two are
// dropped right at that floor. Unlike the hint bar's own dropped segments,
// this never costs keyboard reachability — main.cpp's Alt+<mnemonic>
// dispatch (FindTopMenuByMnemonic) matches independently of whatever this
// function actually laid out, so a menu missing from the row here is still
// one Alt+key press away regardless.
inline std::vector<PlacedMenuEntry> LayOutMenuBarRow(const std::vector<TopMenu>& menus, SHORT width) {
    std::vector<PlacedMenuEntry> placed;
    if (width <= 0) return placed;
    SHORT col = 1;
    for (size_t i = 0; i < menus.size(); ++i) {
        SHORT entryWidth = static_cast<SHORT>(3 + StringDisplayWidth(menus[i].label));  // "[X]" + label
        if (i > 0) col = static_cast<SHORT>(col + 2);                                    // "  " separator
        if (static_cast<int>(col) + entryWidth > width) break;
        placed.push_back(PlacedMenuEntry{static_cast<int>(i), col, entryWidth});
        col = static_cast<SHORT>(col + entryWidth);
    }
    return placed;
}

struct DropdownLayout {
    SHORT col = 0;
    SHORT row = 1;    // always row 1 — directly under the menu bar's own row 0
    SHORT width = 0;  // content width (no border), the longest item's own "[X]"+label plus one space of padding
    std::vector<SHORT> itemRows;
};

// Positions an open dropdown under its top entry (`topEntryCol`, from
// LayOutMenuBarRow), left-shifted just enough to stay fully on screen if
// the entry sits close to the right edge — the same clipping idea
// LayOutHintBar's row-wrap cap applies, adapted for a box instead of a
// wrapped line.
inline DropdownLayout LayOutOpenDropdown(const TopMenu& menu, SHORT topEntryCol, SHORT consoleWidth) {
    DropdownLayout layout;
    SHORT contentWidth = 0;
    for (const auto& item : menu.items) {
        SHORT itemWidth = static_cast<SHORT>(3 + StringDisplayWidth(item.label));  // "[X]" + label
        // KEY-005: two columns of gap, then the right-aligned shortcut.
        if (!item.keyText.empty()) itemWidth = static_cast<SHORT>(itemWidth + 2 + StringDisplayWidth(item.keyText));
        contentWidth = std::max(contentWidth, itemWidth);
    }
    layout.width = static_cast<SHORT>(contentWidth + 1);  // one column of right padding
    layout.col = topEntryCol;
    if (static_cast<int>(layout.col) + layout.width > consoleWidth) {
        layout.col = static_cast<SHORT>(std::max<int>(0, consoleWidth - layout.width));
    }
    for (size_t i = 0; i < menu.items.size(); ++i) {
        layout.itemRows.push_back(static_cast<SHORT>(layout.row + static_cast<SHORT>(i)));
    }
    return layout;
}

// -----------------------------------------------------------------------
// IS-0023: the mouse rules for the menu bar, pure so they are testable. main.cpp's HandleMouseEvent calls this first
// with the raw console mouse record (position, button state, event flags).
//
//   NotForMenu  the event is not the menu's business (menu closed and not a press on the bar): handle it as before.
//   Consumed    the menu dealt with it (state updated); nothing else may react to it.
//   Activate    run menus[menuIndex].items[itemIndex]; the menu has already been closed in `state`.
//
// A menu stays open through the release of the click that opened it, through mouse moves (which only highlight the item
// under the pointer) and through repeated presses while that button is still down. Releasing over a dropdown item after
// a press-drag activates it, like a native menu. Only a press elsewhere, a second press on the open entry, the wheel and
// a right-button press close it (as before).
enum class MenuMouseEffect { NotForMenu, Consumed, Activate };

struct MenuMouseResult {
    MenuMouseEffect effect = MenuMouseEffect::NotForMenu;
    int menuIndex = -1;
    int itemIndex = -1;
};

inline MenuMouseResult HandleMenuMouse(MenuBarState& state, const std::vector<TopMenu>& menus,
                                       const std::vector<PlacedMenuEntry>& barEntries, SHORT consoleWidth, SHORT x,
                                       SHORT y, DWORD buttonState, DWORD eventFlags) {
    MenuMouseResult result;
    const bool isMove = (eventFlags & MOUSE_MOVED) != 0;
    const bool isWheel = (eventFlags & (MOUSE_WHEELED | MOUSE_HWHEELED)) != 0;
    const bool plain = !isMove && !isWheel;  // a button change: flags 0 or DOUBLE_CLICK
    const bool leftDown = (buttonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
    const bool rightDown = (buttonState & RIGHTMOST_BUTTON_PRESSED) != 0;
    const bool leftPress = plain && leftDown;
    const bool rightPress = plain && !leftDown && rightDown;
    const bool release = plain && !leftDown && !rightDown;

    auto entryAt = [&]() -> const PlacedMenuEntry* {
        if (y != 0) return nullptr;
        for (const auto& entry : barEntries) {
            if (x >= entry.col && x < static_cast<SHORT>(entry.col + entry.width)) return &entry;
        }
        return nullptr;
    };
    auto consumed = [&]() {
        result.effect = MenuMouseEffect::Consumed;
        return result;
    };

    if (!state.active) {
        if (leftPress && y == 0) {
            if (const PlacedMenuEntry* entry = entryAt()) {
                state = OpenMenu(entry->menuIndex);
                state.mouseDownOnBar = true;
            }
            return consumed();  // a press on the bar is the bar's, even on its empty part
        }
        return result;  // NotForMenu
    }

    // The item (index into the open menu) under the pointer, or -1.
    int itemUnderPointer = -1;
    if (state.openMenuIndex >= 0 && state.openMenuIndex < static_cast<int>(menus.size())) {
        auto openEntry = std::find_if(barEntries.begin(), barEntries.end(),
                                      [&](const PlacedMenuEntry& e) { return e.menuIndex == state.openMenuIndex; });
        const SHORT col = openEntry != barEntries.end() ? openEntry->col : SHORT{0};
        const DropdownLayout dropdown =
            LayOutOpenDropdown(menus[static_cast<size_t>(state.openMenuIndex)], col, consoleWidth);
        for (size_t i = 0; i < dropdown.itemRows.size(); ++i) {
            if (y == dropdown.itemRows[i] && x >= dropdown.col && x < static_cast<SHORT>(dropdown.col + dropdown.width)) {
                itemUnderPointer = static_cast<int>(i);
                break;
            }
        }
    }
    auto activate = [&]() {
        result.effect = MenuMouseEffect::Activate;
        result.menuIndex = state.openMenuIndex;
        result.itemIndex = itemUnderPointer;
        state = CloseMenu();
        return result;
    };

    if (isMove) {
        if (!leftDown) state.mouseDownOnBar = false;  // the release was lost; do not stay stuck waiting for it
        if (itemUnderPointer >= 0) state.highlightedItemIndex = itemUnderPointer;
        return consumed();
    }
    if (leftPress) {
        if (state.mouseDownOnBar) return consumed();  // the opening button is still down: a repeat, not a new click
        if (y == 0) {
            const PlacedMenuEntry* entry = entryAt();
            if (entry && entry->menuIndex != state.openMenuIndex) {
                state = OpenMenu(entry->menuIndex);
                state.mouseDownOnBar = true;
            } else {
                state = CloseMenu();  // the open entry again (a toggle), or the empty part of the bar
            }
            return consumed();
        }
        if (itemUnderPointer >= 0) return activate();
        state = CloseMenu();
        return consumed();
    }
    if (release) {
        if (state.mouseDownOnBar) {
            state.mouseDownOnBar = false;
            if (itemUnderPointer >= 0) return activate();  // press, drag, release onto an item
            if (y != 0) state = CloseMenu();               // released away from the menu; on the bar it stays open
        }
        return consumed();
    }
    if (isWheel || rightPress) state = CloseMenu();
    return consumed();
}

}  // namespace mc
