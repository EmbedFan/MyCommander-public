# Fix Plan — IS-0004

Source: `AIPrompt/issue_fix_requests.md`

> I nedd the ability, user can select files using mouse. (one, or more, using shift, right mouse
> click/drag)
> - when I select an item by right mouse click, and another also but any SHIFT is pressed select
>   all items in the list between the first and second selected item.

**Addendum (2026-09-25):** the issue was extended with the bullet above after the first three
gestures below were already implemented — see [Section 4](#4-ctrlright-click-select-a-range-between-two-right-clicked-items),
added by this revision of the plan, and the `## Status` section for exactly what is/isn't done.

**Second addendum (2026-09-25, reported after Section 4 shipped): Shift doesn't work on classic
console host — switched to Ctrl.** The user reported that Shift+right-click "is not sensed by the
software at all," with no popup/menu appearing either. Follow-up confirmed: Shift+left-click
(Section 3, already-shipped) shows the identical symptom, and the user is running the classic
console host (`conhost.exe` via `cmd.exe`/PowerShell's own window), not Windows Terminal. Since
*both* buttons are affected identically and nothing visibly intercepts the click (no menu flash),
the conclusion is that classic console host silently swallows a Shift-held mouse click before it
ever reaches the app's `ReadConsoleInputW` queue — this can't be fixed from inside the app via any
`SetConsoleMode` flag. Per the user's explicit choice, **every mouse gesture in this plan that
used Shift as its modifier now uses Ctrl instead** (Sections 3 and 4 below, and every reference
throughout this document) — Ctrl isn't reserved the same way. IS-0003's *unrelated* use of Shift
(held at the moment of a copy/move drag's *drop*, to mean "move instead of copy") is deliberately
left alone: that's a different moment (a button *release* during an already-active drag, not a
fresh click) and hasn't been reported broken — see that section's own note for why it's out of
scope here.

## Summary

Mouse-driven file **selection** (marking entries with `*`, as Insert/Ctrl+A/Ctrl+NumpadMinus/
`*` already do from the keyboard — SEL-001/002), which today's mouse support does not offer at
all: `HandleMouseEvent` (added by UI-012, extended by IS-0003) only ever reacts to the *left*
button, and a left click there only moves the cursor/activates a panel — it never marks an
entry. This adds three mouse gestures, matching the issue's own parenthetical:

1. **One** — a right-click on an entry toggles its selection mark.
2. **More, using a modifier** — a Ctrl+left-click selects every entry between wherever the cursor
   already was and the clicked entry (inclusive). (The issue's own wording said "shift" — see the
   second addendum above for why this plan uses Ctrl instead.)
3. **Right mouse click/drag** — holding the right button and dragging over further entries
   extends the selection to each one, without needing to click them individually.
4. **Ctrl+right-click** (the addendum) — right-click one entry, then right-click a second entry
   while holding Ctrl, to select every entry between the two (inclusive) — the same range-select
   outcome as gesture 2, but starting and ending both endpoints with a right-click instead of a
   left-click.

Like UI-012/IS-0003, this is purely additive: every resulting selection is already reachable
from the keyboard alone (Insert toggles one entry and advances; Ctrl+`+`/Ctrl+`-`/`*` select-
all/clear/invert; `Ctrl+A`... wait, `Ctrl+A` is attribute-toggle here, selection-by-mask is
`+`/`-` with a typed pattern) — this only adds a faster, mouse-only *path* to selections that
were always achievable by moving the cursor and pressing Insert repeatedly, so KEY-001's "no
mouse-only feature" invariant is preserved the same way UI-012/IS-0003 already established it.

## Why the right button (not a second use of the left button)

The left button is already fully committed: a plain left-drag is IS-0003's copy/move
drag-and-drop (source panel to destination panel), and a plain left-click moves the cursor and
can activate a panel. Overloading left-drag for "select a range by dragging" would collide with
that already-shipped gesture (which one should a left-drag mean?), so this plan deliberately
follows Total Commander's own convention of using the **right** button for click/drag selection,
leaving the left button's existing meanings completely untouched. Ctrl+left-**click** (not
drag) is unambiguous and doesn't collide with anything, so it's the one selection gesture that
still uses the left button.

## Design

### 1. Right-click: toggle one entry

Add two small `Panel` methods (`Panel.h`/`.cpp`), alongside the existing
`ToggleCursorSelection`/`SelectByMask` (same "skip `..`, it's never selectable" rule both
already follow):

```cpp
// Toggles entry `index`'s selection mark (except "..") and returns its new
// state — used by IS-0004's right-click-to-select so the caller can seed a
// drag-select's target state without a separate read.
bool ToggleEntrySelected(int index);

// Forces entry `index`'s selection mark to `selected` (except "..") — used
// by IS-0004's right-drag to extend a selection over every entry the mouse
// passes over, applying the drag's target state rather than re-toggling on
// each visit (so passing over the same cell twice doesn't flicker it).
void SetEntrySelected(int index, bool selected);
```

A right-button press (`dwEventFlags == 0` or `DOUBLE_CLICK`, `RIGHTMOST_BUTTON_PRESSED` set,
matching the existing left-click precedent's own event-flag guard) hit-tests via the existing
`HitTestPanel`, moves the cursor to the clicked entry (matching left-click's own behavior, for
consistent visual feedback), then calls `ToggleEntrySelected`.

### 2. Right-click + drag: extend the selection

A new cross-event state struct in `main.cpp`, mirroring `DragState`'s own press/move/release
shape (IS-0003) — needed for the same reason: Windows console mouse events carry no distinct
"button released" event, so release is still inferred from `RIGHTMOST_BUTTON_PRESSED`
disappearing between events:

```cpp
struct SelectDragState {
    bool active = false;
    bool isLeftPanel = false;
    bool targetSelected = false;  // the state every further entry is forced to
    int lastIndex = -1;           // avoids re-processing the same cell repeatedly
};
```

Unlike `DragState`, this needs no "candidate" stage — the very first right-press already commits
to toggling that entry (there's no ambiguity to resolve, since a right button press has no other
meaning in this app to disambiguate against), so `active` becomes true immediately on press.
While `active` and the button stays held, each further `HandleMouseEvent` call that hits a
*different* entry (`index != lastIndex`) than last time, in the *same* panel the drag started in
(a right-drag does not cross panels — selection is a per-panel concept, unlike copy/move's
cross-panel drag), calls `SetEntrySelected(index, targetSelected)` and moves the cursor there.
Dragging outside both panels, or into the other panel, simply has no further effect until the
mouse returns to the origin panel — not specially tracked, since each event is handled
statelessly against the panel comparison above. On release, `SelectDragState` just resets;
unlike `DragState`'s drop, there's no deferred action to perform — every selection change already
took effect live as the mouse passed over each entry.

This whole gesture is handled in its own block near the top of `HandleMouseEvent`, entered
whenever the right button is held or a right-drag is already active, and returns before reaching
any of the existing left-button branches (hint-bar click, copy/move `DragState`, plain
click/double-click) — so the two buttons' gestures can never interfere with each other. A
`RIGHTMOST_BUTTON_PRESSED` (or a `SelectDragState`) check right after the existing
`MOUSE_WHEELED` check is where this is inserted.

### 3. Ctrl+left-click: select a range

In the existing plain-left-click branch (`main.cpp`, after IS-0003's `drag.dragCandidate`
handling), capture the panel's cursor position *before* this click moves it
(`int previousCursor = clicked.Cursor();`), then, once the click has moved the cursor to the
clicked entry as it already does today: if `mouse.dwControlKeyState & (LEFT_CTRL_PRESSED |
RIGHT_CTRL_PRESSED)`, call a new `Panel::SelectRange(previousCursor, index)` and return —
skipping the existing double-click/`drag.dragCandidate` logic below it entirely, so
Ctrl+left-drag is never interpreted as a copy/move drag candidate (that stays exclusively a
plain, unmodified left-drag, per IS-0003). (Originally designed and shipped with Shift instead
of Ctrl — see the second addendum at the top of this document for why it was switched.)

```cpp
// IS-0004: sets every entry whose index falls in the inclusive range
// [min(a,b), max(a,b)] selected (except ".."); entries outside the range
// are left untouched — the same "only touch what's named/ranged, never a
// wholesale replace" convention SelectByMask already follows, rather than
// Explorer's own "shift-click replaces the whole selection" behavior.
void SelectRange(int indexA, int indexB);
```

**Anchor model, and how it differs from Explorer's:** the range's other endpoint is always
*wherever the panel's cursor already was* at the moment of the click — not a separately
remembered, "sticky" anchor that survives across multiple Ctrl+clicks the way Windows Explorer's
own Shift+click does. Concretely: click A, then Ctrl+click B selects [A, B] as expected; a
further Ctrl+click C then selects from *B* (the cursor's new position after the previous click),
not from the original A. This is a deliberate simplification — it needs zero new persisted
`Panel` state (no anchor field, no reset-on-plain-click bookkeeping) and matches how several
other terminal file managers implement range-select, at the cost of not being pixel-for-pixel
Explorer behavior for a *second* Ctrl+click in a row. Called out explicitly here so it reads as a
documented scope decision, not a bug, if it's ever compared side-by-side with Explorer.

### 4. Ctrl+right-click: select a range between two right-clicked items

The addendum's own wording — "select an item by right mouse click, and another also but any
SHIFT is pressed select all items... between the first and second selected item" — maps onto the
*already-implemented* right-click branch almost exactly, because a right-click already moves the
panel's cursor to the clicked entry before toggling it (Section 1 above). That means the "first
selected item" is, at the moment of the second right-click, still sitting at `Cursor()` — exactly
the same anchor Section 3's Ctrl+left-click already uses. No new state, and no new `Panel`
method, is needed: `Panel::SelectRange` (Section 3) is reused as-is. (Originally designed and
shipped with Shift instead of Ctrl — see the second addendum at the top of this document.)

Change needed, entirely inside the "fresh right-button press" branch added in Section 2 (the one
guarded by `!selectDrag.active`, in `main.cpp`'s `HandleMouseEvent`): capture
`int previousCursor = clicked.Cursor();` before moving the cursor to the newly-clicked entry (the
branch already moves the cursor unconditionally, same as the plain-click branch does). Then,
*instead of* calling `ToggleEntrySelected` when `mouse.dwControlKeyState & (LEFT_CTRL_PRESSED |
RIGHT_CTRL_PRESSED)` is set, call `clicked.SelectRange(previousCursor, index)` and return
*without* setting `selectDrag.active` — i.e. Ctrl+right-click is a single discrete gesture, not
draggable (matching Ctrl+left-click's own "returns immediately, never becomes a drag candidate"
shape in Section 3, for the same reason: combining a range-select with an in-progress drag would
be ambiguous about which one the user meant). A plain (non-Ctrl) right-click continues to toggle
exactly as Section 1/2 already describe.

Net effect: right-click A (toggles A on); Ctrl+right-click B selects every entry in [A, B]
inclusive (a superset of "toggle B", so B ends up selected too, consistent with A). Exactly
mirrors Ctrl+left-click's own anchor model (Section 3) — including the same documented
"anchor is wherever the cursor currently is, not a sticky one across repeated clicks"
simplification — just entered via the right button instead of the left.

## Files touched

- `Source/MyCommander/Panel.h`/`Panel.cpp` — `ToggleEntrySelected`, `SetEntrySelected`,
  `SelectRange`. (Section 4's addendum reuses `SelectRange` as-is — no further `Panel` change.)
- `Source/MyCommander/main.cpp` — `SelectDragState`; the new right-button block in
  `HandleMouseEvent`; the Ctrl+left-click branch; a `SelectDragState selectDrag;` declared in
  `wmain` alongside `drag`/`pendingEdits`, threaded into `HandleMouseEvent`'s parameter list.
  Section 4's addendum is a small edit inside the already-added right-button block (captures the
  pre-move cursor position and branches on the Ctrl bits), not a new block. The second addendum
  (Shift -> Ctrl) touched only the two `dwControlKeyState` checks and their doc comments — no
  `Panel`-level change, since `SelectRange` itself doesn't know or care which modifier its caller
  used to decide to call it.
- Tests: `Source/MyCommanderTests/PanelTests.cpp` (new cases for the three new `Panel` methods —
  none further needed for Section 4 or the Shift -> Ctrl switch, since both only change which
  `main.cpp` condition calls the already-tested `SelectRange`/`ToggleEntrySelected`).

## Test plan

`ToggleEntrySelected`/`SetEntrySelected`/`SelectRange` are ordinary `Panel` methods with no
`Console` dependency, directly testable the same way `PanelTests.cpp` already covers
`ToggleCursorSelection`/`SelectByMask`/`InvertSelection`: toggling an entry on then off,
confirming `".."` is never selectable through either new method, `SelectRange` with `a < b`,
`a > b` (reversed order still selects the same inclusive range), `a == b` (single-entry range),
and confirming entries outside a `SelectRange` call keep whatever selection state they already
had (the "additive, not replacing" behavior called out above). `HandleMouseEvent`'s right-button
dispatch and the `SelectDragState` state machine itself gets no automated coverage, consistent
with this project's established limit (`main.cpp` has no direct test coverage — the same
limitation every mouse-handling change since UI-012 has noted). Manual verification of
right-click, right-drag, Ctrl+left-click, and Ctrl+right-click in the running app is still
outstanding — no interactive-console capture is available in this environment; the Shift -> Ctrl
switch itself was diagnosed and reported directly by the user, not caught by any automated test,
underscoring that this whole file's `main.cpp` gap is a real, not just theoretical, limitation.

## Risk / scope notes

- No `Console`/`Panel` behavior changes for the *keyboard* selection path (Insert/Ctrl+`+`/
  Ctrl+`-`/`*`/mask-select) — this is additive, matching UI-012/IS-0003's own precedent.
- Simultaneous left-and-right button interaction (e.g. holding both at once mid-gesture) is
  unspecified/undefined behavior — an edge case, deliberately not solved here, the same kind of
  documented boundary IS-0003's "drop back on the source panel is a no-op" already established.
- The hint bar and dialog option clicks (IS-0003) stay left-button-only; the right button is now
  reserved exclusively for in-panel selection gestures, so there's no overlap to resolve there.
- This extends UI-012 (already Implemented, "mouse selection... without being required") and
  SEL-001/002 (already Implemented via keyboard) rather than completing a still-open
  requirement — no `Implemented`/Disposition change to any requirement row is anticipated, only
  a version-history note, the same treatment IS-0003 itself got.
- Windows Terminal (ConPTY) was never confirmed to have the same Shift-swallowing problem classic
  console host does — the switch to Ctrl was made everywhere anyway (not conditionally, based on
  host) for simplicity and consistency, at the cost of Windows Terminal users losing the
  originally-requested Shift binding too. Not re-litigated per-host, since the project has no
  runtime host-detection mechanism and adding one for this alone would be disproportionate.

## Status

**Sections 1-3 (the original request): implemented**, per the design above, with no deviations
beyond the Shift -> Ctrl modifier switch documented in the second addendum:

- `Panel::ToggleEntrySelected`/`SetEntrySelected`/`SelectRange` landed exactly as designed
  (`Panel.h`/`Panel.cpp`), including the `".."`-exclusion rule shared with every other
  selection method on `Panel`.
- `SelectDragState` and the right-button block in `HandleMouseEvent` landed exactly as designed,
  inserted right after the `MOUSE_WHEELED` check so it returns before any left-button branch.
- Ctrl+left-click range-select landed exactly as designed (originally Shift, switched to Ctrl —
  see the second addendum), including the documented "anchor is the cursor's current position,
  not a sticky one" simplification.
- See `3_Reports/ChangeLogs/changelog_2026-09-25.md`'s IS-0004 entries for the full file list and
  test counts.

**Section 4 (the 2026-09-25 addendum, Ctrl+right-click): implemented**, per the design above:

- The right-button "fresh press" branch in `HandleMouseEvent` now captures the cursor's
  pre-move position and, when Ctrl is held, calls the already-existing `Panel::SelectRange` and
  returns without starting a `SelectDragState` — exactly as designed, and reusing `SelectRange`
  unmodified (no new `Panel` method was needed after all). Originally shipped with Shift, then
  switched to Ctrl the same day after the user reported it wasn't sensed at all — see the second
  addendum at the top of this document.
- No new `PanelTests.cpp` cases were needed; the full suite (315 cases) was re-run after each
  change to confirm no regression.
- See `3_Reports/ChangeLogs/changelog_2026-09-25.md`'s IS-0004 entries for the full file list.
