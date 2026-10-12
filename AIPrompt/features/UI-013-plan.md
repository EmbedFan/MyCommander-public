# Implementation Plan — UI-013 (Single-panel and directory-tree layouts)

Source: `AIPrompt/my-commander.md`, `UI-013` — "A single-panel and a directory-tree layout may be provided
in addition to dual-pane mode." P3, currently `LATER` / `Not Started`.

## Summary

Two independent, loosely related asks grouped under one requirement ID:

- **Part A — Single-panel layout.** The active panel fills the whole screen; the other panel's real
  path/cursor/selection are untouched underneath, just not drawn. A pure screen-geometry change.
- **Part B — Directory-tree layout.** The active panel shows a navigable tree of its own drive/share's
  directories instead of a flat file listing; picking a directory returns that panel to an ordinary
  listing there. Built as `Panel`'s existing `IPanelContent` mechanism (the same one `ZipPlugin`'s archive
  browser already uses) — not a new screen region.

They compose orthogonally: a tree can be shown inside a single-panel-width panel exactly as it is inside a
half-width one, with no extra code. Both are additive, keyboard-driven (KEY-001), and change nothing about
dual-pane mode's default behavior.

## Status

- **Part B (directory tree): implemented.** Phase 1 (`DirectoryTree.{h,cpp}` + `DirectoryTreeTests.cpp`),
  Phase 2 (`DirectoryTreeContent` in `main.cpp`, `Ctrl+T`, `View` → `Tree`, `BuiltinShortcutInventory`), and
  Phase 3 (docs, spec, version 1.14.0) are done. Decisions taken as the recommended defaults above (content-swap
  design, `Ctrl+T`, reparse points listed/marked but not auto-expanded, session-only, no tree-specific hint text,
  one `UI-013` row marked `Partial`).
- **Part A (single-panel layout): implemented 2026-10-09 (1.26.0)** to the revised plan just below, with all
  four open questions decided as recommended: `Ctrl+F1`, a new View → "Single panel" item, remembered in the
  session file, and the "Other panel" border label. UI-013 is now Implemented. One detail: the layout is restored
  from the session even when panel paths are given on the command line, because those paths only replace the
  folders.

Deviation during implementation: `ListSubdirectories` returns `std::optional<std::vector<...>>` (nullopt when
the folder itself can't be enumerated) instead of a plain vector, so "unreadable" and "empty" stay distinct.

## Part A — revised plan (2026-10-09)

The original Part A design (further below, from 2026-10-06) was written before KEY-005 (rebindable keys), the
FTP/SFTP plugin panels and SRC-006/007 (compare/synchronize). This section re-checks it against the code of
1.25.0 and **supersedes** it where they differ. The core idea is unchanged: the active panel fills the screen,
and the hidden panel keeps its path, cursor, selection and content underneath.

### Implementation check (1.25.0)

| Area | Today | Needed |
|---|---|---|
| Frame drawing | `DrawFrame` (`main.cpp:858`) always draws two boxes (`leftWidth = width / 2`, `rightX = leftWidth + 1`) and calls `DrawPanel` twice | One box, one `DrawPanel` |
| Mouse | `HitTestPanel` (`Navigation.cpp:34`) repeats that split. The panel-content click forwarding (`main.cpp:3994`) computes it a **third** time for the plugin/tree content's relative X | All three use one geometry function |
| Redraws behind dialogs | **50** `DrawFrame(...)` calls; only the main loop's (`main.cpp:4660`) passes the live command line/menu | The layout must survive every redraw (see D2) |
| Keys | KEY-005: every built-in shortcut is a row in `KeyMap.cpp`'s `Commands()`, and a test checks it against `BuiltinShortcutInventory`. `Ctrl+F1` is free | A new command row + one `case` in the key switch |
| Menu | View has `Full`/`List` stubs, plus real `Tree`, `Executable properties` and the two `Order by` items (mnemonics F, L, T, E, N, S) | A new item |
| Plugin/tree panels | Their content draws at whatever width `DrawPanel` gives it (`OnPanelDraw(width, ...)`) | Nothing: they get the full width automatically |
| Drive picker | `Alt+F1`/`Alt+F2` already make that side active (`main.cpp:1939`) | Nothing: in single-panel mode it also shows that side |
| F5/F6 to the hidden panel | The destination prompt shows (and lets you edit) the other panel's path | Nothing for safety; an on-screen hint of where it is (D4) is still useful |

### Design

**D1 — One pure geometry function** (`Navigation.{h,cpp}`, unit-tested):

```cpp
struct PanelLayout {
    bool showLeft = true, showRight = true;
    SHORT leftX = 1, leftWidth = 0;    // the panel's inner area (borders excluded)
    SHORT rightX = 0, rightWidth = 0;
    SHORT dividerX = -1;               // the middle border column, -1 when single-panel
};
PanelLayout ComputePanelLayout(SHORT consoleWidth, bool leftActive, bool singlePanel);
```

- In dual-pane mode it reproduces today's numbers exactly; a test pins this for every width from 60 to 300.
- `DrawFrame` draws the borders, junction glyphs and `DrawPanel` calls from it.
- `HitTestPanel` takes the layout instead of recomputing it.
- The content click forwarding uses `layout.leftX`/`rightX` as the origin, replacing the third copy.

**D2 — The layout is a file-scope flag (`gSinglePanelLayout`) that `DrawFrame` reads, not a new parameter.**

- **Why this changes the original A2:** the original design passed a defaulted parameter only from the main
  loop, accepting that the ~40 (now 50) "redraw the background" calls behind dialogs would flash back to
  dual-pane while a dialog is open. A flag read by `DrawFrame` itself keeps every redraw consistent with no
  call-site changes. `gPluginManager` and `gCommandLineViewOffset` are the existing precedent for state like
  this.
- **Mouse:** `HandleMouseEvent` reads the same flag through `ComputePanelLayout`, so a click can never reach the
  hidden panel.

**D3 — Toggle: `Ctrl+F1`, rebindable** (KEY-005):

- a new `Commands()` row: `single-panel` — "Single-panel layout (on/off)", default `Ctrl+F1`;
- one `case` in the main key switch, which flips the flag and redraws;
- `BuiltinShortcutInventory` follows automatically, so a plugin can't claim it;
- F1 help, README and the hint bar's relabelling follow the user's binding as for every other command.

**D4 — Telling the user where the hidden panel is.** In single-panel mode the bottom border of the visible box
carries a right-aligned label: ` Other panel: D:\Backup ` (or the content title, e.g. ` Other panel: FTP
ftp.example.com `). It is cut from the left with "..." via `TextWidth.h` when it doesn't fit, and dropped when the
panel is too narrow for it. This is what makes `F5`/`F6`/`Shift+F2` "to the other panel" understandable without
switching. The label text comes from a pure helper (`FormatOtherPanelLabel(text, maxWidth)`), unit-tested
including full-width characters (UI-011).

**D5 — Behaviour that needs no new code** (verified in the check above, listed for the user guide):

- **Tab** switches which panel is shown. **Alt+F1/Alt+F2** pick a drive and show that panel.
- **F5/F6 and Alt+F5/F6** copy/move to the hidden panel's folder; the prompt shows it.
- **Shift+F2 (compare) and Ctrl+Shift+F2 (synchronize)** work on both folders as before. The hidden panel's
  marks are there when you Tab to it.
- **ZIP/FTP/SFTP/tree panels** simply get the full width; the FTP/SFTP panel then has room for all its columns.

**D6 — Remembered across restarts (recommended).** Store one `single-panel = yes|no` line in the Session file
(NAV-008), next to the two panel paths. It is cosmetic layout state like the paths, not a preference and not
anything replayable (ERR-006), so Session (written only on a clean exit) is the right home rather than `Config`.
This reverses the original A5 ("session-only"); open question 3.

**D7 — Menu: a new View item "Single panel" (P), showing `Ctrl+F1`.** `Full` and `List` stay stubs. In Total
Commander those two mean *detailed* vs *brief* listing, so reusing `Full` for a layout toggle (the original A3)
would block that meaning later.

### Files touched (revised Part A)

| File | Change |
|---|---|
| `Source/MyCommander/Navigation.h`/`.cpp` | `PanelLayout`, `ComputePanelLayout`, `FormatOtherPanelLabel`; `HitTestPanel` takes the layout |
| `Source/MyCommander/main.cpp` | `gSinglePanelLayout`; `DrawFrame` (one or two boxes, the border label); `HandleMouseEvent` (the three geometry users); the `Ctrl+F1` case; Session load/save of the flag |
| `Source/MyCommander/KeyMap.h`/`.cpp` | New command id and `Commands()` row `single-panel` |
| `Source/MyCommander/MenuBar.h` | View → "Single panel" (P) |
| `Source/MyCommander/Session.h`/`.cpp` | `singlePanel` field, parsed and written (D6) |
| `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md` | `Ctrl+F1` and a short "Single-panel layout" section |
| `Source/MyCommanderTests/NavigationTests.cpp`, `SessionTests.cpp` (+ the existing KeyMap/inventory test) | New cases |
| `AIPrompt/my-commander.md`, `CLAUDE.md`, `Version.h`, changelog | UI-013 Partial → Implemented, history; version 1.26.0 |

### Tests

- **`NavigationTests.cpp`:**
  - dual-pane geometry identical to today's formula for widths 60–300 (the regression guard for D1);
  - single-panel, left or right active: one full-width area (`width - 2`), no divider;
  - `HitTestPanel` in single-panel mode: every in-bounds X hits the shown side, never the hidden one;
  - `FormatOtherPanelLabel`: fits, cut from the left with "...", CJK width, too narrow → empty.
- **`SessionTests.cpp`:** the flag round-trips; an old session file without it means dual-pane; a bad value is
  ignored.
- **`KeyMapTests`:** the existing table-vs-inventory test covers the new row; add one case that `Ctrl+F1`
  translates to `single-panel` and can be rebound.
- **Manual:**
  - `Ctrl+F1` and the menu item;
  - `Tab`, `Alt+F2`;
  - F5 to the hidden panel;
  - a dialog opened in single-panel mode keeps the single-panel background;
  - FTP and the tree in full width;
  - the mouse;
  - a restart keeps the layout;
  - the minimum 60×15 terminal.

### Phasing

1. `ComputePanelLayout` + `FormatOtherPanelLabel` + Navigation tests; `HitTestPanel` and `DrawFrame` switched to
   the layout with dual-pane output unchanged (full test suite green before the feature is visible).
2. The flag, `Ctrl+F1`/KeyMap row, the menu item, the border label, the mouse paths, Session persistence and
   their tests.
3. Docs, F1 help, spec (UI-013 → Implemented), CLAUDE.md, version 1.26.0, changelog.

### Open questions

1. **Key:** `Ctrl+F1` (free today, rebindable in `keys.conf`)? Or another key?
2. **Menu:** a new View → "Single panel" item, leaving `Full`/`List` for detailed/brief listings later
   (recommended)? Or turn `Full` into the toggle as originally planned?
3. **Persistence:** remember the layout across restarts in the Session file (recommended)? Or reset to dual-pane
   on every start (the original plan)?
4. **Hidden-panel label** on the bottom border, ` Other panel: <path> ` (recommended)? Or nothing?

---

*Everything below is the original 2026-10-06 plan. For Part A, the revised plan above supersedes it wherever
they differ.*

## Prior art already in the codebase

- `Source/MyCommander/MenuBar.h:134-136` — the View menu already has three stubs: `stub(L"Full", L'F')`,
  `stub(L"List", L'L')`, `stub(L"Tree", L'T')`, left over from FEA-0005 ("most requested menu items name
  features MyCommander doesn't have yet... a tree view... the request itself says wiring those up is not
  part of this ask"). This plan proposes `Full` → Part A's single-panel toggle, `Tree` → Part B's directory
  tree, both converted from `stub(...)` to `real(...)` the same way `AssignFileToApp`/`ToggleColorMode`
  converted their own stubs (FEA-0009/FEA-0005). `List` is left untouched — it doesn't obviously correspond
  to anything UI-013 asks for; flagged in Open items in case you intended something for it too.
- `Source/MyCommander/Panel.h:29-39` (`IPanelContent`) and `Source/MyCommander/main.cpp` — FEA-0002 Phase 3's
  generic "a panel's content can be swapped out" mechanism. `DrawPanel` (`main.cpp:473-478`) already renders
  through `panel.Content()->Draw(...)` when installed, with no path/volume line, and the main key-dispatch
  loop (`main.cpp:3165-3168`) already routes every key but `Tab`/`F10`/`F1` straight to
  `panel.Content()->OnKey(...)` and `continue`s, skipping the rest of the switch. Both are generic over any
  `IPanelContent*` — nothing plugin-specific — so a *native* (non-plugin) content implementation needs zero
  changes to either. `ZipPlugin`'s `ArchiveView` is the existing precedent for "replace one panel's content,
  leave its real path/history untouched, install/release symmetrically."
- `Source/MyCommander/Navigation.cpp:34-48` (`HitTestPanel`) and `Source/MyCommander/main.cpp:669-671`
  (`DrawFrame`) independently compute `leftWidth = consoleWidth / 2` / `rightX = leftWidth + 1` — already a
  documented "must stay in exact agreement" duplication (`main.cpp:587-588`'s own comment). Part A adds a
  third variant of this math (single-panel) and takes the opportunity to deduplicate all of it into one
  pure function both call, rather than growing the duplication to three copies.

## Part A — Single-panel layout

### What exists today

`DrawFrame` (`main.cpp:645`) always draws two boxes, each `width/2`-ish wide, and always calls `DrawPanel`
twice. `HitTestPanel` (`Navigation.cpp:34`) always treats the screen as two fixed halves. `ComputeVisibleRows`
(`main.cpp:788`) depends only on *vertical* reserved rows (hint bar, status, command line, menu bar) — not on
how the width is split — so it needs no change at all.

### Design

**A1 — One new pure geometry function, shared by `DrawFrame` and `HitTestPanel`.**

New in `Navigation.h`/`.cpp` (the existing home of `HitTestPanel`'s own geometry math):

```cpp
struct PanelLayoutGeometry {
    bool showLeft = true;
    bool showRight = true;
    SHORT leftX = 1, leftWidth = 0;    // meaningful only if showLeft
    SHORT rightX = 0, rightWidth = 0;  // meaningful only if showRight
};
// Dual-pane (today's math) when singlePanel is false. When true, only the
// active side is shown, full width (minus the two outer border columns).
PanelLayoutGeometry ComputePanelLayoutGeometry(SHORT consoleWidth, bool leftActive, bool singlePanel);
```

`DrawFrame` calls this once, uses `showLeft`/`showRight` to decide which borders/junctions and which
`DrawPanel` call(s) to draw (single-panel: a plain full-width box, no middle divider or top/bottom
junction glyph). `HitTestPanel` calls the same function and just checks which reported rect (if shown) the
mouse `x` falls in, replacing its own hand-written `leftWidth`/`rightX` math.

**A2 — New runtime state: `bool singlePanelLayout`, a `wmain` local.**

Same shape as `commandLineCursor`/`leftActive` — a plain local threaded into `DrawFrame` as a new
*defaulted* trailing parameter (`bool singlePanelLayout = false`), the exact technique IS-0015 Part 3 used
for `commandLine`/`menuState`/`commandLineCursor`. Only the *one* real call site in the main loop passes the
live value; the ~40 other `DrawFrame(console, left, right, leftActive)` calls (dialog "redraw background"
lambdas throughout `main.cpp`) keep using the default (`false`, dual-pane) — see Risks.

**A3 — Toggle key and menu item.**

Recommended: **`Ctrl+F1`** (unclaimed — confirmed against `BuiltinShortcutInventory` in
`PluginManagerLogic.cpp`, which lists `F1` alone and `Alt+F1` but no `Ctrl+F1`). Flips `singlePanelLayout`.
`MenuBar.h`'s `stub(L"Full", L'F')` becomes `real(L"Full", L'F', VK_F1, /*ctrl=*/true)`, reusing
`SimulateKeyPress` like every other menu item with an existing key, no new `MenuItemKind`.

**A4 — Interactions that need no new code, worth calling out.**

- `Tab` already just flips `leftActive`; in single-panel mode this becomes "switch which panel is shown,"
  correct for free once `DrawFrame`/`HitTestPanel` read `leftActive` through `ComputePanelLayoutGeometry`.
- `F5`/`F6` copy/move "to the other panel's directory" keep working unchanged — the hidden panel's real
  `Path()` is untouched, only not drawn. This also means a user can single-panel-browse a source directory
  and still copy/move to wherever the other (hidden) panel was last pointed.
- `BuiltinShortcutInventory` needs the new `Ctrl+F1` entry (so a loaded plugin can't claim it later and
  silently break this toggle — same reasoning every other app-level shortcut there already has).

**A5 — Persistence: not persisted (recommended).** Resets to dual-pane on every launch, like every other
transient view toggle in this app (hidden/system visibility is the one exception, and that's `NAV-010`'s own
explicit, already-decided default-off preference — not a close enough precedent to extend to this without
being asked). See Open items if you'd rather it stuck via `Config`.

### Files touched (Part A)

| File | Change |
|---|---|
| `Source/MyCommander/Navigation.h`/`.cpp` | New `PanelLayoutGeometry`/`ComputePanelLayoutGeometry`. `HitTestPanel` rewritten to use it. |
| `Source/MyCommander/main.cpp` | `singlePanelLayout` local; `DrawFrame` gains the new defaulted parameter and uses the geometry function; new `Ctrl+F1` case; the one real `DrawFrame` call site updated. |
| `Source/MyCommander/PluginManagerLogic.cpp` | `BuiltinShortcutInventory` gains `Ctrl+F1`. |
| `Source/MyCommander/MenuBar.h` | `Full` stub → real item. |
| `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md` | New key documented. |
| `AIPrompt/my-commander.md` | `UI-013` wording/status (shared with Part B, see below). |

### Test plan (Part A)

Unit (`NavigationTests.cpp`, pure):

1. Dual-pane (`singlePanel=false`): both shown, widths/positions match today's `leftWidth=width/2`,
   `rightX=leftWidth+1` exactly (a regression guard for the dedup in A1).
2. Single-panel, left active: `showLeft=true`, `showRight=false`, `leftWidth` spans the full width minus
   the two border columns.
3. Single-panel, right active: mirrors case 2 on the right side.
4. At `kMinWidth` (60): both modes still produce a positive width (no underflow).
5. `HitTestPanel` with `singlePanel=true`: every in-bounds `mouseX` resolves to the one shown side; the
   hidden side never matches.

Manual: toggle `Ctrl+F1` and the `Full` menu item in the running app; confirm `Tab` switches which panel is
shown; confirm `F5`/`F6` to the hidden panel's directory still works; confirm a dialog opened while in
single-panel mode shows dual-pane behind it (expected, see Risks) and single-panel mode returns after.

## Part B — Directory-tree layout

### What exists today

Nothing walks a directory tree today except `Search.h`'s one-shot recursive filename search (which doesn't
retain structure) and `Panel`'s own flat, one-level `std::filesystem` listing. `IPanelContent` (see Prior
art) is the mechanism this plugs into.

### Design decision: content-swap vs. a third screen region (recommendation: content-swap)

UI-013 lists "a directory-tree layout" alongside "a single-panel layout" as peer alternatives to dual-pane,
which could be read as a *third screen arrangement* — a tree pane and a listing pane shown side by side at
once, replacing the current left/right-listing split with a left-tree/right-listing split. That reading is
viable but considerably larger: new screen geometry (a third variant in `ComputePanelLayoutGeometry`), a new
widget needing its own mouse hit-testing region, and no reuse of `IPanelContent` at all.

**Recommended instead:** implement the tree as a native `IPanelContent`, installed on *one panel* exactly
the way `ZipPlugin` installs its archive browser — the screen stays an ordinary dual-pane (or single-panel,
composing with Part A for free) frame; only one panel's *content* changes, from a flat listing to a tree.
Picking a directory releases the content and returns that panel to an ordinary listing there, so the other,
untouched panel is immediately available as a copy/move destination without ever leaving the familiar
two-panel frame. This reuses `DrawPanel`'s and the key-dispatch loop's existing, already-generic
content-routing (`main.cpp:473-478`, `:3165-3168`) with **zero changes to either** — the entire risk and
effort is contained to the new tree module itself. Flagged prominently in Open items — say so if you meant
the third-screen-region reading instead; this plan does not build that.

### Design

**B1 — Pure tree model, `DirectoryTree.{h,cpp}` (new, `Console`-free, TST-004 precedent: tested against a
real `TempDir`, not a mocked filesystem, matching `FileOpsTests`/`PanelTests`/`SearchTests`).**

```cpp
struct TreeDirEntry { std::wstring name; bool isReparsePoint = false; };
// Direct subdirectories of `dir` only, sorted by name. A reparse-point
// subdirectory is listed (SEC-006: never silently hidden) but marked, so
// the tree never auto-expands/descends through one (Expand() below still
// lets the user open it explicitly).
std::vector<TreeDirEntry> ListSubdirectories(const std::filesystem::path& dir);

// The chain of directories from `root` down to `target` inclusive (just
// {root} if target isn't under root) -- used to auto-expand the tree down
// to the panel's current directory when it's first shown, Explorer-style.
std::vector<std::filesystem::path> ExpansionChain(const std::filesystem::path& root,
                                                   const std::filesystem::path& target);

struct TreeNode {
    std::filesystem::path path;
    int depth = 0;
    bool expanded = false;
    bool childrenLoaded = false;
    bool loadFailed = false;      // LIST-006-style: enumeration failed (permissions, broken junction)
    bool isReparsePoint = false;
    std::vector<size_t> childIndices;
    size_t parentIndex = SIZE_MAX;
};

// Owns the flat node storage, cursor, and scroll offset. Expand/Collapse
// lazily call ListSubdirectories on first expand only (LIST-008/PER-003
// precedent: never more I/O than what's visibly asked for).
class DirectoryTree {
public:
    explicit DirectoryTree(std::filesystem::path root, const std::filesystem::path& initialSelection);
    void Expand(size_t nodeIndex);
    void Collapse(size_t nodeIndex);
    void MoveCursor(int delta, int visibleRows);  // clamps, scrolls, matches Panel::MoveCursor's shape
    void PageMove(int delta, int visibleRows);
    void CursorHome();
    void CursorEnd(int visibleRows);
    const std::filesystem::path& CursorPath() const;
    std::vector<size_t> VisibleNodeIndices() const;  // flattens expanded subtree into on-screen order
    const TreeNode& NodeAt(size_t index) const;
    int Cursor() const;
    int TopIndex() const;
};
```

**B2 — Thin `IPanelContent` wrapper, same file or a small `DirectoryTreeContent` class in `main.cpp`.**

Owns a `DirectoryTree` plus a reference to the `Panel&` it's installed on (and the *other* panel, for
`SyncOtherPanelIfSamePath` reuse — not actually needed here since this panel itself is the one navigating,
but kept for symmetry/future-proofing... actually not needed at all: dropped, see B4). `Draw` renders each
visible node as `indent + glyph + name`, cursor row highlighted with the exact same formula `DrawPanel`
already uses for a normal row (`main.cpp`'s `(attr & kColorMask) | FOREGROUND_INTENSITY | (active ?
BACKGROUND_BLUE : 0)`), so a tree's cursor looks identical to a listing's. Expand/collapse glyph: `▸`/`▾`
(UI-014/ACC-006 precedent: `+`/`-` under `Config::useBasicSymbols`).

**B3 — Keys (`OnKey`).**

| Key | Action |
|---|---|
| `Up` / `Down` | Move cursor one visible node. |
| `Page Up` / `Page Down` | Move cursor one screen. |
| `Home` / `End` | First / last visible node. |
| `Right` | Expand the cursor node (lazy-loads children on first expand). |
| `Left` | Collapse the cursor node if expanded; otherwise move the cursor to its parent (standard tree-view convention, e.g. Explorer's own). |
| `Enter` | Navigate this panel to the cursor node's directory and release the tree (`SetContent(nullptr)`), returning to an ordinary listing there. |
| `Esc` | Release the tree without navigating — this panel's directory is whatever it was before entering tree mode (never touched). |
| mouse click | Move cursor to the clicked row; double-click same as `Enter`. |

**B4 — Entering/leaving.**

Entering (`Ctrl+T`, recommended, or the `Tree` menu item — both convert the stub to `real(..., 'T',
/*ctrl=*/true)`): guarded on `target.Content() == nullptr` (don't stomp a plugin's content, e.g. an open ZIP
archive — pressing `Ctrl+T` there does nothing, matching this app's existing "no code path silently
overrides another feature's state" discipline). Root = the filesystem root of the panel's *current* path
(`PathUtil.h`'s existing root/`IsFilesystemRoot` helpers — a drive root or UNC share root, same concept
`IsFilesystemRoot` already recognizes), auto-expanded down to, and cursored on, the panel's current
directory via `ExpansionChain`. `main.cpp` owns one `DirectoryTreeContent` per side (`leftTree`/`rightTree`
locals in `wmain`, parallel to `left`/`right` themselves), constructed fresh each time `Ctrl+T` is pressed —
no state survives between tree sessions, matching how a normal panel's cursor/selection isn't persisted
either.

Leaving via `Enter` calls `panel.NavigateTo(chosenPath)` then `panel.SetContent(nullptr)` — the exact
install/release symmetry `ZipPlugin`'s own root-level `LeaveOrUp` already established, minus any
dirty-state confirmation (a tree has nothing to lose, so no "leave?" prompt is needed, unlike `ZIP-010`'s
save/discard one).

**B5 — Hint bar: left generically stale, matching today's ZIP precedent (recommended).**

`GetHintContext` (`main.cpp:375`) doesn't special-case installed content *at all* today — while a ZIP
archive is open, the bottom hint bar already just reflects whatever the panel's real, hidden listing
happens to compute, not anything archive-specific. This plan does the same for consistency and to avoid
extending `IPanelContent` with a new virtual method for a P3/LATER feature. Flagged in Open items as a
nice-to-have if you want tree-specific hints (`Right/Left Expand/Collapse  Enter Go here  Esc Cancel`)
instead — doable, but needs either a small `IPanelContent::OwnHintContext()` optional virtual or a
`main.cpp`-local special case keyed on the tree's own pointer.

**B6 — Scope limits (explicit, to keep a P3/LATER feature bounded).**

No drive switching inside the tree (use `Alt+F1`/`F2` on an ordinary listing first, then `Ctrl+T`); no
multi-select or file operations from inside the tree (directories only, one action: navigate); no
filesystem-change watching while open (consistent with `LIST-007` being postponed project-wide already); no
filtering/search within the tree.

### Files touched (Part B)

| File | Change |
|---|---|
| `Source/MyCommander/DirectoryTree.{h,cpp}` (new) | `ListSubdirectories`, `ExpansionChain`, `TreeNode`, `DirectoryTree`. Pure aside from the real filesystem calls. |
| `Source/MyCommanderTests/DirectoryTreeTests.cpp` (new) | Unit tests (below), using `TestFixtures.h`'s `TempDir`. |
| `Source/MyCommander/main.cpp` | `DirectoryTreeContent` (wraps `DirectoryTree` as `IPanelContent`); `leftTree`/`rightTree` locals; `Ctrl+T` case (guarded on no existing content); menu wiring. |
| `Source/MyCommander/PluginManagerLogic.cpp` | `BuiltinShortcutInventory` gains `Ctrl+T`. |
| `Source/MyCommander/MenuBar.h` | `Tree` stub → real item. |
| `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md` | New key and behavior documented. |
| `Source/MyCommander/MyCommander.vcxproj` (+ `.filters`), `Source/MyCommanderTests/MyCommanderTests.vcxproj` | Add the new files, same registration pattern as `HexDump`. |
| `AIPrompt/my-commander.md` | `UI-013` wording/status (shared with Part A). |

### Test plan (Part B)

Unit (`DirectoryTreeTests.cpp`, against a real `TempDir`):

1. `ListSubdirectories`: only directories, not files; sorted by name; empty for a leaf directory; a
   junction/reparse-point child is listed and marked, not silently hidden or auto-descended.
2. `ExpansionChain`: target under root at depth 3 returns the 4-entry chain (root..target); target equal to
   root returns `{root}`; target not under root returns `{root}` only.
3. `DirectoryTree` construction auto-expands and cursors exactly the `ExpansionChain` path.
4. `Expand`/`Collapse`: lazy-loads children exactly once (a second `Expand` doesn't re-scan the filesystem —
   assert via a modified-since check or a call counter in the test, matching `LIST-008`'s own "loads once"
   testing shape); collapsing hides descendants from `VisibleNodeIndices()` without discarding them (a
   re-expand doesn't need to re-scan).
5. `Collapse` on an already-collapsed/leaf node moves the cursor to its parent.
6. `MoveCursor`/`PageMove`/`CursorHome`/`CursorEnd`: clamp correctly at both ends; scrolling (`TopIndex`)
   follows the cursor the same way `Panel::MoveCursor` already does.
7. A directory that fails to enumerate (permissions) sets `loadFailed`, doesn't crash, and `Expand` is a
   harmless no-op afterward.
8. Zero subdirectories anywhere: tree is just the root node, `VisibleNodeIndices()` has one entry.

Manual (no interactive capture in this project, same limitation as every other UI feature here): enter via
`Ctrl+T` and the `Tree` menu item; expand/collapse with `Left`/`Right`; navigate with `Enter`; cancel with
`Esc`; confirm `Ctrl+T` is a no-op on a panel already showing a ZIP archive; confirm the other panel is
unaffected and usable as an `F5`/`F6` destination throughout.

## Shared files touched (both parts)

| File | Change |
|---|---|
| `AIPrompt/my-commander.md` | `UI-013` → `IN-MVP`/`Implemented` (or split into two rows if you'd rather track the parts separately — see Open items), new version-history paragraph. |
| `Source/MyCommander/Version.h` | MINOR bump (two new, additive capabilities). |
| `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md` | Entry per part/phase, real timestamp. |

## Risks and notes

- **Dialogs behind single-panel mode revert to dual-pane.** `DrawFrame`'s new parameter is a default that
  only the one real top-level call site overrides (A2) — the same tradeoff already accepted for
  `commandLine`/`menuState`/`commandLineCursor` (e.g. the command line's own text already vanishes behind
  every modal dialog today). Threading it through all ~40 "redraw background" lambdas is disproportionate
  effort for a cosmetic P3 nicety; not attempted here.
- **Geometry deduplication (A1)** changes `HitTestPanel`'s implementation, not just adds to it — its
  existing dual-pane behavior must be provably unchanged (test 1 in Part A's plan exists specifically to
  pin that down before anything else relies on the new shared function).
- **Lazy loading is per-directory, not per-subtree** — expanding a node with 10,000 subdirectories still
  lists all 10,000 at once (one `ListSubdirectories` call), unlike `Panel`'s own `LIST-008` *incremental
  batch* loading of a single directory's *files*. A directory with that many immediate subdirectories is
  pathological rather than a realistic case this feature needs to optimize for; flagged, not solved.
- **No tree-specific hint bar (B5)** is a deliberate scope-matching decision, not an oversight — see B5.
- **`Ctrl+F1`/`Ctrl+T` are this plan's own choice**, not reused from any other file manager's documented
  default (unlike, say, `Shift+F8` permanent-delete, which does mirror a real Explorer/Commander
  convention) — easy to rebind later if you'd rather different keys.

## Phasing

- **Part A, Phase 1:** `ComputePanelLayoutGeometry` + `NavigationTests.cpp` cases. Standalone build/test.
- **Part A, Phase 2:** `DrawFrame`/`HitTestPanel` wired to it; `singlePanelLayout` state; `Ctrl+F1`; menu item.
- **Part A, Phase 3:** Docs, spec, version, changelog.
- **Part B, Phase 1:** `DirectoryTree.{h,cpp}` + `DirectoryTreeTests.cpp`. Standalone build/test.
- **Part B, Phase 2:** `DirectoryTreeContent` (`IPanelContent`); `Ctrl+T`; menu item; entry/exit guards.
- **Part B, Phase 3:** Docs, spec, version, changelog.

Parts are independent and can be implemented/landed in either order, or one without the other, exactly like
IS-0015's own Part 1/2/3 split in this project's history.

## Open items

1. **The single biggest decision:** confirm the content-swap design for Part B (one panel's content becomes
   a tree) rather than a third screen region showing a tree and a listing side by side at once. The latter
   is viable but meaningfully larger (new screen geometry, no `IPanelContent` reuse) — say so if that's what
   you actually want and this plan will be revised before implementation.
2. **Toggle keys:** `Ctrl+F1` (single-panel) and `Ctrl+T` (tree) are both free today but are this plan's own
   choice, not a copied convention. Confirm or propose alternatives.
3. **`View` menu stub mapping:** `Full` → single-panel, `Tree` → directory tree, `List` left untouched.
   Confirm, especially `List` — say what (if anything) you want it to do.
4. **Persistence:** both parts default to session-only (reset every launch). Say so if either should persist
   via a new `Config` preference instead.
5. **Reparse-point directories in the tree** are shown (marked) but never auto-expanded/descended by
   default — matching `SEC-006`. Confirm, or say if they should be hidden entirely instead.
6. **Hint bar (B5):** generically stale while the tree is shown, matching today's ZIP-browsing precedent.
   Say so if you want tree-specific hint text instead (a small, doable addition, not included by default).
7. **Requirement tracking:** one `UI-013` row covering both parts (as today), or split tracking per part.
