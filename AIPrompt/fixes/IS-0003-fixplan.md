# Fix Plan — IS-0003

Source: `AIPrompt/issue_fix_requests.md`

> I need some extension for the current mouse working.
> - using mouse, it is required the drag drop of copy file, or move files when the SHIFT is
>   pressed
> - using the mouse, it is needed to trigger the down side menubar commands
> - on dialogs the control buttons also shall be able to trigger.

## Summary

Three independent extensions to UI-012's mouse support, filed together under one issue:

1. **Drag-and-drop** between the two panels — default drag = copy, Shift held at drop = move.
2. **Clickable bottom hint bar** ("menubar") — clicking a hint like `F5 Copy` does what `F5`
   does.
3. **Clickable dialog option buttons** — clicking `[Y]es`/`[N]o`-style options in
   `ShowChoiceDialog` selects them, the same as pressing the hotkey.

Each is architecturally separate (no dependency between them) and is written up as its own
section below. All three build directly on UI-012's infrastructure (`HitTestPanel` in
`Navigation.h`, the mouse-event plumbing already wired into `wmain`'s loop) rather than
introducing a second, parallel mouse-handling mechanism.

## Part 1 — Drag-and-drop copy/move

### Design

Windows console mouse events don't have a distinct "button released" event type —
`MOUSE_EVENT_RECORD.dwButtonState` reports which buttons are *currently* down on every mouse
event (move, click, wheel), so a release is detected by noticing a button's bit disappeared
between one event and the next. A drag is therefore a small state machine, tracked across
main-loop iterations (declared in `wmain` alongside `pendingEdits`, threaded into
`HandleMouseEvent` the same way):

```cpp
struct DragState {
    bool dragging = false;         // true once actual movement was seen, not just a press
    bool sourceIsLeftPanel = false;
    std::vector<std::wstring> items;  // captured the moment dragging starts
};
```

Transitions, all inside `HandleMouseEvent` (`main.cpp`):

1. **Press** (`dwEventFlags == 0`, `FROM_LEFT_1ST_BUTTON_PRESSED` newly set, not already
   dragging): existing UI-012 click-to-select logic runs unchanged (moves the cursor, may
   activate the other panel). Additionally remember a *drag candidate* (which panel, so a
   plain click — press then release with no movement — never becomes a drag).
2. **Move while the button stays held** (`dwEventFlags & MOUSE_MOVED`,
   `FROM_LEFT_1ST_BUTTON_PRESSED` still set, a drag candidate exists but `dragging` is still
   false): promote to `dragging = true`, capturing `items = sourcePanel.SelectionOrCursor()`
   at this exact moment (SEL-004's existing "selection, or just the cursor entry" convention —
   the same set `F5`/`F6` would act on). While `dragging`, keep updating the cursor in
   whichever panel the mouse currently hovers over (via `HitTestPanel` against the live mouse
   position) as visual feedback — the user sees where they're about to drop.
3. **Release** (`FROM_LEFT_1ST_BUTTON_PRESSED` no longer in `dwButtonState`, on any event type)
   while `dragging == true`: hit-test the drop position. If it lands in the *other* panel from
   `sourceIsLeftPanel`, perform copy (default) or move (`mouse.dwControlKeyState &
   SHIFT_PRESSED` at the moment of release) with `destDir` = the drop panel's current `Path()`
   — already known, so **no destination-path prompt** (unlike `F5`/`F6`, which ask). Dropping
   back on the same panel, or outside both panels, is a no-op — reset `DragState` either way.

### Reusing the existing copy/move engine

`DoCopyOrMove` (`main.cpp`) currently does: prompt for a destination path → check/create it →
run the `ProgressUi`/`CopyItems`/`MoveItems`/`ReportOutcome` machinery. Factor the *second*
half out into a shared helper:

```cpp
void PerformCopyOrMove(Console& console, Panel& left, Panel& right, bool leftActive,
                       Panel& source, const std::vector<std::wstring>& items,
                       const std::filesystem::path& destDir, bool isMove, Logger& logger);
```

`DoCopyOrMove` becomes: prompt for `destDir` (unchanged), then call `PerformCopyOrMove`. The
drag-drop handler calls `PerformCopyOrMove` directly with the drop panel's path as `destDir`,
skipping the prompt entirely — exactly the "I already told you where by dropping it there"
semantics drag-and-drop implies. This is a real refactor (removing what would otherwise be a
second, parallel copy of `ProgressUi`/conflict/error/elevation wiring), not new code duplicated
alongside old.

### Recommended addition: a status-line cue while dragging

The console has no drag "ghost" rendering, but the existing status bar row can say `Dragging N
item(s) — release to copy (hold Shift to move)` while `dragging == true`, so the gesture isn't
silently invisible. Not strictly required by the issue text, but cheap and directly addresses
"how would the user know this is even happening" — recommended, not mandatory.

### Test plan

`HandleMouseEvent`/the drag state machine itself has no automated coverage, consistent with
`main.cpp`'s established limit — but `PerformCopyOrMove` (once factored out) is exercisable by
existing patterns: no *new* test is strictly needed since `FileOpsTests.cpp` already covers
`CopyItems`/`MoveItems` directly, and `DoCopyOrMove` itself was never separately tested either
(same `main.cpp` limit). Manual verification: drag a file across panels (copy), drag with Shift
held (move), drag-and-drop-back-on-source (no-op), drag a multi-item selection.

## Part 2 — Clickable hint bar

### Design

`HintBar.h`'s `BuildHintBar(HintContext)` currently returns one flat, pre-joined string. Split
it into structured segments it's built from, so a segment's *label* (for rendering) and
*action* (for a click to trigger) are defined together instead of only existing as
uninterpretable text:

```cpp
// HintBar.h
struct HintAction {
    WORD virtualKeyCode = 0;
    bool ctrlHeld = false;
    bool shiftHeld = false;
    bool altHeld = false;
};

struct HintSegment {
    std::wstring label;                // e.g. "F5 Copy" -- unchanged display text
    std::optional<HintAction> action;  // nullopt = informational only, not clickable (see below)
};

std::vector<HintSegment> BuildHintSegments(HintContext context);

// Re-expressed in terms of BuildHintSegments so the two can never drift --
// same leading space + "  "-separated join as today, byte-compatible with
// HintBarTests.cpp's existing substring-based assertions.
inline std::wstring BuildHintBar(HintContext context) { /* joins BuildHintSegments(context)'s labels */ }
```

Stays header-only (matching today's style); `HintBarTests.cpp` needs no changes (it asserts
via `.find(substring)`, not exact equality, so the join logic staying equivalent is enough).

**Which segments get an `action` vs. `nullopt`:** every hint naming exactly *one* key
(`F5 Copy`, `Esc Clear`, `Ctrl+H Hidden`, `Shift+F7 NewFile`, ...) gets one. A hint naming
*two* keys as a slash-separated pair (`Alt+Left/Right History`, `Ctrl+Up/Down History`,
`Alt+F1/2 Drive`) is deliberately left `nullopt` — clicking it would have to arbitrarily guess
which of the two the user meant, which is worse than not being clickable at all. Full mapping
(one row per context; `nullopt` entries marked *not clickable*):

| Context | Clickable | Not clickable (ambiguous) |
|---|---|---|
| CommandLine | Enter, Backspace, Esc, Ctrl+H, F9, F1, F10 | Ctrl+Up/Down |
| InaccessibleLocation | Backspace, Ctrl+F, Ctrl+H, F9, F1, F10 | Alt+Left/Right, Alt+F1/2 |
| FilteredListing | Enter, Ctrl+F, Ctrl+H, F3, F5, F6, F8, F1, F10 | — |
| Selection | F2, F3, F4, F5, F6, F8, Ctrl+A, Ctrl+H, F1, F10 | — |
| Directory | Enter, Backspace, Ctrl+H, F2, F5, F6, F7, Shift+F7, F8, F1, F10 | Alt+Left/Right |
| File | Enter, Ctrl+H, F2, F3, F4, F5, F6, F8, Ctrl+A, F1, F10 | Alt+Left/Right |
| EmptyListing | Backspace, F7, Shift+F7, Ctrl+F, Ctrl+H, Alt+F7, F9, F1, F10 | Alt+Left/Right, Alt+F1/2 |

### Hit-testing and triggering

Add a generic, reusable hit-test primitive to `Navigation.h`/`.cpp` (alongside `HitTestPanel`
— same pure, Console-independent shape), since both this and Part 3's dialog buttons need
"which labeled region did a click land in":

```cpp
struct ClickableRegion { SHORT row = 0; SHORT startCol = 0; SHORT endCol = 0; };  // endCol exclusive
int MatchClickRegion(const std::vector<ClickableRegion>& regions, SHORT mouseX, SHORT mouseY);  // -1 if none
```

`main.cpp` builds the hint bar's regions the same way `DrawFrame` builds the string — call
`GetHintContext(active, commandLine)` and `BuildHintSegments` again inside the mouse handler
(cheap, pure, deterministic — the same "recompute using the same inputs `DrawFrame` used"
approach `HitTestPanel` already established), accumulating each clickable segment's column
span (row is always `console.Height() - 1`; start column is the running total of prior
segments' display widths plus separators, `TextWidth.h`'s `StringDisplayWidth`, consistent
with UI-011).

**Triggering the action** without refactoring `wmain`'s ~600-line key-dispatch switch (a much
larger, riskier change than this issue asks for): synthesize a real `KEY_EVENT` and hand it
back to the console's own input queue via `WriteConsoleInputW(console.InputHandle(), ...)` — a
standard, well-established Windows console technique. The main loop's *next* iteration reads it
back out via its ordinary `ReadConsoleInputW` call and dispatches it through the exact same
switch a real key press would, with zero changes to that switch:

```cpp
void SimulateKeyPress(Console& console, const HintAction& action) {
    DWORD controlKeyState = 0;
    if (action.ctrlHeld) controlKeyState |= LEFT_CTRL_PRESSED;
    if (action.shiftHeld) controlKeyState |= SHIFT_PRESSED;
    if (action.altHeld) controlKeyState |= LEFT_ALT_PRESSED;
    INPUT_RECORD records[2] = {};
    records[0].EventType = records[1].EventType = KEY_EVENT;
    records[0].Event.KeyEvent = {TRUE, 1, action.virtualKeyCode,
                                 static_cast<WORD>(MapVirtualKeyW(action.virtualKeyCode, MAPVK_VK_TO_VSC)),
                                 {}, controlKeyState};
    records[1].Event.KeyEvent = records[0].Event.KeyEvent;
    records[1].Event.KeyEvent.bKeyDown = FALSE;
    DWORD written = 0;
    WriteConsoleInputW(console.InputHandle(), records, 2, &written);
}
```

No `Console.h` change needed — `InputHandle()` is already public and already used directly by
`main.cpp` for other raw console I/O (`PeekEscapePressed`, the main loop itself).

### Test plan

`BuildHintSegments`'s classification (which segments get an `action`, and with which exact
key/modifiers) is a pure function of `HintContext` — directly testable, no `Console` needed,
matching `HintBarTests.cpp`'s existing style. Add cases asserting: every single-key hint has an
`action` with the documented `virtualKeyCode`/modifiers; every slash-pair hint (`Alt+Left/
Right`, `Ctrl+Up/Down`, `Alt+F1/2`) has `nullopt`; `BuildHintBar`'s output still contains every
literal string `HintBarTests.cpp` already asserts on. `MatchClickRegion` (`Navigation.h`) is
equally pure — add `NavigationTests.cpp` cases mirroring `HitTestPanel`'s own (a hit inside a
region, a miss on the wrong row, a miss between two regions, boundary columns).
`SimulateKeyPress`/the click-to-region-to-injection wiring in `main.cpp` has no automated
coverage, consistent with that file's established limit.

## Part 3 — Clickable dialog option buttons

### Design

`ShowChoiceDialog` (`Dialog.cpp`) already builds one `optionsLine` string
(`"[Y]es  [N]o"`-style) as the last line of the dialog body, left-aligned (not centered, unlike
the title) via `PadLeft(L" " + bodyLines[i], width - 2)` inside `DrawBox` — so its on-screen
start column is deterministic: `geo.x + 2` (box left edge, +1 for the border, +1 for the
leading space `PadLeft` adds). Compute each option's column span the same loop that already
builds `optionsLine` already walks:

```cpp
std::vector<ClickableRegion> optionRegions;  // Navigation.h's type, reused here
SHORT col = static_cast<SHORT>(geo.x + 2);
for (size_t i = 0; i < options.size(); ++i) {
    if (i) { optionsLine += L"  "; col += 2; }
    std::wstring segment = L"[" + std::wstring(1, options[i].hotkey) + L"]" + options[i].label;
    SHORT segWidth = static_cast<SHORT>(StringDisplayWidth(segment));
    optionRegions.push_back({optionsLineRow, col, static_cast<SHORT>(col + segWidth)});
    optionsLine += segment;
    col += segWidth;
}
```

(`optionsLineRow` = `geo.y + 3 + body.size() - 1`, the last body row — `LayOutBox`/`DrawBox`'s
own existing row math.) Add a `MOUSE_EVENT` branch to `ShowChoiceDialog`'s input loop: on a
left-button press (or its double-click variant — either should count, matching how a real
button click works regardless of speed) whose position matches `MatchClickRegion(optionRegions,
mouse.X, mouse.Y)`, return that index immediately — the exact same return value
`MatchDialogHotkey` already produces for the keyboard path, so every existing caller (which
only ever inspects the returned index) needs no change at all.

`ShowMessage` has no `DialogOption` list (dismissed by any keypress) — extend it symmetrically:
a left-button press anywhere dismisses it too, matching "any key" with "any click". Simple,
`DialogOption`-list-free, no region computation needed.

`ShowTextPrompt` is explicitly **out of scope** — it has no buttons, only a text field; mouse
support there would mean click-to-position-the-text-cursor, a materially different (and
larger) feature the issue's "control buttons" wording doesn't ask for. `ShowProgress` has no
input loop at all (it's non-interactive, `main.cpp`'s own loop polls Esc separately) — also out
of scope, nothing to click.

### Test plan

Add `MatchDialogClick`-equivalent coverage the same way `MatchDialogHotkey` already has
coverage in `DialogInputTests.cpp` — but the actual hit-testing now goes through the shared,
already-tested `Navigation.h::MatchClickRegion`, so what's left to test here is just
`ShowChoiceDialog`'s *region-building* loop (start column, per-option width, the `"  "`
separator) being correct — if that loop is pulled out as its own small pure function (e.g.
`BuildOptionRegions(options, startRow, startCol) -> std::vector<ClickableRegion>`, living in
`DialogInput.cpp` next to `MatchDialogHotkey`/`ApplyTextPromptKey`), it becomes directly
testable without a `Console`, matching this project's established pattern; add
`DialogInputTests.cpp` cases for one/two/three-option spans and confirm no overlap between
adjacent regions. `ShowChoiceDialog`'s/`ShowMessage`'s own `MOUSE_EVENT` handling itself has no
automated coverage, consistent with `Dialog.cpp`'s real-`Console` dependency (same reasoning
`DialogInput.cpp` was split out for in TST-005).

## Files touched (implementation phase, not yet made)

- `Source/MyCommander/main.cpp` — `DragState`, drag transitions in `HandleMouseEvent`,
  `PerformCopyOrMove` (factored out of `DoCopyOrMove`), hint-bar click handling,
  `SimulateKeyPress`.
- `Source/MyCommander/HintBar.h` — `HintAction`/`HintSegment`/`BuildHintSegments`;
  `BuildHintBar` re-expressed in terms of it.
- `Source/MyCommander/Navigation.h`/`.cpp` — `ClickableRegion`/`MatchClickRegion`.
- `Source/MyCommander/Dialog.h`/`Dialog.cpp` — `ShowChoiceDialog`'s region-building +
  `MOUSE_EVENT` branch; `ShowMessage`'s click-to-dismiss.
- `Source/MyCommander/DialogInput.cpp` — `BuildOptionRegions` (if factored out per the Part 3
  test-plan note above).
- Tests: `Source/MyCommanderTests/HintBarTests.cpp`, `NavigationTests.cpp`,
  `DialogInputTests.cpp`.

## Risk / scope notes

- All three parts are additive to UI-012, not a redesign of it — the existing click-to-
  select/activate-panel and wheel-scroll behavior is unchanged by any of this.
- Drag-and-drop is the highest-complexity, highest-risk part (a genuine state machine spanning
  multiple main-loop iterations) — recommend implementing/verifying it separately from Parts 2
  and 3 if the three are done incrementally, even though they're planned together here since
  the issue files them as one request.
- The ambiguous slash-pair hints staying non-clickable (Part 2) and `ShowTextPrompt`/
  `ShowProgress` staying out of scope (Part 3) are deliberate, documented boundaries — restated
  here so neither is rediscovered as a surprise or silently "fixed" later without the same
  reasoning being re-applied.
- No requirement ID's `Implemented` status is expected to change by this fix alone — this
  extends UI-012 (already Implemented) rather than completing a still-open requirement; no
  `AIPrompt/my-commander.md` disposition edit is anticipated, only a version-history note (the
  same treatment IS-0001/IS-0002 got for their own already-Implemented parent requirements).

## Status

Implemented, per the design above, with the following notes:

- **Drag-and-drop** (Part 1): implemented as designed — `DragState`, `PerformCopyOrMove`
  factored out of `DoCopyOrMove`, and the status-bar drag cue (recommended, not mandatory, in
  the original plan) were all included.
- **Clickable hint bar** (Part 2): implemented as designed, including the exact clickable/
  non-clickable mapping from the table above. `SimulateKeyPress` and `MatchHintBarClick` live in
  `main.cpp` (not a separate module) since they depend on `Console`/the live `HintContext`,
  matching `main.cpp`'s established no-separate-test-coverage limit; `BuildHintSegments` itself
  (the pure part) lives in `HintBar.h` as planned and is fully unit-tested.
- **Clickable dialog buttons** (Part 3): implemented as designed. `BuildOptionRegions` landed in
  `DialogInput.cpp` as planned (not `Navigation.h`, which holds only the generic
  `ClickableRegion`/`MatchClickRegion` primitive both the hint bar and the dialog options share).
  `ShowTextPrompt`/`ShowProgress` were left untouched, exactly as scoped out.
- No deviations from the plan's architecture. See
  `3_Reports/ChangeLogs/changelog_2026-09-24.md`'s IS-0003 entry for the full file list and test
  counts.
