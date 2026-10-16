# Fix Plan — IS-0023 (a clicked main menu closes at once, or after a few seconds)

Source: `AIPrompt/issue_fix_requests.md`:

> When I click on a main menu item, the popup popping down, but
> 1) when I release the left mouse button the menu is closed suddenly
> 2) when I keep press the left mouse button the menu after some seconds closed also.

**Expected:** a click on a top menu entry opens its dropdown and it **stays open** until the user chooses an item,
clicks elsewhere, presses `Esc`, or clicks the same entry again. Holding the button down must not close it either.

## Root cause

All of it is in `HandleMouseEvent` (`main.cpp`, "FEA-0005" block at the top). The block reacts to **every** mouse event
while a menu is open, not only to presses:

```cpp
bool isLeftPress = mouse.dwEventFlags == 0 && (mouse.dwButtonState & FROM_LEFT_1ST_BUTTON_PRESSED) != 0;
if (isLeftPress || menuState.active) {
    ...
    if (menuState.active) {
        if (isLeftPress) { ...item hit test... }
        menuState = CloseMenu();          // <- any other event closes the menu
        if (isLeftPress) return;
    }
}
```

1. **The button release closes the menu.** A click is two console events: the press (`dwEventFlags == 0`, left bit set) which
   opens the dropdown, then the release (`dwEventFlags == 0`, left bit clear). The release is not a "left press", the menu is
   active, so it falls to `menuState = CloseMenu()`. This is symptom 1, at once, on every click.
2. **Mouse motion closes it.** Holding the button and moving by even one cell (hand tremor, or the terminal's own mouse
   reports) delivers `MOUSE_MOVED` events, and the same fall-through closes the menu. That is symptom 2. Two more event kinds
   hit the same line and have the same effect: a second quick press arrives with `DOUBLE_CLICK` set (not a "left press"), and
   a held button can be re-reported as a fresh press.
   *I can't reproduce the exact "after some seconds" trigger from code alone; the fix below makes the menu independent of
   any event that happens while the button that opened it is still down, whatever the terminal sends.*
3. There is no memory of "this press opened the menu", so the code cannot tell the release of the opening click from a click
   away.

No keyboard path is affected (`Alt+letter`, arrows, `Esc` work, as the key handling is separate).

## Fix

### F1 — Remember the opening press (`MenuBarState`, `MenuBar.h`)

Add `bool mouseDownOnBar = false;` — true from a left press that opened or switched a menu until the matching release.

### F2 — The mouse rules become a pure function (`MenuBar.h`, tested)

```cpp
enum class MenuMouseEffect { NotForMenu, Consumed, Activate };   // Activate: run item `itemIndex`, then the menu closes
struct MenuMouseResult { MenuMouseEffect effect = MenuMouseEffect::NotForMenu; int itemIndex = -1; };

MenuMouseResult HandleMenuMouse(MenuBarState& state, const std::vector<TopMenu>& menus,
                                const std::vector<PlacedMenuEntry>& barEntries, SHORT consoleWidth,
                                SHORT x, SHORT y, DWORD buttonState, DWORD eventFlags);
```

`main.cpp` calls it first; on `Activate` it runs `ExecuteMenuItem(openMenu.items[itemIndex])`; `Consumed` returns;
`NotForMenu` falls through to the existing panel handling. The rules, by event:

| Event | Menu closed | Menu open |
|---|---|---|
| Left press (also with `DOUBLE_CLICK`) on a top entry | Open it, `mouseDownOnBar = true`, consumed | Same entry: close (toggle). Another entry: switch, `mouseDownOnBar = true`. Consumed |
| Left press on a dropdown item | — | Activate the item and close. Consumed |
| Left press anywhere else | not for the menu | Close, consumed (as today) |
| **Left press while `mouseDownOnBar`** (a repeated press) | — | **Ignored**, consumed |
| **Left release**, `mouseDownOnBar` set | — | Clear the flag. Over a dropdown item: **activate it** (press, drag, release works like a native menu). On the top entry row: stay open. Anywhere else: close |
| Left release, flag not set | not for the menu | **Ignored** (no longer closes), consumed |
| **Mouse move** (`MOUSE_MOVED`) | not for the menu | **Never closes.** Over a dropdown item: highlight it (`highlightedItemIndex`); elsewhere unchanged. Consumed |
| Wheel, right-button press | not for the menu | Close, consumed (as today: clicking or scrolling away dismisses) |

Hovering to highlight is the only addition to behaviour the issue asks for; it follows from "a move must not close it" and is
what makes the drag-and-release path possible. Without a button down a move only highlights.

### F3 — `main.cpp`

Replace the FEA-0005 block (about 55 lines) with the call and its three outcomes. No change to panel/hint-bar mouse code.
`menuState` already lives in the main loop and is passed by reference, so the new flag needs no plumbing.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/MenuBar.h` | `MenuBarState::mouseDownOnBar`, `MenuMouseEffect`, `MenuMouseResult`, `HandleMenuMouse` |
| `Source/MyCommander/main.cpp` | `HandleMouseEvent`'s menu block calls it |
| `Source/MyCommanderTests/MenuBarTests.cpp` | Tests below |
| `AIPrompt/my-commander.md`, `README.md`/`USERGUIDE.md` (one sentence each), `AIPrompt/ManualTests.md`, `Version.h`, changelog | Spec history note (IS-0023, UI-012/MENU rows unchanged), manual test, patch bump 2.14.0 → 2.14.1, spec 1.52.0 → 1.52.1 |

## Tests (`MenuBarTests.cpp`, pure, with the real `BuildMenus()` layout)

Event helpers build `(x, y, buttonState, eventFlags)`; the default menus are laid out for a width of 120.

- **Click opens and release keeps it open:** press on the File entry opens it (`active`, `openMenuIndex` right, flag set);
  the release on the same cell leaves it `active`, flag cleared. *(the regression for symptom 1)*
- **Moves never close it:** a hundred `MOUSE_MOVED` events — over the bar, over the dropdown, outside everything, with and
  without the button down — leave `active` true. *(symptom 2)*
- **A held button re-reported as a press does not close or toggle it** while `mouseDownOnBar`.
- **Hover highlights:** a move over the third item sets `highlightedItemIndex == 2`; a move outside leaves it.
- **Press on an item activates:** returns `Activate` with the right index and the item order of the dropdown; the caller
  closes (state helper test: the function leaves `active` for the caller to `CloseMenu()` — or closes it itself; decided
  while implementing and tested either way).
- **Press-drag-release onto an item activates it**; a release over the bar row keeps the menu open; a release elsewhere
  closes it.
- **Click away and toggle:** a press outside closes and is consumed; a second press on the open entry closes it; a press on
  another entry switches menus.
- **Wheel and right-button press close it.**
- **Menu closed:** presses off the bar, moves and releases return `NotForMenu` untouched.
- **A double-click press** (`DOUBLE_CLICK` flag) on the bar behaves like a press (toggles), not like "other activity".
- **A clipped bar** (narrow console, entries omitted) hit-tests only the entries `LayOutMenuBarRow` placed.

## Out of scope / notes

- Keyboard menu handling is unchanged, as is the plugin menu merge (`gMergedMenus` is what the function receives).
- Hover does not switch the open menu to another top entry (a press does); that Windows habit can be added later if wanted.
- Right-click selection (IS-0004) and drag-and-drop (IS-0003) code is untouched: a press that is not on the menu still
  falls through to them.
- If the problem persists after the fix, the next step is to log each mouse event (flags and button state) to the debug log
  for a held click, to see what the terminal really sends.

## Open questions

1. **Press, drag and release onto an item:** activate it (like a native Windows menu — recommended), or only keep the menu open?
2. **Hover highlight:** OK to highlight the item under the pointer while a menu is open (recommended), or leave highlighting to the keyboard?
3. **Version:** a patch bump (2.14.1) and spec 1.52.1 (no requirement row changes)?

## Status

Planned 2026-10-16. **Implemented 2026-10-16** (spec 1.52.1, version 2.14.1) with the three answers as recommended: press-drag-release activates an item, the item under the pointer is highlighted, patch bump.

Differences from the plan:

- `HandleMenuMouse` closes the menu itself when it returns `Activate` and reports the menu and item index in the result, so `main.cpp` copies the item and runs it.
- A move with no button down clears the "opening press is held" flag, so a lost release can never leave the menu ignoring clicks.
- A press on the empty part of the bar with the menu closed is still consumed (as before).
- Tests: 12 new cases in `MenuBarTests.cpp`.

**Not verified here:** the real mouse behaviour in Windows Terminal and the console window (manual test MT-070), including whether the "after some seconds" case is fully covered.
