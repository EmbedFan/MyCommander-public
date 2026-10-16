# Fix Plan — IS-0019 (stepping up a level doesn't select the folder you came from)

Source: `AIPrompt/issue_fix_requests.md`:

> File panel — When we step into a directory then step up one level, not the directory selected what we step out.

**Expected (Total Commander behavior):** after `C:\Work` → Enter on `Projects` → `..` / `Backspace`, the cursor
in `C:\Work` is on **`Projects`**. Today it is on `..`.

## Root cause

There are two separate defects. The first is the reported bug; the second makes the fix look broken in a large
folder.

### 1. Going up never asks for the folder it left (`Panel.cpp`)

- `Panel::GoToParent()` (Backspace, and Enter on `..` via `EnterSelected`) calls
  `NavigateTo(path_.parent_path())` **without a `selectName`**.
- `SetLocation` then has no pending name, so `Refresh()`/`BeginRefresh()` falls back to "keep the cursor on
  the entry it was on". That entry is **`..`**, from the child listing, and the parent listing has a `..` too,
  so the cursor lands on `..` (index 0).
- `NavigateBack()` (`Alt+←`) and `NavigateForward()` pass `L""` the same way. So does any other jump to an
  ancestor: a typed `..` or parent path in the command line (NAV-003), or a hotlist entry that is an ancestor.

### 2. A cursor restored far down the list can be off-screen (`Panel.cpp`)

- After a refresh, `Refresh()`/`FinishRefresh()` set `topIndex_ = min(topIndex_, cursor_)`. That only scrolls
  **up** to the cursor, never down.
- In a parent with, say, 200 folders and the left one at position 150, the cursor would be placed correctly but
  the view would stay at the top. The selected row isn't visible until the first key press, because
  `MoveCursor` clamps it then.
- This already affects other selected-name jumps: IS-0001's new file/folder, search results' Enter (SRC-002),
  and compare's Enter (SRC-006).

## Fix

### F1 — Select the child we came from, for every move to an ancestor

- **A new pure helper** in `PathUtil.h`:

  ```cpp
  // The first path component of `current` below `ancestor`, or empty when
  // `current` isn't strictly inside `ancestor` (lexical, case-insensitive).
  std::wstring ChildNameToward(const std::filesystem::path& ancestor, const std::filesystem::path& current);
  ```

  It works on normalized lexical paths and ignores a trailing separator. It handles drive roots (`C:\` →
  `C:\Work` gives `Work`) and UNC share roots (`\\srv\share` → `\\srv\share\a\b` gives `a`), consistent with
  `IsFilesystemRoot`.
- **`Panel::SetLocation(dir, selectName)`:** when `selectName` is empty, it uses
  `ChildNameToward(dir, path_)` (computed **before** `path_` changes). That one place covers:
  - `GoToParent` (Backspace, Enter on `..`);
  - `NavigateBack` / `NavigateForward` when the destination is an ancestor. `Alt+←` after stepping into a
    folder now lands on that folder;
  - `NavigateTo` an ancestor from the command line, the hotlist or the drive picker.

  An explicit `selectName` from a caller (search results, IS-0001) still wins.
- **Going up several levels at once** (e.g. typing `C:\` while in `C:\Work\Projects\App`) selects the top-level
  folder on the way, `Work`. That's the same rule, and it matches TC.

### F2 — Name matching is case-insensitive for the restored cursor

`Refresh()`/`FinishRefresh()` compare `e.name == name` exactly. A path typed or pasted as `c:\work\projects`
would leave a name spelled `projects` that doesn't match the real `Projects`. The restore switches to
`CompareStringOrdinal(..., TRUE)`, the same rule as everywhere else (NTFS is case-insensitive). The match prefers
an exact spelling when both exist (a case-sensitive folder).

### F3 — Scroll the restored cursor into view

- `Panel` remembers the last `visibleRows` it was given (`lastVisibleRows_`, set by
  `MoveCursor`/`ToggleCursorSelection`/`ClampCursor`).
- After placing the cursor, `Refresh()` and `FinishRefresh()` call `ClampCursor(lastVisibleRows_)` when it is
  known. If the cursor was outside the view, the view scrolls so the cursor shows on the **middle** row, like
  TC, rather than the bottom row. If it was already visible, nothing moves.
- **No `main.cpp` change.** As a side effect this also fixes the off-screen cursor for the IS-0001, SRC-002 and
  SRC-006 jumps.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/PathUtil.h` | `ChildNameToward` |
| `Source/MyCommander/Panel.{h,cpp}` | `SetLocation` uses it when no name is given; case-insensitive restore; `lastVisibleRows_` + scroll-into-view after a refresh |
| `Source/MyCommanderTests/PathUtilTests.cpp`, `PanelTests.cpp` | Tests below |
| `AIPrompt/my-commander.md`, `README.md`/`USERGUIDE.md` (one sentence each), `Version.h`, changelog | Spec history note (IS-0019), 1.21.0 → 1.21.1 |

## Tests

**`PathUtilTests.cpp`** (`ChildNameToward`):

- `C:\Work` ⟵ `C:\Work\Projects` gives `Projects`; ⟵ `C:\Work\Projects\App` gives `Projects`;
- the drive root `C:\` ⟵ `C:\Work` gives `Work`;
- a UNC share root ⟵ a subfolder gives its first component;
- differing case (`c:\work` vs `C:\Work\Projects`) still gives `Projects`;
- a trailing separator is ignored;
- not an ancestor (a sibling, the same path, a different drive) gives empty.

**`PanelTests.cpp`** (`TempDir`):

- **Enter then up:** `GoToParent` from `root\b` puts the cursor on `b`, not `..`. Tested with several folders
  so `b` isn't index 1 by accident, and in both synchronous and incremental-loading panels (`PumpRefresh` until
  done).
- **Enter on `..`:** the same result.
- **Back:** `NavigateBack` after `NavigateTo(root\b)` puts the cursor on `b`. `NavigateForward` keeps today's
  behavior (`..` / the first entry, because the destination is a child).
- **Explicit name:** `NavigateTo(root, L"a.txt")` still selects `a.txt`.
- **Several levels up:** `NavigateTo(root)` from `root\b\c` selects `b`.
- **Case:** a typed lower-case path still selects the folder.
- **Scrolling:** 100 folders, after `MoveCursor(…, 10)` (so the panel knows 10 rows), entering folder 80 and
  going up gives `Cursor() == 80`, and `TopIndex()` is such that row 80 is visible (`TopIndex() <= 80 <
  TopIndex() + 10`).

## Out of scope / notes

- **Leaving a ZIP archive** (plugin panel) back to its folder is the plugin's path. Whether it reselects the
  `.zip` file is checked while implementing. If it doesn't, the fix belongs in the plugin's leave-archive code,
  not here, and gets reported separately.
- **The tree view** (`Ctrl+T`) already opens on the current folder (IS-0017) and isn't affected.

## Status

Implemented 2026-10-08 (version 1.21.1), F1–F3 as planned.

- **Scrolling:** `Panel::PlaceCursorOn` handles both cursor restores (synchronous `Refresh` and incremental
  `FinishRefresh`).
  - If the cursor is above the view, the view starts at it, as before.
  - If it is below the visible rows, the view centers it.
- **Leaving a ZIP archive** needed no change. The panel's own listing and cursor stay untouched underneath plugin
  content (`SetContent`), so after leaving, the cursor is still on the `.zip`.
- **Tests:** `PathUtilTests` (+3) and `PanelTests` (+3, synchronous and incremental). 704 cases, 0 failed
  (Debug).
