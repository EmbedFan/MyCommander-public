# Fix Plan — IS-0020 (Generate Check Code on directories; Brief view column width)

Source: `AIPrompt/issue_fix_requests.md`:

> File Menu/Generate Check Code
> &nbsp;&nbsp;&nbsp;&nbsp;- it shall be run on directory/directories too
>
> CTRL-F8 / brief view.
> &nbsp;&nbsp;&nbsp;&nbsp;- now just use the original filename field width, it shall be use the full panle width - 1 character.

Two unrelated defects in two MENU-STUBS features (FOP-021, UI-017), reported together under one issue number.

## Issue 1 — Generate Check Code skips directories entirely (FOP-021)

### Root cause

`DoGenerateCheckCodeForSelection` (`main.cpp`) builds its `files` list by looking up each selected name in
`target.Entries()`; when the entry `isDirectory`, it is counted in `skippedFolders` and **never added** — no
recursion happens at all. A folder in the selection contributes nothing to either output mode.

### Fix

Add a pure, SEC-006-safe recursive file lister to `CheckSum.h`/`.cpp`, following `DiskUsage.cpp`'s own
`FindFirstFileExW` walk precedent exactly (same reparse-point handling, same shape):

```cpp
// Every file under `root`, as paths relative to `root` ("sub\\file.txt"),
// found by walking its subtree. Never descends into a reparse point
// (SEC-006); a subfolder that can't be listed is skipped and counted.
struct FileListResult {
    std::vector<std::wstring> relativePaths;
    uint64_t unreadableFolders = 0;
    bool cancelled = false;
};
FileListResult EnumerateFilesRecursively(const std::filesystem::path& root,
                                         const CheckSumCancelPoll& cancelPoll = {});
```

In `DoGenerateCheckCodeForSelection`, the directory branch becomes:

```cpp
if (it != entries.end() && it->isDirectory) {
    auto listed = mc::EnumerateFilesRecursively(target.Path() / name, cancelPoll);
    for (const auto& rel : listed.relativePaths) files.push_back(name + L"\\" + rel);
    unreadableFolders += listed.unreadableFolders;
    continue;
}
files.push_back(name);
```

- **Relative names** carry the subfolder prefix (`"Photos\\2024\\a.jpg"`), so both output modes keep working
  unchanged: "one file per file" writes next to each source file (`target.Path() / relativeName` + extension —
  `std::filesystem::path::operator/` accepts an embedded-`\`-separator string directly); "one file for all"
  writes one line per file, same as today, just with more lines.
- `skippedFolders` (the "N folder(s) skipped" message) is replaced by `unreadableFolders` ("N folder(s) could
  not be read"), matching `DiskUsage`'s own wording for the equivalent case — folders are no longer skipped on
  purpose, only when they genuinely can't be listed.
- No change to the "one for all" base-name logic (still the single item's stem, or the panel folder's own name).
- No change to Verify (`DoVerifyCheckSum`) — it already re-hashes whatever relative names a checksum file lists,
  which now may include subfolder paths; `VerifyPlan` already resolves `baseDir / entry.name` and that composes
  correctly with a multi-segment `entry.name`.

## Issue 2 — Brief view ignores the available panel width (UI-017)

### Root cause

`ComputeBriefLayout` (`Navigation.cpp`) sizes each column from the **longest entry name**, capped at half the
panel width:

```cpp
columnWidth = max(12, min(longestNameWidth, width/2));
columns = width / columnWidth;
```

With ordinary (short) file names this produces several narrow columns — each about as wide as Full view's own
Name column — rather than using the space available. That's the reported symptom: the panel doesn't look
different from Full view's Name field, just repeated side by side.

### Fix

Per the issue's own wording, drop the name-length/half-width basis entirely and size a Brief-view column from
the **panel width alone**:

```cpp
BriefLayout ComputeBriefLayout(int visibleRows, SHORT width) {
    int columnWidth = std::max<int>(1, width - 1);
    int columns = std::max(1, static_cast<int>(width) / columnWidth);  // always 1 for width >= 2
    (void)visibleRows;
    return BriefLayout{columns, columnWidth};
}
```

- The `longestNameWidth` parameter is removed (no longer used) — both call sites in `main.cpp` (`DrawPanel`'s
  Brief branch, and `briefColumnsNow()`) drop the now-pointless per-entry longest-name scan.
- **Consequence, stated plainly:** with `columnWidth = width - 1`, `columns` always evaluates to `1`. Brief view
  becomes a single, full-width names column (vertically paged, same scrolling as today) rather than the
  several-narrow-columns layout the original MENU-STUBS plan described — this is a deliberate reversal of that
  earlier design, not a bug in the new formula. `BriefEntryAt`/`BriefFirstVisible` need no change; with
  `columns == 1` they already reduce to plain linear paging.
- `Left`/`Right` (today: move one column = `visibleRows` entries) become equivalent to `PageUp`/`PageDown` once
  there is only ever one column on screen. Left as-is rather than special-cased away — harmless redundancy, not
  a new bug, and keeping the code path uniform means a future "actually give me several columns on a wide
  enough panel" request has less to unwind.

### Open question

The fix above is the literal reading of "use the full panel width − 1 character" (i.e. **one** full-width
column, multi-column Brief view goes away). If several columns across a wide panel is still wanted when there's
obviously room for more than one, say so instead and I'll size columns from `max(12, width / desiredColumnCount)`
without the half-width cap, rather than forcing `columns == 1` unconditionally. Proceeding with the one-column
reading above unless told otherwise.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/CheckSum.{h,cpp}` | `EnumerateFilesRecursively` (new) |
| `Source/MyCommander/main.cpp` | `DoGenerateCheckCodeForSelection`'s directory branch; `DrawPanel`'s Brief branch and `briefColumnsNow()` drop the longest-name scan |
| `Source/MyCommander/Navigation.{h,cpp}` | `ComputeBriefLayout` signature/body (drops `longestNameWidth`) |
| `Source/MyCommanderTests/CheckSumTests.cpp` | `EnumerateFilesRecursively`: flat, nested, reparse-point-not-descended (SEC-006, skips gracefully per TST-004 if junction creation fails), unreadable-subfolder count, cancellation |
| `Source/MyCommanderTests/NavigationTests.cpp` | Replace the now-wrong multi-column `ComputeBriefLayout`/`BriefEntryAt` expectations with the single-full-width-column behavior |
| `AIPrompt/my-commander.md`, `README.md`/`USERGUIDE.md`/`Help.cpp` (wording only, "several columns" → "full width"), `Version.h`, changelog | Spec note (IS-0020), version bump |

## Status

**Issue 1 (Generate Check Code on directories): implemented 2026-10-13** (version 2.1.1 / spec 1.29.1), as
planned — `CheckSum.{h,cpp}`'s `EnumerateFilesRecursively` plus the `DoGenerateCheckCodeForSelection` directory
branch. 6 new `CheckSumTests.cpp` cases (flat, nested, empty, reparse-point-not-descended, cancel,
nonexistent-path). Full solution build succeeds (0 errors); `MyCommanderTests.exe`: 817 cases, 0 failed.

**Issue 2 (Brief view column width): explicitly declined by the user for this pass** ("Do 1, 2 shall be not
implemented") — `ComputeBriefLayout` is unchanged from how UI-017 originally shipped it. Left open in this file
for whenever it's revisited; the open question above still applies then.
