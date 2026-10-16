# Fix Plan — IS-0015

Source: `AIPrompt/issue_fix_requests.md`

> Main app, any plugin panel list
>     Now I can select files using mouse and numeric pad + and so on, but I want to add a new
>     selection feature, when the user press any SHIFT and Down/Up select/unselect the items.
>     When the same directory is shown by left and right panel, and any changes happen on a side
>     the other is not synchronised, fix it.
>
> Any where where one row string editor is working.
>     Switch on the cursor, position it, be able to move the cursor back, and insert, update the
>     previously typed character. A minimal but usable editor needed!

## Summary

Three independent asks filed together:

1. **Shift+Up/Down range-select**, Explorer-style — extend/shrink the selection as the cursor moves,
   not just toggle-and-move (`Insert`'s existing behavior).
2. **Same-directory panel desync** — when both panels show the same real directory, an operation
   performed via one panel doesn't refresh the other, so it goes stale.
3. **A real single-line text editor** (cursor positioning, insert-at-cursor, forward/backward
   delete) — today, the command line, `ShowTextPrompt` dialogs, and `ShowFormDialog`'s text fields
   all independently implement *append-at-end + Backspace-removes-last-character only*; none
   support a positionable cursor at all.

## Part 1 — Shift+Up/Down range-select

### Current behavior

- `shiftHeld`/`ctrlHeld`/`altHeld` are already computed once per key event (`main.cpp:2656-2658`)
  and already used elsewhere in the same switch, so they're live and in scope exactly where
  `case VK_UP`/`VK_DOWN` live (`main.cpp:2770-2802`) — currently those cases only call
  `ClassifyNavigationKey` and `target.MoveCursor(±1, visibleRows)`; `shiftHeld` isn't consulted.
- `ClassifyNavigationKey`/`NavKeyContext` (`Navigation.h`) has no Shift awareness at all — its own
  doc comment scopes it to "genuinely navigational keys," not selection, so adding a `shiftHeld`
  field there would be scope creep on that function and would force updating every existing
  `NavKeyContext{...}` test construction in `NavigationTests.cpp`. Better handled directly in
  `main.cpp`'s switch, parallel to how `shiftHeld` already gates other behavior there (e.g. the
  permanent-delete modifier).
- `Panel` already has the right primitives: `SetEntrySelected(int index, bool selected)` and
  `MoveCursor(int delta, int visibleRows)` — these are exactly what the existing mouse right-drag
  selection gesture uses (`main.cpp:2123,2138`) to "set this entry's selection to a known state as
  the mouse passes over it." `ToggleCursorSelection` (used by plain `Insert`) toggles-then-moves,
  which is the wrong shape here (Explorer's Shift+Arrow always *selects* while extending and
  *deselects* while shrinking back over already-selected ground — it doesn't toggle).
- **No anchor concept exists anywhere** — confirmed by `README.md:244-250`'s own explicit note that
  the existing Ctrl+click mouse range-select's "anchor" is just the cursor position captured
  locally at click time, never a persisted `Panel` field. A correct Shift+Arrow implementation
  needs one, to support shrinking the selection back, not just growing it.

### Design

Add a small persisted anchor to `Panel`, and one new method that does both the move and the
selection bookkeeping together:

```cpp
// Panel.h
void ExtendSelection(int delta, int visibleRows);  // Shift+Up/Down: IS-0015
...
private:
    int selectionAnchor_ = -1;  // -1 = no active Shift+Up/Down gesture
```

```cpp
// Panel.cpp
void Panel::MoveCursor(int delta, int visibleRows) {
    selectionAnchor_ = -1;  // any ordinary cursor move breaks a Shift+Up/Down gesture in progress
    ...existing body unchanged...
}

void Panel::ExtendSelection(int delta, int visibleRows) {
    int anchor = (selectionAnchor_ < 0) ? cursor_ : selectionAnchor_;
    int oldCursor = cursor_;
    MoveCursor(delta, visibleRows);      // clears selectionAnchor_ as a side effect
    selectionAnchor_ = anchor;            // restore it for this gesture
    int newCursor = cursor_;
    int oldLo = std::min(anchor, oldCursor), oldHi = std::max(anchor, oldCursor);
    int newLo = std::min(anchor, newCursor), newHi = std::max(anchor, newCursor);
    // Shrinking: deselect whatever's no longer in range.
    for (int i = oldLo; i <= oldHi; ++i) if (i < newLo || i > newHi) SetEntrySelected(i, false);
    // Growing: select the (possibly new) full range.
    for (int i = newLo; i <= newHi; ++i) SetEntrySelected(i, true);
}
```

Putting `selectionAnchor_ = -1` at the top of `MoveCursor` itself (rather than hunting down every
call site that should break the gesture) means Tab, mouse clicks, Page Up/Down, Home/End, plain
Up/Down, directory navigation, and `Refresh()` all naturally end a Shift+Up/Down gesture for free,
since everything already funnels through `MoveCursor`.

`main.cpp`'s `case VK_UP`/`VK_DOWN` gain one branch each:

```cpp
case VK_UP:
    if (shiftHeld) { target.ExtendSelection(-1, visibleRows); break; }
    ...existing ClassifyNavigationKey/history-browsing logic, unchanged...
case VK_DOWN:
    if (shiftHeld) { target.ExtendSelection(+1, visibleRows); break; }
    ...unchanged...
```

Scoped deliberately to `Shift+Up`/`Shift+Down` only, matching the issue's own wording — not
`Shift+Page Up/Down`/`Shift+Home/End`, which would be a natural follow-up but isn't asked for here.

### Files touched

- `Source/MyCommander/Panel.h`/`.cpp` — `selectionAnchor_`, `ExtendSelection`, one line added to
  the top of the existing `MoveCursor`.
- `Source/MyCommander/main.cpp` — `case VK_UP`/`VK_DOWN`'s new `shiftHeld` branch.
- `Source/MyCommanderTests/PanelTests.cpp` — new cases: extend then shrink back (confirms
  deselection), extend past the original anchor in the other direction, an ordinary `MoveCursor`
  call breaking a prior gesture.
- `README.md`/`Source/MyCommander/Help.cpp` — keybindings documentation.
- `AIPrompt/my-commander.md` — new `SEL-007` row.

### Test plan

`Panel::ExtendSelection` is pure enough to unit test directly (no `Console` dependency) — add cases
to `PanelTests.cpp` per the above. Manual verification: Shift+Down three times selects three
entries and moves the cursor; Shift+Up twice afterward deselects back to one; starting fresh with
Shift+Up extends upward correctly.

## Part 2 — Same-directory panel desynchronization

### Current behavior

Confirmed by reading every `Do*` operation in `main.cpp`: **Rename, Create directory, Create empty
file, Delete, Change attributes, Execute, and the synchronous Edit-and-wait path** all call
`.Refresh()` (or `.NavigateTo(...)`) on only the *target* (active) panel — the other panel, even
when it's showing the exact same directory, is never told to refresh. (Copy/Move and the two
shell-command paths already refresh both panels unconditionally, so they're not part of this bug —
left unchanged.) The one existing same-path-aware code, `RefreshPanelsForEditedFile`
(`main.cpp:1444-1449`, the async file-change-notification path from IS-0002), uses a plain
`std::filesystem::path::operator==`, which is **case-sensitive even on Windows** and doesn't
normalize trailing separators — the wrong comparison for "is this the same directory," and not a
pattern to copy. The codebase already has the *right* tool, already used elsewhere:
`std::filesystem::equivalent` (`FileOps.cpp:379`), which resolves both paths against the real
filesystem (same volume + file ID) — case-insensitive and separator-agnostic on Windows.
`PluginManager::HostRefreshPanel` (used by `RefreshPanel` from any plugin, e.g. ZipPlugin) is
entirely one-sided too — no same-path awareness at all.

### Design

Add one small, shared, header-only helper to `PathUtil.h` (already the project's home for
`Panel`/`FileOps`/`Search`-shared path helpers):

```cpp
// PathUtil.h
// Whether `a` and `b` name the same filesystem entry (same volume + file ID) --
// unlike std::filesystem::path's own operator==, this is case-insensitive and
// separator-agnostic on Windows. False on any error (e.g. a path that doesn't
// currently exist), matching FileOps.cpp's own existing fs::equivalent usage.
inline bool PathsEquivalent(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::error_code ec;
    return std::filesystem::equivalent(a, b, ec) && !ec;
}
```

Use it at two points:

1. **`main.cpp`**: a small helper, `SyncOtherPanelIfSamePath(Panel& target, Panel& other)`, calling
   `other.Refresh()` when `mc::PathsEquivalent(target.Path(), other.Path())` — called right after
   the existing single-panel refresh in `DoRename`, `DoMakeDirectory`, `DoCreateNewFile`,
   `DoDelete`, `DoChangeAttributes`, `DoExecute`, and the synchronous branch of `DoEdit`. Each of
   these functions already takes `(Console&, Panel& left, Panel& right, bool leftActive, ...)`, so
   `other` is trivially `leftActive ? right : left`.
2. **`PluginManager::HostRefreshPanel`** (`PluginManager.cpp:1116-1119`): after refreshing the
   requested `side`, also check the complementary side and refresh it too if it's showing an
   equivalent path. This closes the gap for every plugin that already calls `RefreshPanel`
   (ZipPlugin's Pack/Unpack/add-to-archive among them) with **no ZipPlugin-side change needed** —
   the fix lives entirely at the host layer plugins already call through.

### Files touched

- `Source/MyCommander/PathUtil.h` — new `PathsEquivalent` helper.
- `Source/MyCommander/main.cpp` — new `SyncOtherPanelIfSamePath` helper, called from six existing
  `Do*` functions plus `DoEdit`'s synchronous branch.
- `Source/MyCommander/PluginManager.cpp` — `HostRefreshPanel`'s new same-path check.
- `Source/MyCommanderTests/PathUtilTests.cpp` — new cases for `PathsEquivalent` (same path two
  ways — trailing slash, different case — not equivalent for two genuinely different directories,
  false on a nonexistent path).
- `AIPrompt/my-commander.md` — new `NAV-012` row.

### Test plan

`PathsEquivalent` is pure/header-only and directly unit-testable in `PathUtilTests.cpp`
(`TempDir` fixture, same precedent as every other `PathUtil.h` test). The `Do*`-function wiring
and `HostRefreshPanel`'s own change are real `Console`/`Panel`-dependent integration behavior with
no existing headless-test precedent for this class of change — manual verification: open the same
directory in both panels, create/delete/rename/attribute-toggle a file from the active panel,
confirm the other panel's listing updates without manually refreshing it; repeat with a ZipPlugin
operation (e.g. Pack into a directory shown in both panels).

## Part 3 — A real single-line text editor (cursor, insert, delete)

### Current behavior

All three single-line text surfaces are independently reimplemented, and all three share the exact
same limitation — append-at-the-end plus Backspace-removes-the-last-character only, no cursor
concept, no Left/Right/Home/End/forward-Delete:

- **Command line** (`main.cpp`): `commandLine` is a plain `std::wstring`; `appendToCommandLine`
  always `push_back`s, `VK_BACK` always `pop_back`s. Plain `Left`/`Right` currently resolve to
  `NavIntent::None` and do nothing at all (confirmed free to repurpose) — only `Alt+Left`/`Right`
  (panel history) and `Ctrl+Up`/`Down` (command history) are bound.
- **`ShowTextPrompt`** (`Dialog.cpp`/`DialogInput.cpp`'s `ApplyTextPromptKey`): same append/
  pop_back-only model; the caret is literally a trailing `"_"` character concatenated onto the
  string for display (`Dialog.cpp:212`), not a real positionable cursor.
- **`ShowFormDialog`'s text fields** (`DialogInput.cpp`'s `ApplyFormFieldKey`, `FormFieldKind::Text`
  case): identical append/pop_back-only model; Left/Right *are* handled already, but only for the
  Checkbox/Choice/Number field kinds (toggle/cycle/nudge), not Text.

No shared line-editing primitive exists anywhere to consolidate around — this is the most invasive
part of this issue, touching three separate call sites' both logic and rendering.

### Design

**One new shared, pure primitive**, matching the existing `ApplyTextPromptKey`/`ApplyFormFieldKey`
TST-005 precedent (testable independent of `Console`):

```cpp
// DialogInput.h (alongside the existing ApplyTextPromptKey/ApplyFormFieldKey declarations)
struct LineEdit {
    std::wstring text;
    size_t cursor = 0;  // 0..text.size()
};

// Applies one keypress to `edit` in place. Returns true if it was a line-
// editing key (text or cursor position changed, including a recognized-but-
// no-op case like Home at position 0) -- false for anything else (Enter,
// Esc, Tab, ...), which the caller interprets itself (submit/cancel/etc.).
bool ApplyLineEditKey(LineEdit& edit, uint16_t virtualKeyCode, wchar_t unicodeChar);
```

Behavior: printable char → insert at `cursor`, advance (not just append — this is the literal
"insert... the previously typed character" the issue asks for); `VK_BACK` → erase the character
*before* `cursor` and move left, no-op at position 0 (not always-last-character); `VK_DELETE` →
erase the character *at* `cursor` (forward delete), no-op at the end; `VK_LEFT`/`VK_RIGHT` → move
`cursor` by one, clamped; `VK_HOME`/`VK_END` → jump to start/end; same 240-character cap the
existing implementations already enforce.

**Each of the three call sites delegates its text-editing portion to this**, keeping its own
surrounding dispatch (Esc/Enter for `ShowTextPrompt`; checkbox/choice/number cases and
submit/cancel for `ShowFormDialog`; history browsing and run/navigate for the command line) intact:

- `ApplyTextPromptKey` keeps its own Esc/Enter handling, but its text-editing branch becomes a
  thin call into `ApplyLineEditKey`. `ShowTextPrompt`'s rendering changes from "always append a
  trailing `_`" to drawing the cursor glyph *at* `edit.cursor`'s position within the string (an
  inverse-video character, or a block cursor at end-of-string when `cursor == text.size()`).
- `ApplyFormFieldKey`'s `FormFieldKind::Text` case becomes a call into `ApplyLineEditKey` (the
  Checkbox/Choice/Number cases are untouched). `ShowFormDialog`'s text-field rendering needs the
  same at-cursor-position glyph change.
- The command line in `main.cpp` adopts a `LineEdit` (or an equivalent `cursor` alongside the
  existing `commandLine` string) in place of `appendToCommandLine`'s plain `push_back` and the
  existing `VK_BACK`/`pop_back` handling; `case VK_LEFT`/`VK_RIGHT`/`VK_HOME`/`VK_END` are added to
  the main switch (free today, confirmed above), routed through `ApplyLineEditKey` when the command
  line is non-empty. The command-line row's own rendering (wherever it draws `commandLine` today)
  needs the same at-cursor glyph change.

**Design decision, flagged explicitly: `VK_DELETE`'s dual meaning.** IS-0010 bound `VK_DELETE`
unconditionally to file deletion (`case VK_F8: case VK_DELETE: DoDelete(...); break;`, no
command-line-state check). Supporting forward-delete *in* the command line needs `VK_DELETE` to
mean something different when the command line has text and the cursor isn't already at the end —
exactly the **dual-meaning pattern `Backspace` already uses** today (`ClassifyNavigationKey`'s
existing `ClearCommandLineChar` vs. parent-directory split). Recommended resolution, matching that
existing precedent rather than introducing a second, different rule: `VK_DELETE` forward-deletes in
the command line when it's non-empty; file deletion applies only when the command line is empty
(mirroring Backspace's own already-shipped convention exactly). This is a real behavior refinement
to IS-0010's own Delete-key binding, called out here rather than silently decided.

**Not in scope** (reading "a minimal but usable editor" as genuinely minimal): word-jump
(Ctrl+Left/Right), text selection within the line, clipboard cut/copy/paste, an Insert-key
overwrite-mode toggle. "Insert... update the previously typed character" is read as plain
insert-at-cursor editing, not a literal overwrite-mode feature — `Insert` stays bound to file
selection in the panel, unchanged.

### Files touched

- `Source/MyCommander/DialogInput.h`/`.cpp` (or wherever `ApplyTextPromptKey`/`ApplyFormFieldKey`
  are declared/defined) — new `LineEdit`/`ApplyLineEditKey`; both existing functions' text-editing
  branches delegate to it.
- `Source/MyCommander/Dialog.cpp` — `ShowTextPrompt`'s rendering (cursor glyph at position, not a
  trailing `_`).
- `Source/MyCommander/main.cpp` — command line adopts a cursor index; new `VK_LEFT`/`VK_RIGHT`/
  `VK_HOME`/`VK_END` cases; `VK_DELETE`'s dual-meaning split (command-line-non-empty check, mirroring
  `VK_BACK`'s own existing pattern); command-line row rendering's cursor-glyph change.
- `Source/MyCommanderTests/DialogInputTests.cpp` — new `ApplyLineEditKey` cases (insert mid-string,
  backspace/delete at various positions, Left/Right/Home/End clamping); existing
  `ApplyTextPromptKey`/`ApplyFormFieldKey` text-kind tests updated to the new (still-correct)
  behavior where their assertions were specifically about append/backspace-only semantics.
- `README.md`/`Source/MyCommander/Help.cpp` — document the new command-line editing keys.
- `AIPrompt/my-commander.md` — new `UI-016` row (umbrella requirement covering all three surfaces,
  since this isn't command-line-specific).

### Test plan

`ApplyLineEditKey` is pure and directly unit-testable (no `Console` dependency), following
`ApplyTextPromptKey`/`ApplyFormFieldKey`'s own precedent exactly — cover insert at start/middle/end,
backspace/delete at start/middle/end (including no-op at the boundary), Left/Right/Home/End
clamping, and the 240-char cap. The three call sites' own rendering changes have no existing
headless-test precedent (matching every other `Console`-drawing code in this project) — manual
verification: in each of the three surfaces, type text, move the cursor with Left/Right/Home/End,
insert and delete characters mid-string, and confirm the command line's `VK_DELETE` forward-deletes
when it has text but still deletes files when it's empty.

## Risk / scope notes

- The three parts are independent and can be implemented/landed/tested in any order.
- Part 1 is small and additive, confined to `Panel`'s own selection bookkeeping plus one `main.cpp`
  branch; `MoveCursor`'s one added line is defensive (clears a gesture-in-progress) and has no
  effect on any existing caller's behavior.
- Part 2 is additive and low-risk — `PathsEquivalent` only ever *adds* a refresh that wasn't
  happening before; it can't cause an operation to behave differently, only cause the previously-
  stale panel to catch up.
- Part 3 is the largest, most invasive part — it touches three independently-maintained call sites'
  both logic and rendering, and includes one explicit behavior refinement to an already-shipped
  feature (`VK_DELETE`'s dual meaning). Existing `ApplyTextPromptKey`/`ApplyFormFieldKey` tests that
  assert the *old* append/backspace-only semantics will need their assertions updated, not just
  extended — flagged so this isn't mistaken for an accidental regression during review.

## Status

**All three parts implemented** (2026-10-02). Parts 1 and 2 per the design above, with no
deviations (Part 1 corrected the same day, see below). **Part 3 (the line editor), implemented in
a follow-up session**, per the design above with one deviation, documented under Part 3 below.

- **Part 1:** `Panel::selectionAnchor_`/`ExtendSelection` and `main.cpp`'s `shiftHeld` branches in
  `case VK_UP`/`VK_DOWN` initially landed exactly as designed above, gated on `commandLine.empty()`
  (mirroring the same gate `ClassifyNavigationKey` already applies internally between cursor
  movement and command-history browsing).
  **Corrected the same day, after the user reported a bug**: the anchor-based design above has a
  permanent flaw — the gesture's own starting entry is always one of the range's two endpoints, so
  it's included in every non-empty range for the gesture's whole lifetime; no amount of further
  Shift+Up/Down, in either direction, ever deselects it again (reported as "last item never
  deselected even up or down"). Replaced entirely with an anchor-free design: `Panel::
  ToggleSelectionAndMove(delta, visibleRows)` — Down toggles the entry being left then moves
  (identical to `Insert`'s own `ToggleCursorSelection`); Up moves first then toggles the entry just
  arrived at. This pairing makes the gesture fully, symmetrically reversible: moving one way then
  back over the same entries toggles each one exactly twice (a net no-op), so shrinking all the way
  back now genuinely clears everything, including the starting entry. `selectionAnchor_` and the old
  `ExtendSelection` were removed outright, not deprecated — nothing outside `Panel`/`main.cpp`'s own
  two call sites ever referenced them. The 4 `PanelTests.cpp` cases below were written for the
  *original* design and no longer matched the corrected behavior — replaced (not just extended) with
  4 new cases for `ToggleSelectionAndMove`, including a regression test
  (`Panel_ToggleSelectionAndMove_DownThenUpFullyRetraces_SEL007`) exercising the exact reported
  scenario. `Help.cpp`/`README.md`/`AIPrompt/my-commander.md`'s `SEL-007` wording corrected from
  "extend/shrink a range, Explorer-style" to "toggle each entry as the cursor moves over it."
- **Part 2:** `PathsEquivalent` added to `PathUtil.h`; `SyncOtherPanelIfSamePath` added to
  `main.cpp` and wired into `DoRename`, `DoMakeDirectory`, `DoCreateNewFile`, `DoDelete`,
  `DoChangeAttributes`, `DoExecute`, and `DoEdit`'s synchronous branch; `PluginManager::
  HostRefreshPanel` gained the identical check. Bonus fix found and applied during implementation:
  `RefreshPanelsForEditedFile` (IS-0002's own pre-existing same-path logic) was using a plain,
  case-sensitive `path == path` — switched to `PathsEquivalent` too, for the same correctness
  reason. 5 new `PathUtilTests.cpp` cases (identity, case-insensitivity, trailing separator,
  different directories, nonexistent path).
- New `SEL-007`/`NAV-012` requirements added to `AIPrompt/my-commander.md`, both `Implemented`.
- Verified via `MyCommander.sln` Debug|x64 build (0 errors, including the nested Release
  installer-payload rebuild and the full `ZipPlugin.vcxproj` chain) plus `MyCommanderTests.exe`,
  after the Part 1 correction above: 434 test cases, 11,819 checks, 0 failed (up from 425/11,795
  before this session's work), each new/replaced test case individually confirmed passing by name,
  including the regression test for the reported bug.
- Full interactive verification (Shift+Up/Down in the real running app; same-directory sync across
  all six operations plus a ZipPlugin refresh) remains outstanding — the same standing
  no-interactive-console-capture limitation noted throughout this project.
- **Part 3:** implemented exactly per the Design section above — `LineEdit`/`ApplyLineEditKey`
  added to `Dialog.h`/`DialogInput.cpp`; `ApplyTextPromptKey` (gained a `currentCursor` parameter)
  and `ShowFormDialog`'s `FormField::Text` case both delegate to it; `ShowTextPrompt` renders a
  real cursor glyph in place of the old trailing `_`; the command line (`main.cpp`) gained a
  parallel `commandLineCursor` local and the same cursor-glyph rendering in `DrawFrame`.
  **One deviation from the plan, found while implementing, not while designing**: the plan's own
  Design section asserted `VK_HOME`/`VK_END` were "free today" for the command line to claim —
  false. Both already have an unconditional, shipped meaning (jump the panel cursor to the
  first/last entry), confirmed by reading the actual current `case VK_HOME`/`VK_END` in `main.cpp`
  before touching them. Resolved the same way Backspace's own pre-existing dual meaning already
  works: gated on `commandLine.empty()` (command-line-cursor when it has text, panel-cursor
  otherwise) — a natural, minimal extension of an existing, already-shipped convention rather than
  an arbitrary new rule. The same gate was also applied to `VK_DELETE` (forward-delete in the
  command line vs. `F8`/`Delete`'s existing file-deletion meaning), splitting what used to be one
  shared `case VK_F8: case VK_DELETE:` into two separate cases. 25 new/corrected
  `DialogInputTests.cpp` cases (dedicated `ApplyLineEditKey` coverage per the plan's own Test plan
  section, plus two existing assertions that were specifically about the old append/backspace-only
  semantics — not just recompiled to the new 4-argument signature — corrected to match the new,
  intentional behavior: `ApplyFormFieldKey_Text_BackspaceOnEmptyIsNoOp` now expects `true`, since
  Backspace is a recognized line-editing key even when it's a no-op; the old
  `ApplyTextPromptKey_ArrowKeyWithNoCharacter_LeavesTextUnchanged` was replaced since Left/Right are
  no longer true no-ops). New `UI-016` requirement added to `AIPrompt/my-commander.md`, `Implemented`.
  `README.md`/`USERGUIDE.md`/`Help.cpp` updated to document the new command-line editing keys.
  Verified via `MyCommander.sln` Debug|x64 full solution build (0 errors, including the nested
  Release installer-payload rebuild and the `ZipPlugin.vcxproj` chain) plus `MyCommanderTests.exe`:
  453 test cases, 11,883 checks, 0 failed.

See `3_Reports/ChangeLogs/changelog_2026-10-02.md`'s IS-0015 entry for the full file list.
