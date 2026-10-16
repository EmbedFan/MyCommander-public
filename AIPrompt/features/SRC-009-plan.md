# Implementation Plan — SRC-009 (find and remove empty folders)

Source: a new requirement, proposed here. Related to SRC-001/002 (recursive search) but a different question: not "which
names match" but "which folders hold nothing".

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-009 (proposed) | The application shall find empty folders under a chosen folder, list them, take the user to one, and, only after confirmation, delete the listed folders to the Recycle Bin. | P3 | IN-MVP | Not Started |

## Summary

Old project trees, extraction leftovers and moved media collections leave chains of empty folders. This adds a
**Find empty folders** command that lists them and can clean them up safely.

- **Run:** **Commands → Find empty folders** (menu command; `Alt+Shift+F7` as the optional key). It scans below the active
  panel's folder.
- **Options form:** *Include hidden and system folders* (default off), *Treat folders that contain only empty folders as
  empty* (default on — the top of a chain is listed once), *Ignore `Thumbs.db` and `desktop.ini`* (default on).
- **Results list:** one row per empty folder, relative to the scanned root, with the number of empty subfolders under it.
  `Enter` shows it in the active panel. `Del` removes the highlighted one. `A` removes **all** listed ones. Both ask first
  and delete to the Recycle Bin (never permanently).
- **Safety:** a reparse point (junction/symlink) is never descended into, reported or deleted (SEC-006); a folder is
  re-checked at the moment it is deleted, so a file that appeared since the scan keeps its folder.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| A recursive walk that never descends a reparse point, with cancel poll and progress | `Search.cpp` `SearchFiles`, `DirCompare.cpp` | The walk's shape and the SEC-006 rule |
| `FindFirstFileExW` listing helpers with unreadable-folder counting | `DiskUsage.cpp` `SumTree` (FOP-020) | Checking "does this folder hold anything", cheaply |
| A results list with navigate-on-Enter | `main.cpp` `DoSearch` results UI (SRC-002) | The list UI and the jump |
| Delete to the Recycle Bin with progress, cancel and errors | `FileOps.h` `DeleteItems` | The removal; no new delete code |
| Refusing an operation on files a background job uses | `main.cpp` `RefuseIfBusyWithJob` (FOP-014) | Before deleting |
| Pause/cancel poll | `PeekEscapePressed` (FOP-015) | A pausable scan |
| Options form | `Dialog.h` `ShowFormDialog` | The three checkboxes |

## Design

### D1 — `FindEmptyFolders` (pure walk, `Source/MyCommander/EmptyFolders.{h,cpp}`)

```cpp
struct EmptyFolderOptions {
    bool includeHidden = false;
    bool chainsCountAsEmpty = true;
    bool ignoreJunkFiles = true;   // Thumbs.db, desktop.ini (case-insensitive)
    size_t maxResults = 5000;
};
struct EmptyFolder { std::filesystem::path path; size_t emptyChildren = 0; };
struct EmptyFoldersOutcome {
    std::vector<EmptyFolder> folders;
    bool cancelled = false, truncated = false;
    size_t unreadable = 0;        // folders that could not be listed: never reported
};
EmptyFoldersOutcome FindEmptyFolders(const std::filesystem::path& root, const EmptyFolderOptions&,
                                     const CancelPoll&, const ProgressFn& = {});
```

- A folder is **empty** when it holds no file at all (after the junk-file rule) and either has no subfolder, or — with
  `chainsCountAsEmpty` — all its subfolders are themselves empty. The reported entry is the topmost folder of such a
  chain; its `emptyChildren` counts the folders below it.
- A folder that **cannot be listed** is never reported as empty (an unreadable folder is unknown, not empty), and is
  counted in `unreadable`.
- A reparse point is never entered, and its presence makes the parent *non-empty* (it is an entry).
- The root itself is never reported.
- Hidden and system entries: when `includeHidden` is off, a hidden *file* still counts as content (a folder holding only
  a hidden file is not empty); a hidden *folder* is skipped entirely, neither entered nor reported.
- Bounded memory: the walk keeps only the current path stack and the result list (capped at `maxResults`).

### D2 — The command (`main.cpp`)

- Refuse for a panel showing plugin content or a provider (local folders only, like Disk Usage; provider support is a
  later extension through `SearchProvider`'s listing).
- Form → scan with a live "Scanning <folder>" line (rate-limited like `DoSearch`) → results list. Esc cancels the scan;
  `P` pauses it.
- **Delete** (`Del`, one; `A`, all):
  1. confirmation naming the count and "to the Recycle Bin" (a *destructive operation* — never skipped; there is no
     config opt-out, as for synchronization);
  2. `RefuseIfBusyWithJob` for the listed folders;
  3. each folder is re-verified empty immediately before `DeleteItems` (a folder that is no longer empty is skipped and
     reported);
  4. the result goes through the shared `ReportOutcome` (log, failed items), then the panels refresh.
- Deleting a chain's top folder removes the whole chain in one Recycle Bin entry.

### D3 — Menu, key, help

- `MenuItemKind::FindEmptyFolders` under **Commands**; optional `CommandId::FindEmptyFolders` with `K(VK_F7, _, S, A)`
  (inventory entry and test). Hint-bar, F1, README, USERGUIDE.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/EmptyFolders.{h,cpp}` (new) | The pure walk |
| `Source/MyCommander/Strings.h`, `MenuBar.h`, `KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `HintBar.h`, `Help.cpp`, `main.cpp` | Strings, menu, key, command UI |
| `Source/MyCommanderTests/EmptyFoldersTests.cpp` (new), `KeyMapTests.cpp` | Tests below |
| both `.vcxproj`, `README.md`, `USERGUIDE.md`, `CLAUDE.md`, spec (new SRC-009 row), `ManualTests.md`, changelog, `Version.h` | Docs |

## Tests

- **`FindEmptyFolders` (real `TempDir` trees):**
  - one empty folder is found; a folder with one file is not;
  - a chain `a\b\c` (all empty) is reported once as `a` with `emptyChildren == 2` — and, with `chainsCountAsEmpty` off,
    only `c` is reported;
  - a folder with an empty subfolder *and* a file is not empty, but the empty subfolder is reported;
  - `Thumbs.db`/`desktop.ini` are ignored when asked, and count as content when not;
  - a junction (NAV-009 helper) is neither entered nor reported, and makes its parent non-empty;
  - a hidden file keeps its folder non-empty; a hidden folder is skipped unless `includeHidden`;
  - the root is never reported;
  - an unreadable folder is counted and never reported;
  - cancel mid-walk returns `cancelled`; `maxResults` truncates.
- **Delete path:** a folder that gained a file after the scan is skipped by the re-check (using the pure re-check
  helper, tested directly); the `DeleteItems` call itself is already covered by `FileOpsTests`.
- **Manual:** a tree with several empty chains, junctions and hidden items; run, inspect, delete one, delete all, cancel;
  confirm the Recycle Bin holds the folders and a restored one reappears.

## Phasing

1. `FindEmptyFolders` and tests. 2. Command, form, results list, deletion. 3. Key, docs.

## Risks

- **The worst failure is deleting a folder that matters.** Mitigations: Recycle Bin only; a mandatory confirmation that
  shows the count; the re-check at delete time; unreadable folders and reparse points never listed; no deletion at
  all in the scan itself.
- **A folder that looks empty but isn't:** alternate data streams or files hidden from the listing by permissions.
  `skip_permission_denied`-style failures count as *unreadable* (never reported), not as empty.
- **Network shares:** a long scan over a slow share. Same cancel/pause behaviour as the other scans; the "unreachable
  location" case reports unreadable, not empty.

## Open questions

1. **Include "only junk files" folders by default?** Recommended: yes (`Thumbs.db`/`desktop.ini` ignored), with the
   checkbox to turn it off.
2. **A key, or menu only?** Recommended: menu plus `Alt+Shift+F7`, since it sits next to Find files (`Alt+F7`).
3. **Provider support** (empty folders inside an archive/server)? Recommended: later; the first version is local only.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.36.0) with the recommended answers: junk files ignored by
default, a key plus the menu, local folders only.

Differences from the plan:

- **A hidden/system folder the scan does not enter keeps its parent non-empty** (the plan only said "skipped"): otherwise
  a parent holding only a hidden subfolder with files would be listed as empty and its deletion would take the hidden
  folder along unseen. The same conservative rule applies to an unreadable child folder.
- **Re-check** is `IsFolderEmptyNow`, a public function of the module (the same `Visit` rules), run for each victim just
  before `DeleteItems`; skipped folders are logged and reported.
- **Results list** is `ShowListChoiceWithKeys` (Enter / Del / A) rather than a custom screen; the list drops folders that
  no longer exist after a deletion and stays open.
- **Menu:** `Commands > Find empty folders` (mnemonic `M`).
