# Implementation Plan — FOP-023 (browse and restore from the Recycle Bin)

Source: a new requirement, proposed here. Enables FOP-022's delete undo. Builds on FOP-008 (Recycle Bin delete).

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| FOP-023 (proposed) | The application shall list the current user's Recycle Bin items with their original locations and deletion times, and shall restore selected items to their original locations, refusing rather than overwriting. | P2 | IN-MVP | Not Started |

## Summary

Explorer's Recycle Bin window restores items, but only from its own window. A file-manager user expects to restore from
inside the manager. This adds **Commands → Restore from Recycle Bin**: a list of the bin's items, and a restore.

- **List:** name, original folder, deleted date, size, sorted newest deletion first. The bin of the current user only
  (SEC-002: no elevation, and other users' items are not shown).
- **Restore:** `Enter` restores the highlighted item; `A` restores all listed items after one confirmation; `F3` views a
  restored-or-not file's content only when it is still in the bin (view is not offered for items it cannot read).
- **Conflicts:** if the original location already holds an item of that name, the restore is **refused for that item**
  with a message ("a file with this name is already there; move or rename it first"). Nothing is overwritten, ever.
- **Missing original folder:** Windows recreates the folder on restore when it can; if it cannot, the item is reported as
  failed, not created elsewhere.
- **No bin:** a drive without a Recycle Bin (some network and removable volumes) shows a clear message.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Recycle Bin delete through the shell operation | `FileOps.cpp` (`SHFileOperationW`, FOF_ALLOWUNDO), FOP-008 | The counterpart; the bin is the same store |
| Per-item outcome and failure reporting | `FileOps.h` `OperationOutcome`, `main.cpp` `ReportOutcome` | Reporting restores |
| A list screen with keys | `Dialog.h` `ShowListChoiceWithKeys` (NAV-007) | The bin list |
| Confirmation | `Dialog.h` `ShowChoiceDialog` | Restore-all |

## Design

### D1 — Reading the bin (`RecycleBin.{h,cpp}`, the only shell-folder code)

- A narrow interface, `IRecycleBin`: `List()` returns `RecycleBinItem { displayName, originalPath, deletedUtc, sizeBytes,
  shellItemHandle }`, and `Restore(item)` restores one item. The real implementation uses the Recycle Bin shell folder
  (`SHGetDesktopFolder` → the bin's `IShellFolder2`), finds the "Original Location" and "Date Deleted" columns **by name at
  runtime** (their index differs between Windows versions), and restores through the item's `undelete` verb.
- The interface lets every test use a fake bin. Only `RecycleBin.cpp` includes the shell headers; the pure parts
  (`FormatRecycleBinRow`, the conflict check) take plain strings.

### D2 — Restore rules (pure)

- `CheckRestore(item, existingNames)`: refuses when the original full path exists (case-insensitive), or when the parent
  folder is missing and could not be recreated.
- Restores run one item at a time through the verb; each result is reported. A restore that fails leaves the item in the
  bin.

### D3 — Screen and command (`main.cpp`)

- `DoRestoreFromBin`: the list, `Enter`/`A`/`Del`-not-offered (no deleting from the bin in v1, to keep the screen read-only
  apart from restore), `Esc` closes. After a restore, the affected panels refresh (the same `SyncOtherPanelIfSamePath`
  pattern).
- `CommandId::RestoreFromBin` / `restore-from-bin`, menu item under **Commands** (no default key in v1; open question 2).
- Refused for provider and plugin panels, since the bin is local (the command works from any panel and restores to the
  original locations, so the panel's own content is irrelevant).

### D4 — Interaction with FOP-022

- FOP-022 calls `IRecycleBin::Restore` for a delete undo. The undo verifies first that the item is still in the bin and
  still matches its recorded size and deletion time; otherwise it refuses.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/RecycleBin.{h,cpp}` (new) | `IRecycleBin`, the shell implementation, `CheckRestore`, row formatting |
| `Source/MyCommander/main.cpp` | `DoRestoreFromBin` |
| `Source/MyCommander/Strings.h` | Titles, columns, refusals |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `Help.cpp` | Command and menu item |
| Both `.vcxproj` files | New source and test files; `shell32` and `ole32` already linked by the app? (verify in phase 1) |
| `Source/MyCommanderTests/RecycleBinTests.cpp` (new), `KeyMapTests.cpp` | Tests below |

## Tests

- **Pure:** `CheckRestore` refuses an existing name (case-insensitive) and a missing parent it cannot recreate; the row
  formatter (date, size, original location, long names, Unicode names).
- **With the fake bin:** list order; restore-all stops at the first refusal only if asked (default: continue and report
  each); a failed restore keeps the item listed; the screen refreshes the panels.
- **Real bin (opt-in, `MC_RECYCLE_TEST=1`):** delete a `TempDir` file to the bin with `DeleteItems`, list it, restore it,
  check it is back; skipped by default because it changes the user's real bin.
- **Manual:** delete several files, restore one, restore all, restore onto an existing name (refused), on a network
  drive without a bin (message).

## Documentation updates

- `AIPrompt/my-commander.md`: new row **FOP-023**; version bump; history paragraph; a note on FOP-008 that restore is now
  possible from inside the app.
- `README.md`: a "Restore from the Recycle Bin (FOP-023)" section (what it lists, refusals, read-only apart from restore);
  the Commands menu list.
- `USERGUIDE.md`: a short section with the same rules in plain words.
- `Source/MyCommander/Help.cpp`: none (menu only).
- `CLAUDE.md`: `RecycleBin` in the source layout (the only shell-folder code; `IRecycleBin` seam); the "only Win32 clipboard
  call" rule does not change; Testing list (opt-in real-bin test).
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/FOP-023-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `CheckRestore` and the row formatter (pure) with tests. 2. `IRecycleBin` with the fake and the shell implementation.
3. Screen, command, menu, docs. 4. FOP-022 integration.

## Risks

- **Undocumented column indexes:** solved by looking the columns up by name at runtime, and by a test that fails loudly
  if the names are not found, so a Windows update cannot silently show wrong locations.
- **Shell-folder COM lifetime:** every interface released in a scope guard; the test with the real bin catches leaks in
  a manual run.
- **Items from other users or from a deleted account:** not shown (the bin of the current user only); stated in the help.

## Open questions

1. **Delete from the bin (permanently):** not in v1 (recommended). Later, with a confirmation.
2. **Key:** menu only in v1 (recommended).
3. **Restore-all default:** continue and report (recommended), or stop at the first refusal.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.40.0, version 2.4.0): `RecycleBin.{h,cpp}`, `RecycleBinTests.cpp`,
`Commands > Restore from Recycle Bin`, and FOP-022's delete undo.

Differences from the plan: **menu only** (no `CommandId`/key, as proposed); no delete-from-bin in v1; `A` restores all
after one confirmation and continues past failures, reporting each; original name is the display name plus the `$R`
extension when the shell hid it; the real bin is only *listed* by the opt-in test (`MC_RECYCLE_TEST=1`), never
modified.
