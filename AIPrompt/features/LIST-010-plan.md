# Implementation Plan — LIST-010 (folder sizes in the panel)

Source: a new requirement, proposed here. Builds on DiskUsage.cpp (FOP-020) and on the panel's Size column (UI-005).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| LIST-010 (proposed) | The panel may show the total size of each folder in the Size column, computed in the background, cached, and shown only when the user has asked for it. | P3 | LATER | Not Started |

## Summary

Explorer's Size column is blank for a folder. Total Commander can show folder sizes in its panel. This adds an opt-in
column value: a folder's recursive size, computed in the background for the rows on screen.

- **Setting:** `folderSizes = off | on` in `mycommander.ini`, default **off** (the cost of reading a large tree is
  real, so it is not on for everyone).
- **Display:** when on, a folder's Size cell shows its total once known, `…` while computing, `?` if part of the tree
  could not be read. Off keeps today's `<DIR>` marker.
- **Scope:** visible rows only (PER-003): a folder far down a 100 000-entry listing is not measured until it scrolls
  into view.
- **Sorting:** unchanged. Folders stay grouped and ordered by name, as now, so a sort never waits on a background scan.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Recursive size walk: logical and allocated, reparse points never entered, unreadable folders counted, cancel and progress | `DiskUsage.{h,cpp}` `SumTree` (FOP-020) | The measurement |
| Background thread pattern (Win32 threads, not `std::thread`) with cancel flags | `JobWorker.cpp` | The worker shape |
| Live refresh of a changed folder | `FolderWatch` / `PollAutoRefresh` (LIST-007) | Invalidating cached sizes |
| Size column and `<DIR>` marker | `main.cpp` `DrawPanel` (UI-005) | The display |
| Config key pattern | `Config.{h,cpp}` | `folderSizes` |

## Design

### D1 — The cache (`FolderSizeCache`, pure apart from the cache store)

- Keyed by the lexical path key (as `Favorites`/`Labels` do). Each entry holds the total, the unreadable-folder count,
  a state (`Pending`, `Computing`, `Done`) and the folder's last-write time when it was measured.
- A folder's entry is **stale** when its last-write time changes, or when the panel is refreshed by the user (`Ctrl+R`)
  or by an auto-refresh (LIST-007). Stale entries are recomputed when next shown; the old total stays visible meanwhile.
- The cache is per session, in memory, capped (e.g. 2 000 folders, least-recently-shown evicted). Nothing is persisted
  (sizes change, and a stale number on disk would mislead).

### D2 — The worker (`main.cpp` or `FolderSize.cpp`)

- One background thread at a time, measuring one folder at a time with `SumTree` and a cancel poll.
- The UI thread asks the cache for the rows on screen each loop pass and queues any missing or stale folders, visible
  rows first. Results are published through a mutex-guarded map the UI thread reads, like `JobWorker::Jobs()`.
- **Cancel on navigation:** leaving the folder cancels the queue (the same cancel flag pattern as jobs); the cache keeps
  whatever finished.
- **Never blocks input:** the UI thread never waits for the worker; a `…` cell is redrawn when a result arrives.
- **Provider and plugin panels:** no folder sizes (they are not local folders); the column keeps `<DIR>`.

### D3 — Display (`DrawPanel`)

- Size cell for a folder when `folderSizes = on`: the formatted total (`FormatSize` width rules), `…` while pending or
  computing, a trailing `?` when the unreadable count is non-zero.
- The column width and the `<DIR>` fallback stay as they are.

### D4 — Config

- `folderSizes = off | on` (default off), commented default-file block (IS-0006 append path), parsing and CFG-005 warning
  for other values.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/FolderSize.{h,cpp}` (new) | `FolderSizeCache` (pure key/state logic) and the worker |
| `Source/MyCommander/Config.{h,cpp}` | `folderSizes` |
| `Source/MyCommander/main.cpp` | Queueing on draw, the Size cell, cancel on navigation, cache invalidation hooks |
| `Source/MyCommander/DiskUsage.{h,cpp}` | Nothing expected; `SumTree` is used as is |
| `Source/MyCommanderTests/FolderSizeTests.cpp` (new), `ConfigTests.cpp` | Tests below |
| Both `.vcxproj` files | New source and test files |

## Tests

- **`FolderSizeCache` (pure, with an injected clock/time value):** pending → computing → done transitions; stale when the
  folder's last-write time changes; `Ctrl+R` marks stale; the cap evicts the least recently shown; a folder's key is
  lexical (case, separators).
- **The worker on real `TempDir` trees:** the total equals `SumTree`'s; cancel stops it and keeps finished folders; a
  junction is not entered; an unreadable subfolder raises the `?` count.
- **Config:** default off; `on`; a bad value warns and keeps off.
- **Manual:** a folder with a large tree; scroll a long listing and watch the cells fill; navigate away mid-scan; change a
  file inside and confirm the size updates after the auto-refresh.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **LIST-010** (LATER → IN-MVP when implemented); version bump; history paragraph.
- `README.md`: the `folderSizes` config block; a short note under the panel columns.
- `USERGUIDE.md`: the `folderSizes` row in the configuration table; a sentence on the `…` and `?` marks.
- `CLAUDE.md`: a `FolderSize` entry in the source layout; `Config` description gains `folderSizes`; Testing list.
- `Source/MyCommander/Help.cpp`: none (a setting, not a key).
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/LIST-010-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `FolderSizeCache` and tests (pure). 2. Worker and tests. 3. Display, config, navigation cancel, invalidation, docs.

## Risks

- **Disk load:** measuring many folders hits the disk. Mitigated by the default being off, visible rows only, one thread,
  and cancel on navigation.
- **Stale numbers:** a folder deep inside a tree can change without its own last-write time changing. The cache is
  refreshed on the user's own refresh and on the auto-refresh of the shown folder only; a deeper change shows the old
  total until refresh. Stated in the help.
- **Sort expectations:** a user may expect a size sort on folders. Explicitly out of scope; a separate decision.

## Open questions

1. **Default:** off (recommended).
2. **Sort by folder size:** later, once the cache exists (recommended).
3. **Cache cap:** 2 000 folders (recommended).

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.43.0, version 2.7.0): `FolderSize.{h,cpp}`,
`FolderSizeTests.cpp`, `Config::folderSizes`, the Size cell, MT-060.

Open questions: default off; no sort by folder size (later); cap 2,000 folders.

Differences from the plan:

- The worker and the cache share one file (`FolderSize.{h,cpp}`); the worker is a `CreateThread` thread guarded by a mutex.
- Staleness also uses the panel's `RefreshStarts()` (a same-folder re-list invalidates everything), not a hook in each refresh path.
- `folderSizes` accepts `on`/`off` as documented, as well as `true`/`false`/`yes`/`no`/`1`/`0`.
- The pending marker is the ASCII `...` (ACC-006), not a Unicode ellipsis.
