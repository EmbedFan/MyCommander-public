# Feature Plan — FEA-0005

Source: `AIPrompt/additional_feature_requests.md`

> Add menubar at the top of the commander window
>
> It is a feature that makes this program more efficient.
> It is not a task now to put functionality behind the menu points that actions are not
> implemented yet.
> The main menubar entries should be:
> File Selection Commands Network View Settings
>
> File menu items: Modify Attribs, Pack Files, Unpack Package, Compare contents, Assign File to
> App, Used Disk Area, Group Rename, Generate Check Code
>
> Selection: Select group +, Unselect group -, Toggle Selection, Copy Selected Names to
> Clipboard, Compare Folders
>
> Commands: Find, Disk Label, Command Line
>
> Network: Network Connection, Share Folder, Unshare Folder, FTP connection, New FTP connection
>
> View: Full, List, Tree, Executable properties, Order by Name, Order by Size
>
> Settings: Edit settings, Color mode, About

## Summary

MyCommander has no top menu bar today — every action is reached through F-keys, `Ctrl`/`Alt`
combinations, or the bottom hint bar. This plan adds a persistent, always-visible menu-bar row at
the very top of the screen (`File Selection Commands Network View Settings`), opened with
`Alt+<mnemonic>`, navigated with arrow keys, and also fully mouse-clickable (matching UI-012's
established precedent) — without ever becoming the *only* way to reach anything (KEY-001).

The request itself is explicit that most listed items name features MyCommander doesn't have yet
(archive packing, FTP, disk-usage scanning, tree view, and so on) and that wiring those up is
**not** part of this ask — only the menu itself, with all of its listed entries present and
navigable. This plan's **Item mapping** (§3) below sorts every requested entry into either
**Real** (already exists under some other key today — the menu item just becomes a second way to
reach the exact same code path) or **Stub** (nothing to call yet — selecting it shows a plain
"Not yet implemented" message and does nothing else). No new file-operation/network/archive logic
is designed or written here; this plan is entirely about the menu UI and wiring what already
exists to it a second time.

## Current state

- `main.cpp`'s `DrawFrame` draws the panel frame's top border at row 0 (`console.FillRow(0, ...)`)
  and panel content starting at row 1 (`DrawPanel(..., /*y=*/1, ...)`) — there is no reserved row
  above the panels today, the mirror image of how the bottom hint bar/status/command-line rows are
  already reserved (IS-0007/its addendum).
- `Navigation.h`'s `HitTestPanel`'s own doc comment already says its row math "mirrors `DrawPanel`'s
  own row layout... kept in sync by hand... a future layout change needs the same edit here" — this
  plan is exactly that future layout change, this time adding a row at the *top* instead of the
  bottom.
- `Dialog.h`'s `ShowChoiceDialog`/`ShowMessage` and `Navigation.h`'s `ClickableRegion`/
  `MatchClickRegion` are the two pieces of existing, reusable machinery this plan leans on most:
  the former for a plain "not implemented yet" message (Stub items) and the latter for the
  dropdown's own mouse-click hit-testing (same pattern the bottom hint bar and dialog buttons
  already use, IS-0003).
- No existing keybinding uses a bare `Alt+<letter>` (only `Alt+F1`/`Alt+F2`/`Alt+F7`/`Alt+Left`/
  `Alt+Right` — function keys and arrow keys, not letter keys, so there is no collision to design
  around).

## Design

### 1. A pure, `Console`-free menu-structure module: `MenuBar.h`

Same shape as `HintBar.h`/`Navigation.h`'s existing precedent — the menu's static structure and
open/close/navigate state transitions are plain data and pure functions, independent of `Console`,
so they're fully unit-testable:

- `MenuAction` — an enum naming every real, already-existing action a menu item can trigger
  (`ToggleAttribute`, `SelectByMask`, `DeselectByMask`, `ToggleSelection`, `FindFiles`,
  `OpenShell`, `SortByName`, `SortBySize`, `EditConfig`, `ToggleColorTheme`, `ShowAbout`) plus one
  `NotImplemented` value every Stub item uses.
- `MenuItem { std::wstring label; wchar_t mnemonic; MenuAction action; }` — `mnemonic` is the
  underlined letter within the dropdown (not the top-level `Alt+` letter, a separate, second
  mnemonic layer standard menu bars have — e.g. inside `File`, `Modify Attribs` underlines `M`).
- `TopMenu { std::wstring label; wchar_t mnemonic; std::vector<MenuItem> items; }` — six of these
  (§3's exact item lists), built once by a `BuildMenus()` function mirroring `HintBar.h`'s
  `BuildHintSegments()`.
- `MenuBarState { bool active; int openMenuIndex; int highlightedItemIndex; }` plus pure transition
  functions (`OpenMenu`, `CloseMenu`, `MoveToAdjacentTopMenu`, `MoveHighlight`, ...) — the same
  "state in, key in, state out" shape `Navigation.h`'s `ClassifyNavigationKey` already established,
  so `main.cpp`'s key-dispatch switch stays a thin caller rather than owning the state-machine
  logic itself.
- Layout/click-region helpers analogous to `HintBar.h`'s `LayOutHintBar`: `LayOutMenuBarRow` (top
  entries' column positions across the width, so the underlined-mnemonic label wraps or truncates
  the same defensive way the hint bar already does on a narrow terminal) and
  `LayOutOpenDropdown` (an open dropdown's own row/column box, positioned under its top entry,
  clipped to stay on-screen if the top entry is near the right edge).

### 2. Reserving the new top row

`DrawFrame`'s reserved-row discipline (IS-0007/its addendum: `hintRows`/`hasStatusLine`, computed
fresh every draw and shared with `ComputeVisibleRows` so mouse/paging math never drifts from what's
drawn) gains a mirror-image top offset: `topOffset = 1` (the menu bar's own single row) added to
every row calculation that currently assumes the frame starts at row 0 — `DrawFrame`'s own
`console.FillRow(0, ...)` border calls move to `topOffset`, `DrawPanel`'s `y` argument moves from
`1` to `topOffset + 1`, `contentHeight` shrinks by `topOffset`, and `ComputeVisibleRows`/
`HitTestPanel` (`Navigation.h`) both need the identical `topOffset` added to their own row math —
**this is the single biggest risk in this plan**, structurally identical to IS-0007's own
top-flagged "`ComputeVisibleRows`/`DrawFrame` must agree exactly" risk, just on the opposite edge
of the screen. Grepping every literal `y = 1` / row-0-relative constant in `main.cpp`/
`Navigation.h` before considering this done is as essential here as it was there.

The menu bar row itself is *always* drawn (like the bottom hint bar), whether or not it's
currently "active" (a dropdown open) — only its highlight state changes. This keeps `topOffset`
a constant `1`, not a conditional value the way `hasStatusLine` is conditional — simpler, and
avoids yet another "is the reserved space there or not" case to keep in sync across three
functions.

### 3. Item mapping — Real vs. Stub

**Real** items call the *exact* internal function/handler an existing key already calls (no new
business logic, only a second caller) — listed here as "→ existing key" for traceability:

| Menu | Item | Maps to |
|---|---|---|
| File | Modify Attribs | → `Ctrl+A` (attribute toggle dialog) |
| Selection | Select group + | → `Numpad +` (mask select) |
| Selection | Unselect group - | → `Numpad -` (mask deselect) |
| Selection | Toggle Selection | → `Insert` (toggle cursor entry, no move) |
| Commands | Find | → `Alt+F7` (recursive search) |
| Commands | Command Line | → `F9` (open a separate interactive shell) |
| View | Order by Name | → `Ctrl+F3` (sort by name) |
| View | Order by Size | → `Ctrl+F5` (sort by size) |
| Settings | Edit settings | → `Ctrl+F11` (open config file externally) |
| Settings | About | → `F1` (shortcut reference, opens on its About section, KEY-004) |
| Settings | Color mode | → new: toggles `Config::colorTheme` between `default`/`highcontrast`,
saves, and calls the same `ApplyConfigLive` IS-0006 already uses for a live config-file save — the
one genuinely new (but trivial, config-plumbing-only) piece of logic in this whole plan, not a new
feature so much as a shortcut for an edit any user could already make by hand in the config file. |

**Stub** (no existing feature behind them — selecting one shows a plain `ShowMessage("Not yet
implemented.")` dialog, exactly like any other informational dialog, and does nothing else):
`Pack Files`, `Unpack Package`, `Compare contents`, `Assign File to App`, `Used Disk Area`,
`Group Rename`, `Generate Check Code` (File); `Copy Selected Names to Clipboard`, `Compare
Folders` (Selection); `Disk Label` (Commands); every `Network` item (`Network Connection`,
`Share Folder`, `Unshare Folder`, `FTP connection`, `New FTP connection`); `Full`, `List`, `Tree`,
`Executable properties` (View — `Order by Name`/`Size` are Real, the rest of `View` is Stub, since
MyCommander only has the one listing mode today).

This mapping is a judgment call worth a quick confirmation pass before implementation — in
particular whether `Color mode` should be Real (as designed above) or also just a Stub pointing at
`Ctrl+F11`'s existing config editing.

### 4. Mnemonics

Top-level: `File`(**F**), `Selection`(**S**), `Commands`(**C**), `Network`(**N**), `View`(**V**),
`Settings`(**T**, since **S** is already `Selection`'s — the standard Windows-menu convention of
falling back to a distinguishing interior letter, "seT­tings", when the initial letter collides).
Flagged as a judgment call, easy to change before implementation if a different letter reads
better. Within each dropdown, every item's own mnemonic is its own first letter where that stays
unique within the dropdown (checked per-menu, not globally — e.g. `File`'s `Modify Attribs` (M)
and `Pack Files` (P) don't collide with `Selection`'s own items, since only one dropdown is ever
open at a time).

### 5. Keyboard interaction

- `Alt+<top mnemonic>` — opens that menu (closing any other open one first) if the bar isn't
  active yet; if a menu is already open, switches directly to the newly-named one instead of
  requiring `Left`/`Right` first.
- While a menu is open: `Left`/`Right` moves to the adjacent top menu (wrapping around);
  `Up`/`Down` moves the highlighted item within the open dropdown (wrapping around); `Enter` or
  the highlighted item's own mnemonic letter activates it and closes the bar; `Esc` closes the bar
  without activating anything, returning focus to normal panel navigation exactly where it was.
- The menu bar never intercepts a key it doesn't recognize while active — matching every other
  modal/semi-modal surface in this app (`Dialog.h`'s choice dialogs), an unrecognized key while a
  menu is open is simply ignored, not passed through to panel navigation (which would be
  surprising — e.g. `Down` moving the panel cursor underneath an open dropdown).
- KEY-001 stays intact by construction: every Real item's underlying action already has its own
  direct key, so the menu never becomes the *only* way to reach anything — it's purely an
  additional, discoverable path (the request's own "efficiency" framing).

### 6. Mouse interaction (UI-012/IS-0003, optional, never required)

Clicking a top-level entry opens its dropdown (or closes it, on a second click on the same entry —
the common toggle behavior); clicking a dropdown item activates it; clicking anywhere outside an
open dropdown closes it without activating anything (the standard "click away to dismiss"
convention). Implemented via `Navigation.h`'s existing `ClickableRegion`/`MatchClickRegion` for
both the top bar's own entries and the open dropdown's item rows — two flat lists of regions,
recomputed from `LayOutMenuBarRow`/`LayOutOpenDropdown` (§1) the same way `MatchHintBarClick`
already recomputes its own regions from `LayOutHintBar` on every check (IS-0007).

## Files touched (once implemented — none touched by this plan document itself)

- `Source/MyCommander/MenuBar.h` — new: `MenuAction`, `MenuItem`, `TopMenu`, `BuildMenus`,
  `MenuBarState` and its transition functions, `LayOutMenuBarRow`, `LayOutOpenDropdown` (§1).
- `Source/MyCommander/main.cpp` — `DrawFrame`'s top-row reservation and the new dropdown-drawing
  call (§2); `ComputeVisibleRows`/`Navigation.h`'s `HitTestPanel` gain the same `topOffset` (§2);
  `wmain`'s key-dispatch switch gains the `Alt+<mnemonic>`/menu-open-state branch (§5);
  `HandleMouseEvent` gains the menu bar's click handling (§6); a small `ExecuteMenuAction`
  function maps each `MenuAction` to the exact existing handler its equivalent key already calls
  (§3) plus `ShowMessage("Not yet implemented.")` for `MenuAction::NotImplemented`; `Config`/
  `ApplyConfigLive` gain the `Color mode` toggle's few lines (§3's one new piece of logic).
- `Source/MyCommanderTests/MenuBarTests.cpp` — new, for `MenuBar.h`'s pure functions (see **Test
  plan**).
- `Source/MyCommander/Help.h`/`Help.cpp` — a new "Menu" row/section in the `F1` shortcut reference
  documenting `Alt+<mnemonic>` (KEY-002/003's existing mirroring discipline with README applies
  here too).
- `README.md`/`USERGUIDE.md` — a new section describing the menu bar, its keybinding, and (briefly)
  which entries are live today vs. reserved for a not-yet-built feature — important so a user who
  clicks `FTP connection` and sees "Not yet implemented" understands that's expected, not a bug.
- `AIPrompt/my-commander.md` — likely a new requirement-ID prefix (`MENU-` is proposed, matching
  `INST-`'s own precedent from FEA-0006) for the menu bar itself; exact IDs/wording are an
  implementation-time decision, not fixed by this plan, matching FEA-0006's own plan's approach to
  the same question.

## Test plan

`MenuBar.h`'s structure/state-transition/layout functions are pure (`HintBar.h`/`Navigation.h`'s
own established precedent) — fully unit-testable without a `Console`:

- `BuildMenus()` returns exactly six top menus in the requested order, each with every requested
  item present (a direct regression guard against a typo/omission from the request's own list).
- State transitions: `Alt+F` from a closed state opens `File` at index 0 (no item highlighted, or
  the first one — a UX choice to confirm); `Alt+S` while `File` is open switches straight to
  `Selection` rather than requiring `Esc` first; `Left`/`Right` wrap around both ends of the six
  top menus; `Up`/`Down` wrap around both ends of the open dropdown's own item list; `Esc` returns
  to a fully closed state.
- `LayOutMenuBarRow`/`LayOutOpenDropdown`: every top entry's mnemonic-underline column position is
  correct at a representative width; an open dropdown clips/repositions rather than running off
  the right edge when its top entry is near the console's right border; no label ever overflows a
  dropdown's own column budget.
- `MatchClickRegion` reuse: a click on a top entry and a click on a dropdown item each resolve to
  the right `MenuAction` (or `-1`/no match) via the same regions `LayOutMenuBarRow`/
  `LayOutOpenDropdown` produce.

Not automatable, consistent with every prior `main.cpp`-glue precedent this session (`HandleMouseEvent`,
`wmain`'s key switch, `ShowShortcutReference`'s own mouse-wheel branch, IS-0008): `wmain`'s new
`Alt+<mnemonic>` dispatch branch and `HandleMouseEvent`'s new click handling get no dedicated
`TEST_CASE`, verified live instead once implemented.

## Risk / scope notes

- **The top-row reservation (§2) is this plan's IS-0007-equivalent risk** — every row-relative
  constant in `DrawFrame`/`ComputeVisibleRows`/`HitTestPanel` needs the same `topOffset` added,
  or panel mouse clicks/paging will target the wrong row by exactly one. Called out explicitly so
  a future reviewer checks this first, the same way IS-0007's own plan called out its bottom-row
  equivalent.
- **Most requested items are, and will stay, Stubs** until their underlying feature actually gets
  built (archive packing = `ARC-*`, FTP = `EXT-004`/`005`, disk-usage scanning, tree view, and so
  on — all separately `Not Started`/`LATER` in the spec today). This plan does not change any of
  those dispositions; it only gives their menu entries somewhere to live, exactly as the request
  asked. A Stub item showing "Not yet implemented" is the intended, permanent behavior until a
  *separate* feature request builds the real thing — not a placeholder this plan itself needs to
  finish.
- **`Color mode`'s Real mapping is the one genuinely new (if trivial) piece of logic** in an
  otherwise "wire existing things to a second entry point" plan — flagged in §3 as worth a quick
  confirmation, since a Stub-only, fully-mechanical version of this plan is also a defensible
  scope cut if a hand-edited config file is considered good enough for this already.
- **No requirement ID exists yet for the menu bar itself** — a new `MENU-` prefix is proposed
  (§ **Files touched**), matching `INST-`'s own precedent, but the exact IDs are left to
  implementation time rather than fixed here.
- **Alt-key reliability across terminal hosts is unverified.** This session already hit two
  distinct terminal-host input quirks (`F11` reserved for fullscreen by most hosts, `Shift`-held
  mouse clicks silently swallowed by the classic console host) — `Alt+<letter>` is a different key
  combination from both of those and is not currently known to be affected, but hasn't been
  confirmed clean either; worth an explicit early smoke test once implemented, before investing
  further polish, in case a third host-specific surprise shows up here too.

## Status

**Implemented.** Both flagged judgment calls were confirmed before implementation started:
`Color mode` — a real toggle — and `Settings`' mnemonic — `T`. `MENU-001` through `MENU-004` were
added to `AIPrompt/my-commander.md`'s new `## 24. Menu Bar Requirements` section, all
`Implemented`.

A handful of implementation-time refinements, each a simplification of the original design rather
than a scope change:

- **No separate `MenuAction` enum/dispatch table (§1).** Every Real item instead carries a
  `HintAction` (`HintBar.h`'s own struct) and is executed via `SimulateKeyPress` — the exact
  mechanism IS-0003's hint-bar clicks already use to replay a key press through `wmain`'s existing
  dispatch switch. This is strictly less new code than the plan's original per-action enum would
  have been, and reuses a mechanism the plan didn't originally connect this to. `MenuItemKind`
  (`SimulatedKey`/`ToggleColorMode`/`Stub`) is the only discriminator actually needed — three
  cases, not one per action.
- **Item labels aren't trimmed around the mnemonic (§4).** Every entry renders as
  `"[" + mnemonic + "]" + label` with the label's *full* word — `"[T]Settings"`, not
  `"Se[t]tings"` — rather than `DialogOption`'s convention of dropping the mnemonic's own letter
  out of the label. Simpler, and the only convention that reads unambiguously for `Settings`
  (whose mnemonic isn't its first letter) without a special case.
- **No new `Theme.h` colors.** The open menu's own top entry and its highlighted item reuse
  `kAttrHeaderActive` (the same blue "this is focused" highlight the active panel's header already
  uses); everything else on the bar/dropdown reuses `kAttrStatusBar` (matching the bottom hint
  bar's own look). Avoids adding a new palette entry — and the matching `ColorContrastTests.cpp`
  audit — for this alone.
- **The bar does occasionally drop an entry at the exact 60-column minimum, corrected from the
  plan's original claim.** The six `"[X]label"` entries need roughly 69 columns together — more
  than the plan estimated (it didn't account for the bracket notation's own overhead) — so at
  UI-006's documented floor, one or two entries (typically `Settings`) aren't clickable. This never
  costs keyboard reachability: `Alt+<mnemonic>` is matched independently of whatever
  `LayOutMenuBarRow` actually laid out, so KEY-001 holds regardless. `MenuBar.h`'s own comment and
  `MenuBarTests.cpp` were both corrected to state this accurately rather than the original
  (inaccurate) "never wraps in practice" claim.
- **§2's row-reservation risk was real and handled exactly as flagged.** `DrawFrame` gained a
  `kMenuBarRows = 1` constant; `ComputeVisibleRows` and `Navigation.h`'s `HitTestPanel` (which
  gained an explicit `topOffset` parameter) both had to shift by the same amount. Verified by
  cross-checking every row constant touched, then confirmed by the full `HandleMouseEvent`/
  `wmain` mouse-click paths compiling and passing their existing tests unchanged in shape (only
  the new `topOffset` argument added at each call site).
- **Every Real item's key mapping was cross-checked against `wmain`'s actual dispatch switch**,
  not just against the hint bar's own documentation, to catch any drift between the two (e.g.
  confirming `VK_F1` with no modifiers reaches `ShowShortcutReference`, not the `Alt+F1` drive-
  picker branch) — all twelve matched exactly.
- **Live interactive verification wasn't possible in this environment** — the same ConPTY/no-HWND
  limitation noted during the FEA-0006 installer work applies to `MyCommander.exe` itself, so
  window-handle-based input injection isn't reachable here either. Verified instead via: 34 new
  automated tests (`MenuBarTests.cpp`'s 26 covering `BuildMenus`/mnemonic uniqueness/state
  transitions/layout, `ConfigTests.cpp`'s 5 covering `UpdateConfigValue`, plus 1 new
  `HitTestPanel` regression case for `topOffset`), a full solution build (Debug and Release, all
  three projects), a basic launch smoke test (confirmed the app starts and runs without crashing
  with the new top row reserved), and the exhaustive key-mapping cross-check above.
- Full test suite: 374 cases, 11,659 checks, 0 failed (up from 341/11,525 before this feature).

No deviation from scope: `Network`'s five items, archive packing/unpacking, tree view, and every
other feature-less item remain Stubs exactly as designed; no all-users install, no `--silent`
mode, no MSI tooling were touched (none of those were ever part of this plan to begin with).

## Addendum — Settings' mnemonic changed from T to E

A later polish request changed `Settings`' top-level mnemonic from `T` ("seTtings") to `E`
(matching its own "Edit settings" item, its first and most-used one). This introduced a real
collision that didn't exist before: `E` is already an item mnemonic within two *other* menus
(File's "Generate Check Code", View's "Executable properties") — while either of those menus is
open, pressing `Alt+E` previously would have matched the open menu's own item (via
`FindItemByMnemonic`, checked first) rather than switching to Settings, silently shadowing the
intended "switch menus" gesture.

Fixed by reordering `wmain`'s own menu key-dispatch: `Alt+<letter>` now always tries a top-menu
switch (`FindTopMenuByMnemonic`) *before* the open menu's own item mnemonics; a bare letter (no
Alt) still means "choose this item in the currently open menu," unchanged. This also fixes the
identical latent collision that already existed between `Commands`' own top-level mnemonic (`C`)
and `File`'s "Compare contents" item (also `C`) — not something this polish request asked about,
but the same class of bug, now fixed by the same reordering rather than patched around per-letter.

`MenuBar.h`'s `BuildMenus()` comment for the `Settings` entry documents why `E` is safe despite
the collision. `MenuBarTests.cpp` updated (`BuildMenus_SettingsMnemonicIsE_NotS_FEA0005`, plus a
new `FindTopMenuByMnemonic_SettingsE_ResolvesEvenThoughOtherMenusHaveAnEItemToo_FEA0005`
regression guard). Every doc mentioning `Alt+T` for Settings (`Help.cpp`, `README.md`,
`USERGUIDE.md`) updated to `Alt+E`. Full test suite re-run: 375 cases, 11,660 checks, 0 failed.
