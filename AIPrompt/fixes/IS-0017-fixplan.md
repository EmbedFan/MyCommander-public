# Fix Plan — IS-0017

Source: `AIPrompt/issue_fix_requests.md`

> Tree view:
>     When press enter on a directory in the tree now switch mode to list. Inspite of this expand the directory.
>     When tree request on root, not tree view content shown, just a c:\ is shown.
>     The +/- shall expand dir/collapse level
>     When on a tree the View/Tree view requested the tree list the original level as tree, but when the selected
>     directory is far from the start, the selection is not shown. Required behavior is: show the directory was
>     selected before tree view starting, not just select!

All five items are defects in the directory tree added by UI-013 Part B (`DirectoryTree.{h,cpp}`,
`DirectoryTreeContent` in `main.cpp`, commit pending). They don't touch the file list or any other feature.

## Summary

Three separate problems, each with a different cause:

1. **Root shows only `C:\`** (confirmed by code). `DirectoryTree`'s constructor only expands the root
   when `ExpansionChain` has more than one entry (`DirectoryTree.cpp:82-83`). When the panel is already at
   a drive root, the chain is just `{root}`, so the root stays collapsed and the user sees a single row.
2. **`+` / `-` do nothing** (confirmed by code). `DirectoryTreeContent::OnKey` (`main.cpp:645-685`) only
   handles arrows, Page keys, Home/End, Enter, Esc and Ctrl+T. Expansion is only reachable with `Right`/`Left`.
3. **Enter both switches to the list and expands the directory** (not yet reproduced). I found no path
   where Enter expands a node: `VK_RETURN` calls `NavigateHereAndLeave()` (`main.cpp:671-672`), which
   releases the content and calls `Panel::NavigateTo`. The most likely reading is that, after the switch,
   the list shows the chosen folder's subfolders, which looks like an expansion. The other possibility is
   a real double-action (for example, an expansion happening before the release). This needs an interactive
   repro before fixing. See Part 3.

## Part 1 — Root always shows its children

### Design

In the `DirectoryTree` constructor, expand the root node unconditionally, before the `ExpansionChain` loop.
The chain loop then only needs to expand the intermediate nodes it already expands. Result: at any root, the
tree shows the root's immediate subdirectories, and the cursor still lands on the panel's current folder.

```cpp
DirectoryTree::DirectoryTree(std::filesystem::path root, const std::filesystem::path& initialSelection) {
    // ... root node created as now ...
    Expand(0);  // the root's own children are always visible, even when the cursor starts on the root
    // ... existing ExpansionChain loop, starting from i = 1 ...
}
```

The earlier design (B1) said the deepest matched node stays collapsed. That still holds for the cursor node.
Only the root is special-cased.

### Files touched

- `Source/MyCommander/DirectoryTree.cpp` (constructor, one line).
- `Source/MyCommanderTests/DirectoryTreeTests.cpp`: one new test (below).

### Test plan

- `DirectoryTree_RootAlwaysExpandedSoItsChildrenShow`: a tree built with `initialSelection == root` and two
  subdirectories has three visible nodes (root plus both children), with the cursor on the root.
- Existing `DirectoryTree_EmptyRootHasExactlyOneVisibleNode` still holds: an empty root has no children to show.

## Part 2 — `+` expands, `-` collapses the cursor folder

### Design

Map the numpad and main-keyboard keys in `DirectoryTreeContent::OnKey`:

| Key | Action |
|---|---|
| `VK_ADD` (numpad `+`), `VK_OEM_PLUS` | `ExpandCursor` (no-op if already expanded). |
| `VK_SUBTRACT` (numpad `-`), `VK_OEM_MINUS` | Collapse the cursor folder (keeps the cursor where it is). |

- `-` must not move the cursor to the parent the way `Left` does: `Left` is "collapse, or go up if already
  collapsed", and `-` is only "collapse".
- Add `DirectoryTree::CollapseCursor(int visibleRows)` for the new `-` behaviour. Re-clamp afterwards, the same
  as the other cursor operations.
- To keep the key mapping testable (TST-005 precedent), move it into a pure function in `DirectoryTree.h`:
  `enum class TreeKeyAction { None, MoveUp, MoveDown, PageUp, PageDown, Home, End, ExpandCursor, CollapseCursor,
  CollapseOrGoToParent, GoToSelected, Leave };` and `TreeKeyAction ClassifyTreeKey(WORD virtualKeyCode, bool ctrlHeld);`.
  `OnKey` then just switches on the result.

Assumption: "collapse level" means collapsing the folder under the cursor. If the user meant collapsing the
parent level instead, say so in Open items.

### Files touched

- `Source/MyCommander/DirectoryTree.h` / `.cpp`: `CollapseCursor`, `TreeKeyAction`, `ClassifyTreeKey`.
- `Source/MyCommander/main.cpp`: `DirectoryTreeContent::OnKey` uses `ClassifyTreeKey`.
- `Source/MyCommanderTests/DirectoryTreeTests.cpp`: new cases.
- `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md`: document `+` and `-`.

### Test plan

- `ClassifyTreeKey_MapsPlusAndMinusBothKeyboards`: `VK_ADD`/`VK_OEM_PLUS` → `ExpandCursor`; `VK_SUBTRACT`/`VK_OEM_MINUS` → `CollapseCursor`.
- `ClassifyTreeKey_LeftStillCollapsesOrGoesToParent`: `VK_LEFT` is unchanged.
- `ClassifyTreeKey_UnknownKeyIsNone`: a key the tree doesn't use returns `None`.
- `DirectoryTree_CollapseCursorKeepsCursorWhereItIs`: an expanded cursor folder collapses and stays selected,
  unlike `CollapseCursorOrGoToParent`, which moves to the parent once the folder is already collapsed.
- `DirectoryTree_CollapseCursorOnCollapsedNodeIsNoOp`.

## Part 3 — Enter behaviour (decided: keep current behaviour)

Decision: Enter goes to the selected folder and returns the panel to a normal list. The tree is not expanded and not kept open. No code change. The only work is to state this clearly in Help, README and USERGUIDE.

### Why this part is not yet fixed

The code path for Enter doesn't expand anything, and I can't drive the console interactively to see what
the user sees. Two readings of the report, and what each would need:

- **Reading A (most likely): the list shows the folder's subfolders.** That's `NavigateTo` working as
  designed, with the list now showing the chosen folder's children. The fix would be a clarification (tree
  Enter means "open this folder in the panel"), not a code change. Or, if the user expects the tree to stay
  expanded after Enter, that's a design change (`NavigateHereAndLeave` would then keep the tree installed
  and only update the panel).
- **Reading B: a real double action.** For example, a double-click that both expands and navigates, or the
  key reaching `DirectoryTree::ExpandCursor` before `Leave`. This would show as a tree node expanding just
  before the switch. The code doesn't do this, so it would need a specific reproduction to pin down.

### Plan

1. Ask the user which reading matches (see Open items).
2. Whichever it is, add a regression test that Enter releases content, leaves the panel's path set to the
   cursor folder, and does not change the cursor node's `expanded` flag (a wrapper-level test, if
   `DirectoryTreeContent` is moved out of `main.cpp`; otherwise a manual check).
3. If Reading B is confirmed, reproduce it in the debug build (log the key sequence), then fix it in
   `NavigateHereAndLeave` / `OnKey`.

## Part 4 — Tree opens scrolled to the selected folder

### Cause (confirmed by code)

The `DirectoryTree` constructor places the cursor on the panel's current folder, which is correct, but it
leaves the scroll position at 0 (`DirectoryTree.cpp:95`). The scroll window only moves inside
`ClampToVisible(visibleRows)`, and that is only called from cursor movement. Nothing calls it when the tree
is first drawn. So when the selected folder is more than one screen down the list, the cursor is off-screen
and the user sees the top of the tree with no selection visible.

Expanding the ancestors of the selected folder makes the list longer, and that is why this shows up for
folders far from the start.

### Design

- Add a public `DirectoryTree::ScrollToCursor(int visibleRows)` that runs the existing private
  `ClampToVisible`. `ClampToVisible` already brings the cursor's row into the window, so no new scrolling
  logic is needed.
- `DirectoryTreeContent::Draw` calls `tree_->ScrollToCursor(height)` before rendering. The call is cheap, and
  Draw is the only place the real visible height is known. Calling it every frame also keeps the cursor on
  screen after a terminal resize.

### Files touched

- `Source/MyCommander/DirectoryTree.h` / `.cpp`: `ScrollToCursor`.
- `Source/MyCommander/main.cpp`: `DirectoryTreeContent::Draw` calls it first.
- `Source/MyCommanderTests/DirectoryTreeTests.cpp`: new cases.

### Test plan

- `DirectoryTree_ScrollToCursorShowsDeepSelectionInWindow`: a folder with 30 subfolders, cursor on the 25th,
  `ScrollToCursor(10)`. Assert that `TopVisiblePosition()` is at least 16 and that the cursor's visible
  position is inside `[top, top + 10)`.
- `DirectoryTree_ScrollToCursorIsNoOpWhenAlreadyVisible`: top stays put when the cursor is already in the window.
- `DirectoryTree_ScrollToCursorAfterResizeKeepsCursorVisible`: a smaller `visibleRows` on a second call still
  brings the cursor into the window.

Manual: open the tree on a folder deep in a large directory and confirm the selection is on screen at once.

## Part 5 — Other side while a tree is open

Source line: "When a tree opened one side the other side shall list current level. On the other side the
directory changes shall not work."

### Current behaviour (from the code)

Entering the tree (`DirectoryTreeContent::Enter`, `main.cpp`) only touches the panel it is installed on. It
does not change the other panel's path or content. So the other side already lists its own current folder
while the tree is open, and that part of the request is met.

The second sentence is ambiguous, so the plan records both readings and asks which one is meant.

- **Reading 1 (dropped):** "directory changes" means the tree must not follow or redraw when the
  other side changes folder. The tree already fixes its root and cursor at Enter time, so this already holds.
  The work is a regression test only.
- **Reading 2 (adopted, narrowed by the user):** while a tree is open on one side, the other side must not
  enter a folder with Enter, and must not go to the parent with Backspace. Every other function on the other side
  (list operations and the rest of its navigation) keeps working.

### Design (Reading 2)

- A helper `TreeIsOpenOn(side)` (true when that side's `DirectoryTreeContent` is installed).
- In the main key switch, before the navigation intents run, if the *other* side has a tree open, the
  directory-changing keys above do nothing (and show a short status message such as "Close the tree to change
  folder on the other side", using the existing status-line message path).
- The pure part (which keys count as directory-changing) goes next to `ClassifyNavigationKey`, so it is
  unit-testable, matching TST-005.

### Files touched

- `Source/MyCommander/main.cpp`: the other-side check, applied to Enter-on-a-folder and Backspace only.
- `Source/MyCommander/Help.cpp`, `README.md`, `USERGUIDE.md`: say that folders on the other side can't be changed
  while a tree is open.
- `Source/MyCommanderTests/DirectoryTreeTests.cpp` or the key-classifier tests: unit tests for the pure part.

### Test plan

- Unit: Enter-on-a-folder and Backspace are the only keys the block applies to; every other key is unaffected.
- Manual: open the tree on the left. On the right, Enter on a folder and Backspace do nothing, and the status line
  says why. Sort, select, copy, move, rename and the other list operations still work there, and so do Alt+Left/Right,
  the drive picker and search-result jumps. Close the tree and Enter/Backspace work on the right again.

## Version and docs

- `Source/MyCommander/Version.h`: PATCH bump (1.14.0 → 1.14.1). These are fixes to an existing, unreleased feature.
  Part 4 is the same kind of fix, so the bump covers it too.
- `AIPrompt/my-commander.md`: `UI-013` description and a version-history entry for IS-0017.
- `Help.cpp`, `README.md`, `USERGUIDE.md`: the `+` / `-` keys added to the tree section.

## Status

Parts 1, 2 and 4 are implemented and tested (573 cases pass), but not yet committed, versioned or logged.
Part 3 is decided (keep current behaviour); its only remaining work is documentation.
Part 5 is planned with Reading 2 (the other side's folder changes are blocked while a tree is open).

## Open items

1. **Enter (resolved: keep current behaviour):** which reading is right? Does the list showing the folder's subfolders match what you expected,
   or did the tree itself expand on Enter (and if so, in which order relative to the switch)? If you want the
   tree to stay open after Enter, say so.
2. **`-` meaning:** collapse the cursor folder (the plan's assumption), or collapse the parent level?
3. **`=` key:** on a US layout `VK_OEM_PLUS` is the `=`/`+` key, so `=` will also expand. If you'd rather it
   didn't, the main-keyboard `+` can only be matched by its character, which needs an interface change to
   `IPanelContent::OnKey`. Say if that's wanted.
4. **Other side while a tree is open (resolved):** Enter on a folder and Backspace are blocked on the other side
   while a tree is open on this side. Everything else stays available. Alt+Left/Right history, the drive picker and
   search-result jumps are not blocked; say if they should be.
