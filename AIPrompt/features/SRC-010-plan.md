# Implementation Plan — SRC-010 (find the largest files)

Source: a new requirement, proposed here. Not in the spec, the feature-request file or any earlier plan. Part of the
"find" family with SRC-011 (recently changed) and SRC-012 (by attribute); this plan creates the shared search extension
those two reuse.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-010 (proposed) | The application shall list the N largest files below a chosen folder, largest first, and let the user go to one. | P3 | IN-MVP | Not Started |

## Summary

Disk-space cleanup starts with "what is taking the room". Used Disk Area (FOP-020) totals a folder but cannot say which
files are responsible. This adds **Commands → Largest files**: a form, then a ranked list.

- **Form:** folder (default: the active panel's folder), *Number of files* (default 100, 1–10 000), *Minimum size* (default
  0), *Include subfolders* (on), *Include hidden and system files* (off), name mask (default `*`).
- **Result:** the N largest matching files, largest first, each with its logical size, modified date and path. Ties are
  ordered by path so the list is stable.
- **Actions in the list:** `Enter` shows the file in the active panel, `F3` views it. No delete from this list; it is a
  read-only report, so a mistaken key cannot remove anything.
- **Size used:** the logical file size (what Explorer's Size shows). Allocated size is an open question.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Recursive walk with cancel poll, progress and SEC-006 reparse rule | `Search.{h,cpp}` `SearchFiles` (SRC-001/002) | The traversal |
| Results screen with navigate-on-Enter and F3 | `main.cpp` `DoSearch` results loop | The list UI (with a size column added) |
| Form with text, number-free checkboxes and choice fields | `Dialog.h` `ShowFormDialog` | The options form (the number field is a form Text field, parsed) |
| Wildcard name matching | `PathUtil.h` `WildcardMatch` | The name mask |
| `FindFirstFileExW` listing already returns size and attributes | `DiskUsage.cpp` / `Search.cpp` | No extra calls per file |

## Design

### D1 — Extend the search result (shared with SRC-011 and SRC-012)

- `SearchResult` gains `sizeBytes`, `lastWriteUtc` and `attributes`, filled from the listing data the walk already has.
  No extra filesystem call per file.
- `SearchOptions` gains an optional `EntryFilter` predicate over those fields, applied before a result is kept, so every
  search mode shares one filtering point. The existing name and content paths are unchanged when no filter is set.

### D2 — Bounded top-N collection (pure, `Search.cpp`)

- `SearchLargest(root, count, options, cancelPoll, onProgress)` keeps a **min-heap of at most N files**: a new file
  replaces the smallest kept file only when it is larger. Memory is O(N), not O(files in the tree).
- The result is sorted descending by size, then by path.
- The pure helper `OfferTopN(heap, candidate, N)` is separate from the walk, so it is unit-tested directly.
- Reparse points are never entered (SEC-006) and never ranked (their reported size is not a real file size).
- Unreadable folders are counted, not reported.

### D3 — Form and results (`main.cpp`)

- `DoLargestFiles`: refuses provider and plugin panels (local only in v1), shows the form, runs the scan with the
  existing progress line and Esc/`P` (FOP-015), then the results list (reusing `DoSearch`'s result loop; the header
  reads "Largest files: n of N").
- Number parsing: a Text field, trimmed, 1..10 000; out-of-range or non-numeric input re-shows the form with the value
  kept (the `DoSearch` "back to the form" pattern).

### D4 — Command and key

- `CommandId::LargestFiles` / `largest-files`, menu item under **Commands**. Default key: `Ctrl+F12` (checked free in the
  key table; open question 2 asks for confirmation). Rebindable, reserved against plugins.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Search.{h,cpp}` | `SearchResult` fields, `EntryFilter`, `OfferTopN`, `SearchLargest` |
| `Source/MyCommander/Strings.h` | Form labels, title, header and refusal strings |
| `Source/MyCommander/main.cpp` | `DoLargestFiles`, results rows with size |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `Help.cpp` | Command, key, menu item, F1 row |
| `Source/MyCommanderTests/SearchTests.cpp` | Tests below |
| `Source/MyCommanderTests/KeyMapTests.cpp` | The new default key |

## Tests

- **`OfferTopN` (pure):** keeps the N largest; a tie at the cut-off keeps the lexically smaller path; N larger than the
  input returns everything; N = 1; zero-size files.
- **`SearchLargest` on real `TempDir` trees:**
  - the ranking matches the sizes written;
  - memory bound: N = 10 over 2 000 files keeps 10 entries (checked through the result count and a peak-size probe);
  - reparse point (junction, via `test::CreateJunction`) not entered and not ranked;
  - hidden files excluded unless asked; minimum size respected;
  - `cancelPoll` returns `cancelled` with the partial top-N;
  - unreadable subfolder counted.
- **Form parsing:** the number field accepts 1..10 000 and rejects the rest (pure helper, tested in `SearchTests.cpp`).
- **Manual:** a tree with a few large files, a junction and a locked file; check the ranking against Explorer.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **SRC-010** (IN-MVP, Implemented at implementation time); version bump (minor);
  version history paragraph.
- `README.md`: keybindings table row (`Ctrl+F12`); a "Largest files (SRC-010)" section; the Commands menu list.
- `USERGUIDE.md`: keybindings table row; a short "Finding large files" section.
- `Source/MyCommander/Help.cpp`: F1 row for `Ctrl+F12`.
- `CLAUDE.md`: `Search.{h,cpp}` description (the top-N walk and the extended result); Testing list (`SearchTests.cpp` covers
  `SearchLargest`).
- `AIPrompt/ManualTests.md`: new MT entry (next free number) for the manual check above.
- `AIPrompt/features/SRC-010-plan.md`: status and any differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.
- `Source/MyCommander/Version.h`: bumped by the build (no manual edit).

## Phasing

1. `EntryFilter`, extended results, `OfferTopN` and `SearchLargest` with tests. 2. Form, results rows, refusal rules.
3. Command, key, menu, F1, docs.

## Risks

- **Long scans on large volumes:** mitigated by the existing progress line, Esc and pause; the heap keeps memory flat.
- **Logical versus allocated size:** a compressed or sparse file can look bigger than the space it takes. The list says
  "logical size" in its header so nobody mistakes it for disk usage.
- **Reparse points:** never followed, so a large file behind a junction is not reported. Stated in the help text.

## Open questions

1. **Default N:** 100 (recommended).
2. **Key:** `Ctrl+F12` (recommended, checked free), or menu only.
3. **Allocated size option:** later, or in v1? Recommended: later.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.38.0) with the recommended answers: N defaults to 100, the key is
`Ctrl+F12`, and allocated size is left for later.

Differences from the plan:

- **`SearchResult` gained `sizeBytes` only.** The plan also listed `lastWriteUtc` and `attributes`, and an `EntryFilter`
  predicate. SRC-010 needs only the size, so the time and attribute fields and the predicate move to SRC-011 and SRC-012,
  which need them. The hidden/system check is a lazy attribute read for candidates only, as designed.
- **`filesSearched` counts files examined** in ranking mode (the field's content-search meaning is unchanged).
- **The top-N logic lives in `Search.cpp`'s walk** (`largestCount`), not in a separate `SearchLargest` function, so the
  walk, the cancel poll and the progress callback are shared with the other search modes.
- **The results list is its own small loop** in `DoLargestFiles`, not the `DoSearch` loop, because that loop is built
  around content results and its own header; the keys match (Enter, F3, Esc/F10).
