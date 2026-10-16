# Implementation Plan — SRC-011 (find recently changed files)

Source: a new requirement, proposed here. Depends on SRC-010 phase 1 (the extended `SearchResult` and `EntryFilter`).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-011 (proposed) | The application shall list the files and folders below a chosen folder whose modification time falls in a chosen window, newest first, and let the user go to one. | P3 | IN-MVP | Not Started |

## Summary

"What did I touch yesterday?" and "what changed in this project since Monday?" are common questions. Explorer can sort
by date but cannot filter a tree by a window. This adds **Commands → Recently changed**.

- **Form:** folder (default: the active panel's folder); *Changed in the last* N (number) with a unit choice (hours, days,
  weeks); *or changed since* a date typed as `YYYY-MM-DD` (when filled, it overrides the window); *Include subfolders*
  (on); *Include hidden and system* (off); *What to list*: files, folders or both (default both); name mask (`*`).
- **Result:** newest modification first, ties by path; each row shows the local modification time, size and path.
- **Actions:** `Enter` shows the entry in the active panel, `F3` views a file. Read-only, like SRC-010.
- **Meaning of "changed":** the last-write time (the modification time Explorer shows). The creation and metadata-change
  times are not used, because Windows does not expose a metadata-change time to a plain listing.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Extended `SearchResult` (`lastWriteUtc`, `sizeBytes`, `attributes`) and `EntryFilter` | SRC-010 D1 | The time filter, without extra calls |
| Results screen | `main.cpp` `DoSearch` results loop | The list (time column instead of the content line) |
| Local-time to UTC conversion for a date | `main.cpp` `LocalDateTimeFromFileTime` (MULTI-RENAME) / Win32 `SystemTimeToFileTime` | Turning the typed date and window into a cut-off |
| FILETIME formatting | `main.cpp` `FormatModifiedTime` | The row's time column |

## Design

### D1 — Pure cut-off computation (`Search.cpp` or a small helper beside it)

- `CutoffFor(now, window)` for a window in hours, days or weeks, and `CutoffForDate(localDate)` for `YYYY-MM-DD`
  (local midnight converted to UTC). Both are pure, so they are tested with fixed times.
- Parsing the window number: 1..100 000 for hours, 1..10 000 for days or weeks; a date must be a real calendar date
  (`2026-02-30` is rejected). Invalid input re-shows the form with the values kept.

### D2 — The search (`Search.cpp`)

- `SearchRecent(root, cutoff, options, cancelPoll, onProgress)` is `SearchFiles` with an `EntryFilter` that keeps entries
  whose `lastWriteUtc >= cutoff` and whose kind matches *What to list*.
- Results are sorted newest first. The walk is bounded by the existing `maxResults` (5 000) with the usual `truncated`
  flag.
- Reparse points are not entered (SEC-006) and are listed only as entries if they match.
- A file or folder whose time cannot be read is not reported as recent (its `lastWriteUtc` is 0 and the cut-off is
  always later than 0).

### D3 — Form, results, command

- `DoRecentChanges` in `main.cpp`, same shape as `DoLargestFiles` (SRC-010 D3): local panels only, progress and Esc/`P`,
  the shared results loop with a "Changed" column.
- `CommandId::RecentChanges` / `recent-changes`, menu item under **Commands**. Key: none by default (menu only), to keep
  the key table free for the four other new features; open question 2.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Search.{h,cpp}` | `CutoffFor`, `CutoffForDate`, `SearchRecent` |
| `Source/MyCommander/Strings.h` | Form labels, unit names, errors |
| `Source/MyCommander/main.cpp` | `DoRecentChanges`, results rows |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `Help.cpp` | Command and menu item (no default key) |
| `Source/MyCommanderTests/SearchTests.cpp` | Tests below |
| `Source/MyCommanderTests/KeyMapTests.cpp` | Inventory check for the new command |

## Tests

- **Cut-offs (pure, fixed `now`):** hours, days and weeks subtract correctly across a DST change; `YYYY-MM-DD` becomes
  local midnight; `2026-02-30`, `2026-13-01` and `2026-1-5` are rejected; a window of 0 is rejected.
- **`SearchRecent` on real `TempDir` trees:** files with times set through `SetFileTime` at known offsets, so a window
  returns exactly the expected set; order is newest first; ties by path.
- **Filters:** *What to list* (files only, folders only, both); the mask; hidden excluded unless asked.
- **Reparse point:** a junction (`test::CreateJunction`) is not entered.
- **Cancel and cap:** a cancelled walk reports `cancelled`; the cap sets `truncated`.
- **Manual:** touch a few files in a project folder, run with "last 2 hours", check against Explorer.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **SRC-011**; version bump; history paragraph.
- `README.md`: a "Recently changed (SRC-011)" section; the Commands menu list.
- `USERGUIDE.md`: a short "Finding recently changed files" section.
- `Source/MyCommander/Help.cpp`: none (menu only), unless a key is added later.
- `CLAUDE.md`: `Search.{h,cpp}` description (`SearchRecent`, the cut-off helpers); Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/SRC-011-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. Cut-off helpers and their tests (pure). 2. `SearchRecent` and tests (needs SRC-010 phase 1). 3. Form, results,
command, menu, docs.

## Risks

- **Time zones and DST:** a date or window computed in local time must convert correctly; covered by the fixed-time
  tests with a DST boundary.
- **Clock changes:** a file with a future timestamp (copied from a machine with a wrong clock) appears as "recent" for
  any window. The list shows the time so the user can see it.

## Open questions

1. **"Changed" meaning:** last-write time only (recommended), or also creation time?
2. **Key:** menu only (recommended for now), or a free key later.

## Status

Planned 2026-10-15. **Postponed 2026-10-16** by the user's decision; not started. The plan stays valid. Before
implementing: SRC-010 is done but added only `SearchResult::sizeBytes`, so this feature (or SRC-012, whichever comes first)
adds `lastWriteUtc`, `attributes` and the `EntryFilter` predicate described in SRC-010's plan.
