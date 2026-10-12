# Implementation Plan — SRC-006 (Compare directories by name, size, timestamp, or checksum)

Source: `AIPrompt/my-commander.md` §12:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-006 | The application may compare directories by name, size, timestamp, or checksum. | P2 | LATER | Not Started |

Neighbours, **not** in scope here but shaping the design:

- SRC-007: directory synchronization with preview and explicit confirmation;
- SRC-008: a text-file difference viewer, which is File > "Compare contents" and stays a stub.

Also listed in §29 ("Content search and comparison — SRC-004 to SRC-008").

## Implementation check (2026-10-08)

Nothing compares two directories today. The Selection menu's "Compare Folders" item (FEA-0005) is a stub.

| What exists | Where | Reusable for |
|---|---|---|
| Selection > "Compare Folders" (mnemonic `O`), `MenuItemKind::Stub` | `MenuBar.h` | Becomes a real item |
| Per-entry name, size, `lastWriteTimeUtc`, attributes, the `isDirectory` and `isReparsePoint` flags | `Panel.h` `Entry` | Marking the result in the panels |
| `SetEntrySelected`, `ClearSelection`, `SelectedCount` | `Panel.h` | TC-style "mark the differences" |
| Hidden/system visibility toggle (NAV-010); quick filter `Filter()` (SRC-003) | `Panel.h` | What each panel counts as "listed" |
| Recursive walk that never descends reparse points (SEC-006) and skips unreadable folders | `Search.cpp`, `PathUtil.h` | The same rules for a recursive compare |
| `\\?\` long paths, `WildcardMatch`, Win32 error text | `PathUtil.h` | Reading files for the content check |
| Throttled live progress with Esc cancel | `main.cpp` `DoSearch` (SRC-004/005) | The compare progress line |
| `ShowFormDialog` (checkbox/number/choice), `ShowListChoiceWithKeys` | `Dialog.h` | The options form and the results list |
| Free key: **Shift+F2** (today Shift+F2 falls through to plain F2 Rename); it is Total Commander's own "Compare file lists" key | `main.cpp` `case VK_F2` | The shortcut |

## Summary

**Shift+F2** (and Selection > Compare Folders) compares the **left and right panel folders**:

1. A short options form chooses *what counts as different*: name only, plus size, plus timestamp, plus content.
   It can also include subfolders.
2. The comparison runs with live progress, and Esc cancels.
3. Then, the Total Commander way, **the differences are selected in both panels**:
   - files that exist only on one side;
   - the newer file of each differing pair.

   So `F5` straight afterwards copies exactly what's missing or newer to the other side. That's a manual
   one-way sync, until SRC-007.
4. A **results list** shows every compared item with its status, and can be filtered to show only the
   differences.

## Design

### D1 — Options form (`ShowFormDialog`, values remembered for the session)

| Field | Kind | Default | Notes |
|---|---|---|---|
| Compare by | Choice: **Name** / Name + **size** / Name + size + **timestamp** / Name + size + **content** | Name + size + timestamp | The SRC-006 criteria. Each step includes the previous ones. |
| Include subfolders | Checkbox | off | Off compares only the two listed folders' own items, like TC. On walks both trees. |
| Timestamp tolerance (seconds) | Number 0–3600 | 2 | FAT/exFAT/network shares store times with 2-second precision. Used only with "timestamp". |
| Ignore exact 1-hour difference | Checkbox | off | A DST shift or a FAT volume's local times make the same file look one hour apart. |
| Show the results list | Checkbox | on | Off means mark-and-return (pure TC behavior), with a one-line summary. |

- **"Content" compares bytes, not checksums.** The two files are read side by side in 64 KB chunks and the
  read stops at the first difference.
  - This is stronger than a checksum: no collisions.
  - It is never slower, because hashing has to read both files completely too, and an early difference ends
    the read sooner.
  - Files of different sizes are "different" without reading at all.
  - See Open question 2 if a real hash (SHA-256 via Windows CNG, `bcrypt.lib`, not third-party) is wanted
    anyway, e.g. to share with File > "Generate Check Code".
- **Refusals,** each with a message:
  - both panels show the same folder;
  - either panel shows plugin content (ZIP) or the tree view;
  - either location is inaccessible (LIST-006).

### D2 — What each side's "listing" is

The comparison reads the **filesystem directly**, not the panels' `Entry` vectors. A panel might still be
loading in batches (LIST-008), and in recursive mode subfolders are never in a panel anyway. It honors what each
panel would show:

- hidden/system entries are included only if **that** panel shows them (NAV-010);
- `..` is never an item;
- the quick filter (SRC-003) applies to top-level **files** of that panel. Folders are always kept, so a
  filtered compare still recurses;
- reparse points (junctions, symlinks) are compared as **entries** (name, plus the timestamp if asked) and never
  descended (SEC-006). They are never content-read.

Names are matched **case-insensitively** (`CompareStringOrdinal(..., TRUE)`), as NTFS does, so `Readme.md` and
`README.md` are the same item.

### D3 — Statuses (pure, a new `DirCompare.{h,cpp}`)

```cpp
enum class CompareMethod { Name, Size, Timestamp, Content };   // cumulative
struct CompareOptions { CompareMethod method = CompareMethod::Timestamp; bool recursive = false;
                        uint32_t toleranceSeconds = 2; bool ignoreOneHour = false;
                        bool includeHiddenLeft = false, includeHiddenRight = false;
                        std::wstring fileMaskLeft, fileMaskRight; size_t maxItems = 200000; };

enum class CompareStatus { Identical, LeftOnly, RightOnly, LeftNewer, RightNewer,
                           Different,      // same time (or not compared), but size or content differs
                           TypeMismatch,   // a file on one side, a folder on the other
                           Unreadable };   // the content check couldn't open/read one side

struct CompareSide { uint64_t size = 0; uint64_t lastWriteUtc = 0; uint32_t attributes = 0; bool isDirectory = false; };
struct CompareItem { std::wstring relativePath;            // L"sub\\file.txt"; top level has no separator
                     std::optional<CompareSide> left, right;
                     CompareStatus status; };

// Pure decision for one pair; no I/O. contentEqual is only consulted for Content and only when sizes match.
CompareStatus ClassifyPair(const std::optional<CompareSide>& l, const std::optional<CompareSide>& r,
                           const CompareOptions&, std::optional<bool> contentEqual);

struct CompareOutcome { std::vector<CompareItem> items; size_t filesCompared = 0, bytesRead = 0,
                        unreadable = 0; bool cancelled = false, truncated = false; };

CompareOutcome CompareDirectories(const std::filesystem::path& left, const std::filesystem::path& right,
                                  const CompareOptions&, const SearchCancelPoll& cancel,
                                  const std::function<void(size_t files, const std::wstring& current)>& progress = {});

bool FilesHaveEqualContent(const std::filesystem::path& a, const std::filesystem::path& b,
                           const SearchCancelPoll& cancel, std::wstring& error);   // 64 KB chunks
```

**Rules, in order**, for an item present on both sides:

1. File vs folder → `TypeMismatch`.
2. A folder pair:
   - non-recursive mode: `Identical`, because both exist;
   - recursive mode: `Identical` only if every descendant is `Identical`, otherwise `Different`, so it gets
     marked and the user can drill in.
3. A file pair:
   - **Name:** `Identical`.
   - **Size:** sizes differ → `Different`.
   - **Timestamp** (and Content):
     1. times beyond the tolerance → `LeftNewer`/`RightNewer`. With *ignore 1 hour*, a difference within
        3600 s ± tolerance counts as equal.
     2. otherwise, sizes differ → `Different`.
   - **Content:** same size → read both; a mismatch → `Different`, or `LeftNewer`/`RightNewer` if the times
     also differ; an unreadable side → `Unreadable`.

The output is sorted folders-first, then by name, per level, depth-first, so the list reads like a tree.

**Bounded work:**

- items are capped at 200 000 (`truncated`);
- the content read uses two 64 KB buffers, never the whole file;
- Esc is polled per entry and per chunk (PER-002), so cancelling a compare of two 10 GB files is immediate.

### D4 — Marking in the panels (the TC behavior)

The marking happens after the comparison (even if it was cancelled, it covers the items done so far), and only
for **top-level** items, which are the ones the panels show:

| Status | Left panel | Right panel |
|---|---|---|
| `LeftOnly` | selected | — |
| `RightOnly` | — | selected |
| `LeftNewer` | selected | — |
| `RightNewer` | — | selected |
| `Different` (no newer side, e.g. Name+size with equal times, or a differing folder) | selected | selected |
| `TypeMismatch`, `Unreadable` | selected | selected |
| `Identical` | — | — |

- Each panel's existing selection is **cleared first**, so the result is exactly the difference set.
- Matching to panel rows is by name, case-insensitively.
- The status line of each panel shows the summary, e.g. `Compare: 12 differ, 3 only here, 140 identical`.
- With **"Show the results list"** off, that's all, and the user is straight back in the panels.

### D5 — Results list (full screen, the same shape as the `Alt+F7` results)

- **Header:** `C:\a  ⇄  D:\b — Name+size+timestamp — 12 different, 5 left only, 3 right only, 140 identical
  (2 unreadable)`, plus `(cancelled)` or `(truncated)` when that applies.
- **Rows:** a status column, the relative path, then left and right size/time, fitted via `TextWidth.h`
  (UI-011). On a narrow terminal, the times go first, then the sizes.

  | Status | Glyph | ASCII fallback |
  |---|---|---|
  | Identical | `=` | `=` |
  | Left only | `→` | `->` |
  | Right only | `←` | `<-` |
  | Left newer | `>` | `>` |
  | Right newer | `<` | `<` |
  | Different | `≠` | `!=` |
  | Type mismatch | `!` | `!` |
  | Unreadable | `?` | `?` |

  The ASCII fallback follows ACC-006's plain-ASCII frames setting.
- **Keys:**
  - `Up`/`Down`/`PgUp`/`PgDn`/`Home`/`End` move;
  - **`Tab` cycles the view filter:** All → Differences only (the default when anything differs) → Left only →
    Right only;
  - **`Enter`** closes and moves **both** panels to that item's folder, with the cursor on it. A subfolder item
    navigates each side to the matching subfolder, so a recursive difference can be inspected. Only the
    top-level marks from D4 remain;
  - **`F3`** views the left file, **Shift+F3** views the right file (built-in viewer);
  - `Esc` closes, and the marks from D4 stay.
- **Implementation:** `ShowListChoiceWithKeys` with `Tab`/`F3`/`Shift+F3` as extra keys. The filter is
  re-applied and the list re-shown in a loop, the same pattern as NAV-007's hotlist.

### D6 — Keys, menu, docs

- **Shift+F2** opens the compare. Plain F2 and Alt+F2 are unchanged.
- Selection > "Compare Folders" becomes `real(..., VK_F2, shift)`.
- `BuiltinShortcutInventory` gains Shift+F2, so a plugin can't take it.
- The F1 help, README keys table, USERGUIDE ("Comparing folders", with an example of "compare, then F5"), and the
  hint bar's Shift row (`Shift+F2 Compare`) are updated.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/DirCompare.{h,cpp}` (new, pure) | Options, statuses, `ClassifyPair`, `CompareDirectories` (walk, matching, recursion, cap, cancel, progress), `FilesHaveEqualContent` |
| `Source/MyCommander/main.cpp` | `DoCompareDirectories`: refusals, form, progress, marking, results list, Shift+F2 dispatch |
| `Source/MyCommander/MenuBar.h`, `HintBar.h`, `PluginManagerLogic.cpp` | Real menu item, hint, reserved key |
| `Source/MyCommander/Strings.h`, `Help.cpp`, `README.md`, `USERGUIDE.md` | Texts and docs |
| `Source/MyCommanderTests/DirCompareTests.cpp` (new), `MenuBarTests.cpp` | Tests below |
| Both `.vcxproj` (+ filters) | New files |
| `AIPrompt/my-commander.md`, `CLAUDE.md`, `Version.h`, changelog | SRC-006 LATER → IN-MVP / Implemented, §29 note, history; module bullet; 1.19.1 → 1.20.0 |

## Tests

**`DirCompareTests.cpp`**, pure `ClassifyPair` (no I/O):

- each method on a size-equal/time-equal pair → `Identical`;
- a size difference → `Different` under Size, but `Identical` under Name;
- timestamps:
  - newer left/right beyond the tolerance;
  - within 2 s → equal;
  - exactly 3600 s with *ignore 1 hour* → equal, and without it → newer;
- Content:
  - equal size with `contentEqual=false` → `Different`;
  - with a newer time → `LeftNewer`;
  - with `contentEqual` absent (unreadable) → `Unreadable`;
- file vs folder → `TypeMismatch`;
- one side missing → `LeftOnly`/`RightOnly`.

**`DirCompareTests.cpp`**, with `TempDir` (real filesystem):

- **Top-level compare:**
  - only-left, only-right, identical, size-different and newer files, each with the right status;
  - names that differ only in case pair up;
  - `..` is never an item.
- **Hidden files:** a hidden file is ignored unless that side includes hidden files.
- **Mask:** the mask filters top-level files only, and folders are still listed.
- **Recursive:**
  - a difference three levels down marks the top folder `Different`;
  - an identical tree gives an all-`Identical` result;
  - `Include subfolders` off ignores the content of subfolders.
- **Junctions:** a junction is listed but never descended (SEC-006); this skips gracefully where junctions can't
  be created, the existing pattern.
- **Content:**
  - equal files → `Identical`;
  - one byte changed at offset 0, at the end, and across a 64 KB chunk boundary → `Different`;
  - different sizes → `Different` without reading (checked by `bytesRead == 0`);
  - a locked file → `Unreadable`, counted.
- **Limits:** cancel mid-content-read → `cancelled`; the cap → `truncated`.

**`MenuBarTests.cpp`:** "Compare Folders" is now a `SimulatedKey` with Shift+F2.

**Manual:**

1. Two copies of a folder, edit one file and add one on each side. `Shift+F2` marks the right files; `F5` copies
   the left marks across; a second `Shift+F2` shows everything `=`.
2. A recursive compare of a source tree, `Tab` to "Differences only", then `Enter` on a deep item.
3. A content compare of two large ISO files can be cancelled with Esc.
4. A FAT USB stick against NTFS shows no false "newer" results.

## Phasing

1. **Phase 1:** `DirCompare` `ClassifyPair`, the top-level `CompareDirectories`, and `FilesHaveEqualContent`,
   with tests.
2. **Phase 2:** recursive mode (folder roll-up, SEC-006, cap), with tests.
3. **Phase 3:** UI (Shift+F2, form, progress, marking, results list, menu, hint, reserved key) and docs, spec,
   version, changelog.

## Risks and notes

- **Timestamps are the classic false positive** (FAT 2 s rounding, DST). The tolerance and the 1-hour option
  handle them, and both are documented.
- **The marks replace the current selection** in both panels. That is intentional (TC does the same), and it is
  stated in the docs and the summary line.
- **Recursive compares of huge trees** use memory proportional to the item count, capped at 200 000.
- **Network shares:** a content compare reads both files fully when they're equal. The progress line shows
  bytes, and Esc stops it.
- **Out of scope:**
  - synchronizing or copying from the results list (SRC-007);
  - a side-by-side text diff (SRC-008, File > "Compare contents" stays a stub);
  - comparing inside ZIP archives;
  - saving or exporting the report.

## Open questions

1. **Flow:** mark in both panels **and** show the results list (planned, the list can be turned off in the
   form), or only mark, as plain TC Shift+F2 does?
2. **"Checksum":** compare bytes directly (planned: exact, and never slower), or compute a real hash (SHA-256 via
   Windows CNG) so the same code can later power File > "Generate Check Code"?
3. **Key:** Shift+F2 (planned, TC's key, currently unused), or something else?
4. **Defaults:** Name + size + timestamp, 2 s tolerance, subfolders off. Are these right?

## Status

Implemented 2026-10-08, all three phases (version 1.20.0), with the planned answers to the open questions:
mark **and** show the list (switchable), byte-by-byte content comparison, `Shift+F2`, and the planned defaults.

Changes made while implementing:

- **The file mask applies to files at every level,** not only the top level. Folders are never filtered, so a
  `*.cpp` filter on a panel compares just the `.cpp` files across the whole tree.
- **With "content", timestamps only decide the direction of a real difference.** Two byte-identical files are
  `Identical` even if their times differ; the plan's rule order would have made them "newer".
- **Only-on-one-side folders aren't expanded** in recursive mode. The folder itself is the one `->`/`<-` item,
  which keeps the list short.
- **The results list is its own full-screen loop,** like the `Alt+F7` results, rather than `ShowListChoiceWithKeys`.
  That's needed for the multi-column rows and the `Tab` view filter.
- **Status marks are plain ASCII** (`=` `->` `<-` `>` `<` `!=` `!` `?`) in every mode, so no ACC-006 fallback is
  needed.
- **No panel status-line summary.** A panel's status line shows the selection count whenever something is
  selected, which would hide it. With the list off, the summary appears in a message box instead.
- **No hint-bar entry.** Adding `Shift+F2 Compare` to the Directory hint bar broke the IS-0007 guarantee that
  every segment fits at the 60-column minimum. The key is reachable from the menu and F1.
- **`Enter` on a top-level item** moves both cursors onto it without reloading, so the marks survive. A deeper
  item navigates each panel (where that folder exists) to its parent folder, with the cursor on it.
