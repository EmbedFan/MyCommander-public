# Implementation Plan — NAV-016 (swap the two panels)

Source: a new requirement, proposed here. Not in `AIPrompt/my-commander.md`, `additional_feature_requests.md` or any
earlier plan.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| NAV-016 (proposed) | The user shall be able to exchange the left and right panels — location, history, sort order, filter, selection and cursor — with one keystroke. | P3 | IN-MVP | Not Started |

## Summary

Total Commander calls it "Swap panels" (`Ctrl+U`). With a dual-pane manager the two panels are often
"source" and "target" for a run of operations; swapping lets you reverse the direction without retyping either path.

- **Key:** `Ctrl+U`, also **Commands → Swap panels**.
- **What moves:** everything that belongs to a panel's listing, not the side it sits on: current folder, back/forward
  history, sort key and direction, quick filter, hidden/system toggle, Full/Brief view, selection, cursor and scroll
  position.
- **What stays:** which side is active. After the swap the same side has focus, now showing the other folder.
- **Refused** (with a one-line message) while either panel shows plugin panel content, a tree view or a provider
  (archive / server): those are tied to a side by the plugin manager and the provider hooks.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| `Panel` holds all listing state in plain members | `Panel.h` | A member-wise exchange; no new state |
| `IPanelContent` / `SetContent` and EXT-002 provider mode | `Panel.h`, `PluginManager` | The "refuse" test: both panels must be plain local |
| Per-side watches re-arm when the path changes | `main.cpp` `PollAutoRefresh` (LIST-007) | Nothing to change: the loop compares each side's path |
| Session writes the two paths at clean exit | `Session.cpp` | Nothing to change: it reads the panels as they are |
| Key table with a test against the built-in inventory | `KeyMap.cpp` `Commands()`, `PluginManagerLogic.cpp` | One new row, one new inventory entry |
| `Ctrl+U` is free | `KeyMap.cpp` (checked: no `K('U', C)` today) | The default key |

## Design

### D1 — `Panel::SwapListingWith(Panel& other)`

- Exchanges every listing member of the two panels: `path_`, `entries_`, `cursor_`, `topIndex_`, `filter_`,
  `sortKey_`, `sortDescending_`, `showHiddenAndSystem_`, `briefView_`, history stacks, `volumeInfo_`,
  `locationInaccessible_`, the loading state (`loading_`, `refreshIterator_`, `refreshCursorName_`,
  `pendingSelectName_`) and `lastVisibleRows_`.
- Does **not** exchange the preferences that are the same for both panels (`groupDirectoriesFirst_`,
  `incrementalLoading_`) or `content_`/`contentTitle_` (both must be empty).
- `refreshStarts_` is exchanged too, so LIST-007's "already covered by a listing" bookkeeping stays consistent; the loop
  re-arms the watches because each side's path changed.
- Returns `false` and changes nothing when either panel has content or is in provider mode.

### D2 — The command (`main.cpp`)

- `DoSwapPanels`: refuse with `RefuseIfBusyWithJob`-style text when a background job uses either folder? **No:** a
  swap changes no files, so jobs are unaffected; it only refuses for content/provider panels.
- After the swap, redraw. A panel that was mid-incremental-load continues loading on its new side.
- Single-panel layout (UI-013): the swap still works; the hidden panel is the one that moves.

### D3 — Keys, menu, help

- New `CommandId::SwapPanels` / `swap-panels` in `Commands()` with `K('U', C)`; `BuiltinShortcutInventory` and the
  inventory test updated (the repo has a test that compares them).
- **Commands → Swap panels** menu item; hint-bar and F1 rows; README and USERGUIDE tables.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Panel.{h,cpp}` | `SwapListingWith` |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp` | `SwapPanels` command, `Ctrl+U` |
| `Source/MyCommander/MenuBar.h`, `HintBar.h`, `Help.cpp`, `main.cpp` | Menu item, hint, help row, `DoSwapPanels` |
| `Source/MyCommanderTests/PanelTests.cpp`, `KeyMapTests.cpp` | Tests below |
| `README.md`, `USERGUIDE.md`, `CLAUDE.md`, spec (new NAV-016 row), changelog, `Version.h` | Docs |

## Tests

- **`Panel::SwapListingWith`:**
  - two panels on two temp folders: after the swap each shows the other's entries, cursor name, selection and sort;
  - the quick filter, hidden toggle and Full/Brief preference travel with the listing;
  - back/forward history travels (back from the swapped panel goes to its own previous folder);
  - swapping twice restores the original state;
  - returns `false` and leaves both unchanged when a panel has content set;
  - a panel in the middle of an incremental load keeps loading correctly after the swap.
- **KeyMap:** `Ctrl+U` resolves to `SwapPanels`; the command is in the built-in inventory.
- **Manual:** two different folders, a selection on each, swap; Esc-cancel nothing; swap with a ZIP open (refused with a
  message); swap in single-panel layout; both panels on the same folder.

## Phasing

One phase: the method and its tests, then the command, key, menu and docs.

## Risks

- **A forgotten member:** a new `Panel` member added later and not swapped. Mitigation: the test enumerates observable
  state, and the method's comment lists the members; consider implementing the exchange as `std::swap` of a small
  `ListingState` struct that holds exactly these members, so a new listing member has one place to go.
- **Active-side expectation:** some users expect focus to follow the panel. Recommended default is "the side keeps
  focus" (Total Commander's behaviour); easy to revisit.

## Open questions

1. **Should focus follow the listing?** Recommended: no, the side keeps focus.
2. **Also offer "target = source" (`Alt+Shift+O` in some managers: show the active folder in the other panel)?**
   Recommended: separate small request; not part of this plan.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.34.0) with the recommended answers: the side keeps focus; "target = source" is not part of this change.

Differences from the plan:

- **No new `ListingState` struct:** `SwapListingWith` exchanges the members one by one (a comment lists the rule); the
  tests enumerate selection, cursor, sort, filter, Full/Brief, hidden toggle and back history.
- **Menu:** the item is `Commands > Swap panels` (mnemonic `W`), a simulated `Ctrl+U`; it therefore also reaches a
  panel's plugin content first, like every other key, and the refusal message covers the rest.
- **No `HintBar` row:** `Ctrl+U` is not one of the hint bar's per-context entries.
