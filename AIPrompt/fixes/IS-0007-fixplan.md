# Fix Plan — IS-0007

Source: `AIPrompt/issue_fix_requests.md`

> In some cases the bottom menu panel not all command are seen, because the of the font size.
> In this case the de bottom command palette shell be mon more rows.
> When the terminal resized it is required to reset the size of teh bottom command bar as it
> required to see all command.

## Summary

The bottom hint bar (the "F-key command palette" row — `UI-008`) is hard-coded to exactly one
console row. Its text is built by joining every `HintSegment` for the active `HintContext` into
one long string, which `PadOrTrim` then silently truncates to the console's width. On a narrow
terminal, or a wide font (fewer effective columns), or a context with many hints (`Directory`,
`EmptyListing` — both 12 segments), the tail of the line — often including `F1 Help`/`F10 Quit`
— is cut off and simply never shown. There's no error, no indicator, no way to reach the missing
commands: they're just gone from the display.

The fix: let the hint bar grow to as many rows as it needs to show every segment for the current
context at the current width, and shrink back down when it doesn't. The row count is recomputed
from `(console width, active HintContext)` on **every** `DrawFrame` call, not just after a resize
— context changes (moving the cursor onto a selection, typing into the command line, clearing a
filter) change which segments are shown just as often as a resize does, and a context-only change
needs the same re-layout a resize does. This also directly satisfies the issue's "when resized,
reset the size" ask as a side effect, without needing separate resize-specific handling: resize
already triggers a redraw through the same `DrawFrame` path, so recomputing on every draw call
already covers it — no additional plumbing needed for that half of the request specifically.

## Root cause / current behavior

`Source/MyCommander/main.cpp`:

- `DrawFrame` (~line 520) hard-codes the reserved bottom-row count:
  ```cpp
  SHORT contentHeight = static_cast<SHORT>(height - 3); // reserve cmdline + status + hint rows
  ```
  and draws the hint bar as a single `PutText` at a single fixed row:
  ```cpp
  console.PutText(0, static_cast<SHORT>(height - 1),
                  PadOrTrim(mc::BuildHintBar(GetHintContext(active, commandLine)), width), kAttrStatusBar);
  ```
  `PadOrTrim` (→ `TextWidth.h`'s `PadToDisplayWidth`) trims to `width` — this is the actual
  truncation point; everything past it is simply dropped, not wrapped.

- `ComputeVisibleRows` (~line 597) has its own, independent copy of the same "3 reserved rows"
  assumption, used for panel cursor paging and every mouse hit-test (`HitTestPanel` call sites):
  ```cpp
  int ComputeVisibleRows(Console& console) {
      SHORT contentHeight = static_cast<SHORT>(console.Height() - 3);
      return std::max<int>(1, contentHeight - 4);
  }
  ```
  This **must** stay in exact agreement with `DrawFrame`'s own reserved-row count, or panel
  paging/mouse clicks will target rows that don't match what's actually drawn — a bug class this
  plan has to avoid introducing, not just fix the hint bar in isolation.

- `MatchHintBarClick` (~line 1647, IS-0003) computes per-segment clickable column ranges assuming
  a single fixed row:
  ```cpp
  SHORT hintRow = static_cast<SHORT>(consoleHeight - 1);
  if (mouseY != hintRow) return std::nullopt;
  ...
  SHORT col = 1;
  for (size_t i = 0; i < segments.size(); ++i) {
      if (i) col = static_cast<SHORT>(col + 2);
      SHORT segWidth = static_cast<SHORT>(StringDisplayWidth(segments[i].label));
      if (segments[i].action && col < consoleWidth) { ...push a ClickableRegion at hintRow... }
      col = static_cast<SHORT>(col + segWidth);
  }
  ```
  This never wraps — a click on a segment that's already off the visible single row can never be
  reached today (consistent with it not being visible either), but once the hint bar wraps to
  multiple rows, this needs to wrap its column math the same way `DrawFrame` lays the text out, or
  clicks on wrapped-down segments won't register.

**Concretely reproducible even at the documented minimum size.** `HintContext::Directory`'s 12
segments (`HintBar.h`, `BuildHintSegments`) — `Enter Open`, `Alt+Left/Right History`, `Backspace
Parent`, `Ctrl+H Hidden`, `F2 Rename`, `F5 Copy`, `F6 Move`, `F7 MkDir`, `Shift+F7 NewFile`,
`F8 Delete`, `F1 Help`, `F10 Quit` — total roughly 155 display columns including the leading
space and `"  "` separators. At `kMinWidth` (60 columns, `UI-006`'s documented floor), that's
already **3 rows'** worth of content truncated down to 1 today. `EmptyListing` (also 12 segments,
similar length) is the same story. This isn't an edge case; it's the common case for these two
contexts at anything narrower than roughly 155-160 columns.

## Design

### 1. A shared, pure row-layout helper (`HintBar.h`)

Add this next to the existing `BuildHintSegments`/`BuildHintBar` (same file, same "pure, no
`Console` dependency" character the rest of `HintBar.h` already has):

```cpp
// A hint segment placed on a specific row/column — the output of laying
// BuildHintSegments' flat list out across as many rows as it takes to show
// every segment at `width` columns (IS-0007). Never splits a segment's
// label across two rows; each row is filled greedily left-to-right,
// wrapping to a new row only when the next segment (plus its "  "
// separator) wouldn't fit.
struct PlacedHintSegment {
    HintSegment segment;
    int row = 0;      // 0-based, relative to the hint bar's own first row
    SHORT col = 0;    // display column within that row
};

// Lays BuildHintSegments(context) out across however many rows are needed
// at `width` display columns. `maxRows` caps how many rows this will ever
// return (see "Row-count cap" below) — segments beyond that are simply
// omitted from the result, the same "some commands aren't reachable in an
// extreme case" fallback behavior a single truncated row already has
// today, just pushed to a much narrower slice of cases. Returns an empty
// vector for `width <= 0`.
std::vector<PlacedHintSegment> LayOutHintBar(HintContext context, SHORT width, int maxRows = 3);

// How many rows LayOutHintBar(context, width, maxRows) actually used — the
// value DrawFrame/ComputeVisibleRows need to reserve. Cheap to call
// separately from LayOutHintBar itself (callers that only need the row
// count, i.e. ComputeVisibleRows, shouldn't have to materialize the full
// placement list).
int HintBarRowCount(HintContext context, SHORT width, int maxRows = 3);
```

Layout algorithm (`LayOutHintBar`): walk `BuildHintSegments(context)` once; track `row`/`col`
starting at `(0, 1)` (the existing leading-space convention). For each segment: compute its
`StringDisplayWidth` (UI-011 — same display-width-aware measurement `MatchHintBarClick` already
uses, not raw `.size()`); if placing it at the current `col` would exceed `width`, move to
`row + 1, col = 1` first (unless already at `maxRows - 1`, in which case stop emitting further
segments entirely — the cap); place the segment, then advance `col` by its width plus the `"  "`
separator width (2) for the next one. This is a direct generalization of `MatchHintBarClick`'s
existing single-row column-walking loop, not a new concept — the change is adding the "would this
overflow `width`? then wrap" check `MatchHintBarClick` doesn't currently need.

`BuildHintBar` (the existing flat-string function `HelpBar`/tests/anything not yet updated still
calls) stays exactly as-is, unchanged — it's still correct for anything that genuinely wants one
joined line (there may be none after this fix lands, but nothing needs to be removed to make this
work, and removing it isn't part of what the issue asks for).

### 2. Wiring into `DrawFrame`

Replace the single fixed `PutText` hint-bar call with one that:

1. Computes `int hintRows = HintBarRowCount(GetHintContext(active, commandLine), width);` once,
   near the top of `DrawFrame`, before `contentHeight` is computed.
2. Changes the reserved-row constant from a literal `3` to `2 + hintRows` (cmdline + status stay
   fixed at 1 row each; only the hint bar's own share is variable):
   ```cpp
   SHORT contentHeight = static_cast<SHORT>(height - 2 - hintRows);
   ```
3. Draws each `PlacedHintSegment` from `LayOutHintBar(...)` at `height - hintRows + segment.row`
   instead of the single `console.PutText(0, height - 1, ...)` call — clearing each hint row's
   full width first (as `kAttrStatusBar`) the same way the panel-body "blank remaining rows" loops
   elsewhere in `DrawFrame`/`DrawPanel` already do, so a row that shrinks from 2 hint-rows-worth of
   content back to 1 (context or width changed) doesn't leave stale text from the previous frame
   behind.

Everywhere else in `DrawFrame` that currently reads `height - 3`/`height - 1`/`height - 2` for the
command-line and status rows needs the same `hintRows`-aware adjustment (command line moves to
`height - 2 - hintRows`, status line to `height - 1 - hintRows`, hint rows occupy
`height - hintRows` through `height - 1`) — this plan's **Files touched** section below lists
every such site found in the current source so none is missed.

### 3. Wiring into `ComputeVisibleRows`

Same recomputation, reusing `HintBarRowCount` instead of the literal `3`:

```cpp
int ComputeVisibleRows(Console& console, HintContext context) {
    SHORT contentHeight = static_cast<SHORT>(console.Height() - 2 - HintBarRowCount(context, console.Width()));
    return std::max<int>(1, contentHeight - 4);
}
```

This changes `ComputeVisibleRows`'s signature (needs a `HintContext` to size against) — every one
of its current call sites (paging, `HitTestPanel`, drag-and-drop hover/drop hit-testing) already
has access to `GetHintContext(target-or-active-panel, commandLine)` inline or one line away, so
this is a mechanical threading change, not a design problem; **Files touched** enumerates each
call site.

### 4. Wiring into `MatchHintBarClick`

Replace its inline column-walking loop with `LayOutHintBar(context, consoleWidth)`, then match
`mouseY` against `hintBarTopRow + segment.row` (where `hintBarTopRow = consoleHeight - hintRows`)
instead of the single fixed `hintRow`, building one `ClickableRegion` per placed, clickable
segment across however many rows there are. `MatchClickRegion` (`Navigation.h`, already
row-aware — `ClickableRegion::row` already exists) needs no change at all; it was already generic
enough for this.

### 5. Row-count cap and narrow-terminal interaction

Defaulting `maxRows` to **3**: at `kMinHeight` (15 rows, `UI-006`'s floor), 3 hint rows plus the
fixed 1 command-line and 1 status row leaves 10 rows for the two bordered panels (top/bottom
border + path line + `UI-003` volume line + at least one entry row each) — checked against
`DrawPanel`'s own row budget (border + path + volume + entries), this stays comfortably usable
rather than squeezing panels down to nothing. A context that still doesn't fit in 3 rows at 60
columns (arithmetically shouldn't happen — the longest context is ~155 columns, and even 3 rows
at 60 columns is 180 columns of budget — but kept as a defensive cap rather than assumed) falls
back to the exact same "extra segments simply aren't shown" behavior the single-row truncation has
today, just for a much narrower slice of cases than "every narrow terminal, always." `maxRows` is
a function default, not a magic number scattered across call sites, so it's a one-place tuning
knob if 3 turns out to be wrong in practice.

### 6. Resize handling

No separate resize-specific code path is needed. `WINDOW_BUFFER_SIZE_EVENT` already calls
`console.UpdateSize()` and then the main loop's next iteration calls `DrawFrame` again as normal
— since `DrawFrame` now recomputes `hintRows` fresh from the (now-updated) `console.Width()`
every single call, the resize case is just one instance of "width changed since the last draw,"
already handled by the general fix. This is deliberately simpler than the issue's literal wording
("when resized, reset the size") suggests is needed — flagged here so a reviewer doesn't go
looking for resize-event-specific code that doesn't exist and isn't required.

## Files touched

- `Source/MyCommander/HintBar.h` — add `PlacedHintSegment`, `LayOutHintBar`, `HintBarRowCount` (§1
  above). `BuildHintSegments`/`BuildHintBar`/`HintSegment`/`HintAction`/`HintContext` unchanged.
- `Source/MyCommander/main.cpp`:
  - `DrawFrame` (~line 520-573) — reserved-row math and the hint-bar draw call (§2).
  - `ComputeVisibleRows` (~line 597) — new `HintContext` parameter (§3), plus every call site:
    `MoveCursor`/paging key handling, `HitTestPanel` calls for click/hover/drop hit-testing, the
    drag-and-drop hover-scroll logic (all currently around lines 1723-1887, 2271 per the current
    layout — exact lines will shift once earlier edits land, so this is "grep
    `ComputeVisibleRows(console)` and update every hit," not a fixed line list).
  - `MatchHintBarClick` (~line 1647) — rewritten to use `LayOutHintBar` (§4).
  - Any other literal `height - 3`/`height - 1`/`height - 2` reference tied to the cmdline/status/
    hint rows specifically (not e.g. the search-results picker or drive picker, which have their
    own single-purpose bottom hint row unrelated to `HintContext`/`BuildHintSegments` and are out
    of scope — see **Risk / scope notes**).
- `Source/MyCommanderTests/HintBarTests.cpp` — new `TEST_CASE`s for `LayOutHintBar`/
  `HintBarRowCount` (see **Test plan**). Existing `BuildHintSegments`/`BuildHintBar` tests are
  unaffected (those functions don't change).
- `Source/MyCommanderTests/NavigationTests.cpp` — likely unaffected (`MatchClickRegion`/
  `ClickableRegion` don't change shape), double-check once `MatchHintBarClick` is rewritten in
  case any existing test there exercises it indirectly.
- `README.md` — the "Context-sensitive hints" Status bullet and/or keybindings-table preamble, if
  either currently implies a single fixed hint row (needs a re-read once the design above is
  final — not confirmed stale yet, just flagged to check).
- `AIPrompt/my-commander.md` — `UI-008`'s row stays `Implemented` (this is a refinement, not a new
  capability); likely only a version-history preamble note, the same treatment `IS-0005`'s
  Files-touched section describes for a behavior-only fix with no `Implemented`/Disposition change
  — confirm against `UI-008`'s and `UI-006`'s exact current wording before writing that note.

## Test plan

`LayOutHintBar`/`HintBarRowCount` are pure (`HintContext` + `SHORT width` in, data out) — fully
unit-testable without a `Console`, matching every other `HintBar.h` function's existing precedent.
Proposed cases:

- A context whose total width fits in one row at a wide width (e.g. `CommandLine` at 200 columns)
  → `HintBarRowCount` is 1, every segment lands on `row == 0`.
- `HintContext::Directory` at `kMinWidth` (60) → `HintBarRowCount` > 1 (concretely, given the
  ~155-column estimate above, expect 3), and **every** segment from `BuildHintSegments(Directory)`
  appears exactly once somewhere in `LayOutHintBar`'s output — the core "nothing gets silently
  dropped anymore" regression guard the issue is actually about.
- No segment's `col + StringDisplayWidth(label)` ever exceeds `width` — wrapping never lets a
  label run past the right edge.
- Row/column values are monotonically consistent (a segment on a later row never has a smaller
  row index than one before it in `BuildHintSegments`' original order — the walk never reorders
  segments, only breaks lines between them).
- `maxRows` cap: a synthetic very-narrow width (or a `maxRows` of 1 passed explicitly) truncates
  cleanly — no crash, no out-of-range row, remaining segments just absent from the result.
- `width <= 0` → empty result, not a crash (defensive, cheap to guarantee).

Also add/extend a `MatchHintBarClick`-equivalent coverage point (wherever the rewritten function's
tests land — likely still colocated with its current test coverage, if any, or new coverage
alongside `NavigationTests.cpp`'s existing `MatchClickRegion` cases) confirming a click on a
segment that only exists on a *wrapped* row (row > 0) still resolves to the right `HintAction` —
this is the one behavior that's genuinely new, not just "the same thing on more rows."

No test can drive `DrawFrame`/`ComputeVisibleRows`/live resize end-to-end (no interactive-console
capture available in this environment, the same standing limitation every prior `IS-000N` fix in
this series has noted) — verification there is build-clean plus a manual check once implemented:
resize a real terminal narrower until a hint context needs 2-3 rows, confirm every command becomes
visible and panels shrink to make room, confirm mouse clicks on a wrapped-down hint still work,
confirm cursor paging (`Page Up`/`Page Down`) still pages by the *correct* (now-smaller) row count
rather than one computed before the hint bar grew.

## Risk / scope notes

- **Keep `ComputeVisibleRows` and `DrawFrame` in agreement.** This is the single biggest way this
  fix could go subtly wrong — if one uses stale/different `hintRows` math than the other (e.g. one
  call site still hardcodes `- 3`), mouse clicks and Page Up/Down will target rows that don't
  match what's actually drawn. Grepping for every literal `- 3`/`ComputeVisibleRows(console)` call
  in `main.cpp` before considering this done is essential, not optional.
- **Search-results picker (`Alt+F7`) and drive picker (`Alt+F1`/`Alt+F2`) are out of scope.** Both
  have their own single-line bottom hint (`kStrSearchResultsHint`/`kStrSelectDriveHint`), fixed,
  short, single-purpose strings unrelated to `HintContext`/`BuildHintSegments` — not what the issue
  describes ("the bottom menu panel"/"command palette" reads as the main dual-pane screen's
  function-key hint bar specifically), and not touched by this plan.
- **Dialogs' own `redrawBackground` callback.** `DrawFrame` is reused as the "repaint what's
  behind me" callback for `ShowChoiceDialog`/`ShowMessage`/`ShowTextPrompt`, called with
  `commandLine` left at its default empty value in that path (per `DrawFrame`'s own existing
  comment). Since `HintContext` depends partly on `commandLine`, the hint-bar row count computed
  while a dialog is open could differ by one context (never by width) from what it would be with
  the dialog closed — purely cosmetic (the dialog box covers the affected rows anyway), already
  true of the existing single-row hint text today, not a new problem this fix introduces, but
  worth a one-line callout in code review so it isn't "discovered" later as a surprise.
- **No requirement ID's Implemented/Disposition field is expected to change** — `UI-008` (context-
  sensitive hints) and `UI-006` (resize adaptation) are both already `Implemented`; this refines
  behavior under both, it doesn't newly satisfy either from scratch.
- **`maxRows = 3` is a judgment call, not derived from the issue text**, which doesn't specify a
  number ("shell be mon more rows" / "as it required to see all command"). Flagged in §5 above as
  the one real open design decision in this plan — worth confirming before implementation starts,
  since it directly trades off against how much of a minimum-size (15-row) terminal the hint bar
  is allowed to consume.

## Status

**Implemented**, per the design above, with two notes:

- `MatchHintBarClick`'s row-aware click matching (§4) did **not** get a dedicated automated test.
  It lives in `main.cpp`, un-exported, and — like every other key/mouse-dispatch function there
  (`ClassifyNavigationKey`'s callers, `HandleMouseEvent` itself, etc.) — has never had its own
  `TEST_CASE`; exporting it just to test this one change would have been a bigger, separate
  architectural change the issue didn't ask for. Verified live instead (see below). The pure
  `LayOutHintBar`/`HintBarRowCount` functions it's built from are fully unit-tested, including the
  exact `Directory`-at-60-columns case this behavior depends on.
- Live verification confirmed the *mechanism* (resize correctly re-triggers `DrawFrame`, the
  `UI-006` too-small threshold fires/clears correctly across it, column-narrowing takes visible
  effect, no crash across five separate resizes) but could **not** visually confirm the wrapped
  hint-bar rows themselves on screen — `PrintWindow` against the Windows Terminal host in this
  environment reliably clips the bottom several rows of its own composited content regardless of
  window size or position (confirmed by repeatedly hitting the same artifact on unrelated
  screenshots elsewhere this session, not something introduced by this change). This is the same
  "no interactive-console capture available in this environment" limitation every prior `IS-000N`
  fix in this series has already noted, encountered here in a more specific, diagnosed form.

Otherwise no deviation from the design: `HintBar.h` gained exactly `PlacedHintSegment`/
`LayOutHintBar`/`HintBarRowCount`; `DrawFrame`/`ComputeVisibleRows`/`MatchHintBarClick` and every
`ComputeVisibleRows` call site in `main.cpp` were updated together (including the hint-bar click
pre-filter's row range, folded into "any other literal reserved-row reference" in **Files
touched** above rather than called out as its own line item there). `maxRows = 3` was used as
designed, unchanged. See `3_Reports/ChangeLogs/changelog_2026-09-25.md`'s IS-0007 entry for the
full file list and test counts.

## Addendum — blank status row above the hint bar

**Reported:** user screenshot of the live app showed an apparently blank/useless green row sitting
directly above the (correctly working) multi-row hint bar, in the default "cursor on an entry,
nothing selected" browsing state.

**Root cause:** this was pre-existing behavior, not something the original fix above introduced —
just newly noticeable. `BuildStatusLine(panel)` (main.cpp, ~line 191) returns an **empty string**
whenever nothing is selected and `panel.StatusMessage()` is also empty, which is the common/default
browsing state. `DrawFrame` always drew that empty string as its own reserved row, using the same
`kAttrStatusBar` green background as the hint bar directly below it — so the empty status row and
the hint bar visually merged into one green block, reading as "one extra blank hint row" once the
hint bar became tall enough (2-3 rows, per the fix above) to draw attention to the adjacency. At a
single fixed hint row this was much less visible, which is presumably why it went unreported until
now.

**Fix:** made the status-line row conditional. `DrawFrame` now computes
`bool hasStatusLine = !status.empty();` and only reserves/draws that row when true; the reserved-row
formula changed from the fixed `2 + hintRows` to `1 + (hasStatusLine ? 1 : 0) + hintRows`.
`ComputeVisibleRows` gained a matching `bool hasStatusLine` parameter and identical formula, kept in
agreement with `DrawFrame` for exactly the same reason (§ "Risk / scope notes" above) the original
`hintRows` threading had to be — every call site in `HandleMouseEvent` and `wmain`'s key-dispatch
loop was updated to pass `!BuildStatusLine(target-or-active-panel).empty()` alongside its
`HintContext`. `MatchHintBarClick`/`LayOutHintBar` are unaffected (they only reason about the hint
bar's own rows, not the status row above them).

**Files touched:** `Source/MyCommander/main.cpp` only — `DrawFrame`, `ComputeVisibleRows`'s
signature, `HandleMouseEvent`'s ~9 internal `ComputeVisibleRows` call sites, and `wmain`'s
key-dispatch loop's one call site.

**Test plan:** no new automated test — `hasStatusLine` is a boolean threaded through existing
`main.cpp` dispatch/draw glue that (per the original plan's own precedent) has no `TEST_CASE`
coverage of its own; the full `MyCommanderTests.exe` suite (329 cases, 11508 checks) was re-run
and passed with zero regressions after the change. Verified by code review that every
`ComputeVisibleRows` call site and `DrawFrame`'s own row math agree (grepped for `hasStatusLine`/
`reservedRows`/`BuildStatusLine` across `main.cpp`). Live pixel-level confirmation of the row
actually disappearing is subject to the same `PrintWindow`/Windows Terminal capture limitation
noted above and in prior `IS-000N` fixes; the user's own screenshot (which was not subject to that
capture-tool limitation) is what surfaced the original bug, and the fix directly removes the
`PutText` call that authored the blank row when `hasStatusLine` is false.

**Status:** Implemented.
