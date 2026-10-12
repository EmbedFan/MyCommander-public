# Implementation Plan — SRC-007 (Directory synchronization with preview and explicit confirmation)

Source: `AIPrompt/my-commander.md` §12:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| SRC-007 | Directory synchronization shall require preview and explicit confirmation. | P3 | LATER | Not Started |

Also listed in §29 ("Content search and comparison — SRC-004 to SRC-008"). SRC-008 (a text diff viewer) stays
out of scope.

## Implementation check (2026-10-08)

Nothing synchronizes folders today. SRC-006 (1.20.0) did most of the groundwork:

| What exists | Where | Reusable for |
|---|---|---|
| The recursive compare, with statuses (`LeftOnly`, `RightNewer`, `Different`, …) | `DirCompare.{h,cpp}` `CompareDirectories`, `ClassifyPair` | Deciding what to copy or delete |
| Its rules: case-insensitive names, reparse points never descended or read (SEC-006), each side's hidden flag and mask, the cap, cancel, progress | `DirCompare.cpp` | The same rules for a sync |
| The compare form, progress line and full-screen results list (`Tab` filter, F3 view) | `main.cpp` `DoCompareDirectories` | The shape of the preview screen |
| Copy and delete engines: conflict prompt, Retry/Skip/Skip all/Cancel, Elevate & Retry, progress, Esc, timestamps preserved by `CopyFileEx` | `FileOps` `CopyItems`, `DeleteItems` | Executing the plan, with no new copy code |
| `ProgressUi` / `AnimatedProgressDialog`, `ReportOutcome` (logs failures, ERR-004) | `main.cpp` | Progress and the final report |
| `RefuseIfBusyWithJob` (FOP-014) | `main.cpp` | Not syncing paths a background job is using |
| Free keys: **Ctrl+Shift+F2**; the free Commands menu letter `S` | `PluginManagerLogic.cpp`, `MenuBar.h` | The shortcut and menu item |

## Summary

**Commands → Synchronize Folders** (and **Ctrl+Shift+F2**) makes the two panels' folders match:

- **one way:** left → right, or right → left;
- **or both ways:** each side gets what it's missing, and the newer version of each file.

Nothing is changed until the user has:

1. **seen a preview** of every planned action, which they can also adjust item by item;
2. **confirmed** a summary dialog that spells out the copies, the overwrites and, above all, the deletions.

This confirmation is **always** shown. It isn't one of the config-controlled delete confirmations (CFG-*),
because SRC-007 makes it mandatory.

## Design

### D1 — Options form (`ShowFormDialog`, remembered for the session)

| Field | Kind | Default | Notes |
|---|---|---|---|
| Direction | Choice: **Left → Right** / **Right → Left** / **Both ways** | Left → Right | One-way is the common case: a backup or a deploy. |
| Compare by | Choice: Name + size + timestamp / Name + size + content | timestamp | "Name only" and "Name + size" are left out: they can't tell which side is newer. |
| Include subfolders | Checkbox | **on** | Unlike the compare, a sync is usually wanted for the whole tree. |
| Delete files that exist only on the target | Checkbox | **off** | **Mirror** mode. Only offered for one-way. Deletions go to the **Recycle Bin**. |
| Timestamp tolerance (seconds), Ignore exact 1-hour difference | as in SRC-006 | 2 / off | Shared with the compare form's values. |

The same refusals as the compare apply:

- either panel shows plugin content or the tree view;
- either location is inaccessible;
- both panels show the same folder;
- either root is inside the other (`IsSameOrWithin`, SEC-005). A sync into a subfolder of itself would chase its
  own copies;
- either root is in use by a background job (`RefuseIfBusyWithJob`, FOP-014).

### D2 — The plan (a new pure module `DirSync.{h,cpp}`)

```cpp
enum class SyncDirection { LeftToRight, RightToLeft, Both };
enum class SyncAction { None, CopyToRight, CopyToLeft, DeleteRight, DeleteLeft };
struct SyncOptions { SyncDirection direction; bool deleteExtras = false; };

struct SyncItem {
    CompareItem item;           // from DirCompare: relative path, both sides, status
    SyncAction action;          // what will happen (the user may change it in the preview)
    SyncAction proposed;        // what the rules proposed (shown when the user overrides it)
    bool overwrite = false;     // the copy replaces an existing file
    bool conflict = false;      // the rules couldn't decide safely -> None until the user picks
    std::wstring reason;        // short text for the preview, e.g. L"target is newer"
};

std::vector<SyncItem> BuildSyncPlan(const CompareOutcome&, const SyncOptions&);
struct SyncSummary { size_t copyRight, copyLeft, overwrite, deleteRight, deleteLeft, conflicts;
                     uint64_t bytesRight, bytesLeft; };
SyncSummary Summarize(const std::vector<SyncItem>&);
bool CanApply(const SyncItem&, SyncAction);   // e.g. no CopyToRight for an item missing on the left
```

**The rules.** "Source" is the side being copied from; in both-ways mode each item has its own.

| Status | Left → Right | Both ways |
|---|---|---|
| `LeftOnly` | copy → | copy → |
| `RightOnly` | delete on the right if *Delete extras*, else nothing | copy ← |
| `LeftNewer` | copy →, overwrite | copy →, overwrite |
| `RightNewer` | **conflict**: the target is newer; nothing until the user chooses | copy ←, overwrite |
| `Different` (no newer side) | copy →, overwrite (mirror semantics), shown as "differs" | **conflict** |
| `TypeMismatch` (a file vs a folder) | **conflict** | **conflict** |
| `Unreadable` | nothing; reason "couldn't be read" | nothing |
| `Identical` | nothing | nothing |

Right → Left is the mirror image of the first column.

- **Folders:**
  - a folder present on only one side is copied (or deleted) **as one item**, with everything in it. That's how
    `CopyItems`/`DeleteItems` already work, so the plan stays short;
  - a folder present on both sides never has an action of its own; its children carry the actions.
- **Order of execution:** all copies first, then deletions. A failed or cancelled copy therefore never leaves the
  user with something deleted and nothing copied.
- **Invariant:** the destination folder of every copy already exists. A missing parent would itself be a
  one-side folder, copied whole. A test checks this.

### D3 — The preview (the SRC-007 "preview")

A full-screen list, the same layout and keys as the compare results, plus an **Action** column:

| Action | Glyph |
|---|---|
| Copy left → right | `->` (`=>` when overwriting) |
| Copy right ← left | `<-` (`<=` when overwriting) |
| Delete on the right / left | `DEL>` / `<DEL` |
| Nothing | (blank) |
| Unresolved conflict | `??` |

- **Header:** the two roots and the direction.
- **Totals line:**
  `Copy 14 → (38.2 MB, 5 overwrite), 2 ← (1.1 MB), delete 3 on the right, 2 conflicts left as "nothing"`.
- **Keys:**
  - `↑`/`↓`/`PgUp`/`PgDn`/`Home`/`End` move;
  - **`Space`** toggles the item between its proposed action and *nothing*;
  - **`→` / `←`** force a copy in that direction (only where that side has the item, `CanApply`). This is also
    how a conflict gets resolved;
  - **`Del`** marks it for deletion on the side it would otherwise be copied to; only where the item exists on
    that side and only in a one-way direction with *Delete extras* allowed, so a deletion is never created by
    accident;
  - `Tab` filters: actions only (the default) / conflicts / all;
  - `F3` / `Shift+F3` view the left/right file;
  - **`F5`** synchronizes; `Esc` cancels, changing nothing.
- **An empty plan** ("Nothing to synchronize — the folders match") is shown as a message, not a preview.

### D4 — The explicit confirmation

`F5` opens a `ShowChoiceDialog`:

> **Synchronize folders?**
> `D:\Work  →  E:\Backup\Work`
> Copy 14 files/folders to the right (38.2 MB), **overwriting 5**
> Copy 2 to the left (1.1 MB)
> **Delete 3 on the right (to the Recycle Bin)**
> `[S]ynchronize  [B]ack to preview  [C]ancel`

- **The default button is Back,** not Synchronize, so a stray `Enter` can't start it. With any deletions or
  overwrites, the dialog lists the first five paths of each.
- **It is shown every time.** No config key skips it. This is the requirement, and it is consistent with the
  CLAUDE.md rule that only the user's own opt-out may skip a confirmation (there is none here).

### D5 — Execution

It runs in the **foreground** with the usual progress (`ProgressUi`, per-item names, bytes, Esc).

- **Copies** are grouped by (source folder, destination folder) and run through **`CopyItems`**, so retry, skip,
  elevate and timestamp preservation all come for free.
  - The conflict handler answers **Overwrite** for exactly the items the confirmed plan marked as overwrites.
  - Any other collision means something changed since the preview. The usual Overwrite/Skip/… prompt is asked
    then, never a silent overwrite.
- **Stale preview guard:** just before each group, every item's source and target are re-read. If the size or
  time differs from what the preview showed, the item is **skipped** and reported as "changed since the
  preview". The same applies to a deletion target.
- **Deletions** run through **`DeleteItems(..., permanent = false)`**, grouped per folder, **after** all copies.
  The Recycle Bin can't take a very long path; such an item fails with the normal Retry/Skip prompt, and is
  reported.
- **The result:** `ReportOutcome` with the verb "Synchronize" (failures logged, ERR-004), both panels refreshed
  (NAV-012). A follow-up `Shift+F2` compare should then show only `=`.

### D6 — Keys, menu, docs

- **Ctrl+Shift+F2** opens the sync, alongside Shift+F2's compare. It goes in `BuiltinShortcutInventory`.
- **Commands → Synchronize Folders** (mnemonic `S`, free in that menu).
- **From the compare results list (SRC-006), `F5` opens this sync form,** pre-filled with the compare's method and
  subfolder choice. That's the natural "I see the differences, now fix them" step; the compare list's hint line
  gains `F5: synchronize`.
- **Docs:**
  - F1 help, README (keys, temporary screens, a "Synchronize folders" section);
  - USERGUIDE ("Synchronizing folders" with a backup example and a two-way example, and a clear warning about
    *Delete extras*).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/DirSync.{h,cpp}` (new, pure) | `SyncOptions`, `SyncItem`, `BuildSyncPlan`, `Summarize`, `CanApply`, grouping for execution, stale check helper |
| `Source/MyCommander/main.cpp` | `DoSynchronizeDirectories`: refusals, form, compare with progress, preview, confirmation, execution, report. Compare list `F5` hook |
| `Source/MyCommander/MenuBar.h`, `PluginManagerLogic.cpp`, `Strings.h`, `Help.cpp` | Menu item, reserved key, texts, help |
| `Source/MyCommanderTests/DirSyncTests.cpp` (new), `MenuBarTests.cpp` | Tests below |
| Both `.vcxproj` (+ filters) | New files |
| `README.md`, `USERGUIDE.md`, `CLAUDE.md`, `AIPrompt/my-commander.md`, `Version.h`, changelog | Docs; SRC-007 LATER → IN-MVP / Implemented, §29 note, history; 1.20.0 → 1.21.0 |

## Tests

**`DirSyncTests.cpp`**, pure `BuildSyncPlan` / `Summarize` / `CanApply`:

- **Every row of the rule table,** for all three directions:
  - `RightNewer` in Left → Right is a conflict with no action;
  - `Different` in Both ways is a conflict.
- **Delete extras:**
  - off → `RightOnly` has no action;
  - on → `DeleteRight`;
  - never in Both ways.
- **Folders:**
  - a one-side folder is a single item;
  - a folder present on both sides never has its own action.
- **`CanApply`:**
  - no copy from a side that lacks the item;
  - no delete in Both ways;
  - no delete where the item doesn't exist.
- **Summary:** counts and bytes (folders' bytes come from their files in the compare outcome).
- **Invariant:** the destination parent of every planned copy exists on the target side.

**`DirSyncTests.cpp`**, with `TempDir` (the execution helpers):

- **Stale check:** a file touched after planning is detected.
- **Grouping:** copies grouped by folder, deletions last.

**Manual:**

1. **Backup:** `D:\Work → E:\Backup\Work` with new, changed and deleted files and *Delete extras* on. The preview
   shows `->`, `=>` and `DEL>`. The confirmation lists them, with Back as the default. After syncing, `Shift+F2`
   shows everything `=` and the deleted files are in the Recycle Bin.
2. **Both ways:** a file changed on each side. A `Different` conflict is resolved with `←` in the preview.
3. **Stale preview:** edit a file while the preview is open. It's skipped and reported as "changed since the
   preview".
4. **Cancelling:** Esc during a large copy stops it, and nothing is deleted.

## Phasing

1. **Phase 1:** `DirSync` plan, summary, `CanApply` and stale-check helpers, with tests.
2. **Phase 2:** UI: form, preview with per-item edits, confirmation, execution with progress and report,
   refreshes.
3. **Phase 3:**
   - menu, key, compare-list `F5`, help;
   - docs, spec, version, changelog.

## Risks and notes

- **Deletion is the dangerous part.** It is off by default, one-way only, and goes to the Recycle Bin. It is
  listed by name in the confirmation, runs last, and is skipped if the target changed.
- **Two-way sync can't detect deletions.** A file deleted on one side reappears from the other, because there is
  no sync history (no database of the last sync). This is documented; deletion propagation would need a state
  file and is out of scope.
- **Timestamps on FAT and network shares:** the tolerance and 1-hour options from SRC-006 apply.
  `CopyFileEx` preserves times, so a re-compare is clean.
- **Large trees:** the preview holds at most the compare's 200 000 items. A truncated compare **refuses** to
  sync ("too many items — narrow it with a filter or a subfolder"), so a partial plan can never delete or skip
  things silently.
- **Out of scope:**
  - background (queued) sync (FOP-014 jobs could carry it later);
  - saved sync profiles;
  - two-way deletion propagation;
  - permanent (non-Recycle-Bin) deletion;
  - syncing into ZIP archives;
  - the SRC-008 text diff.

## Open questions

1. **Directions:** offer all three, Left → Right / Right → Left / Both ways (planned), or one-way only at first?
2. **Mirror deletions:** include *Delete files that exist only on the target* (planned: off by default, Recycle
   Bin only), or leave deletion out entirely?
3. **`Different` with no newer side in one-way mode:** copy and overwrite, i.e. mirror semantics (planned), or
   treat it as a conflict the user must resolve?
4. **Key:** `Ctrl+Shift+F2` plus Commands → Synchronize Folders plus `F5` from the compare list (planned)?

## Status

Implemented 2026-10-08, all three phases (version 1.21.0), with the planned answers to the open questions:
all three directions, mirror deletions (off by default, Recycle Bin only), one-way "different" overwrites the
target, and the keys `Ctrl+Shift+F2`, Commands → Synchronize Folders and `F5` from the compare list.

Changes made while implementing:

- **`Del` in the preview** marks only a one-way **extra** (an item present only on the target) for deletion, and
  only with *Delete files only on the target* on. It never deletes a file that exists on both sides.
- **A stale item is reported as a failure** with the reason "changed since the preview". It is listed by
  `ReportOutcome` and logged, rather than counted as a silent skip.
- **The SRC-006 compare refactor.** `RefuseCompareOfPanels`, `RunCompareWithProgress` and `CompareSideText` were
  pulled out of `DoCompareDirectories` so compare and sync share them. `DoCompareDirectories` now returns the
  `F5` preset.
- **The confirmation lists the first five** overwrites and deletions on each side, then "… and N more". The
  full list is the preview itself.
