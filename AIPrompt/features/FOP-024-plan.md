# Implementation Plan — FOP-024 (secure delete)

Source: a new requirement, proposed here. Related to FOP-008 (permanent delete) but deliberately stronger, and deliberately
limited: a file manager cannot guarantee erasure on every storage device, and the plan says so in the product.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| FOP-024 (proposed) | The application shall offer a secure delete that overwrites a file's content before removing it, only for files it can overwrite in place, after a confirmation that states what it cannot guarantee. | P3 | IN-MVP | Not Started |

## Summary

A permanent delete removes the directory entry; the old content stays on disk until reused. Secure delete overwrites the
content first. Its value depends on the storage, so the plan limits it:

- **Menu only** (**File → Secure delete**), no key, so it cannot be hit by accident.
- **Confirmation** that names the selection and states, in plain words, what it does not cover (see *Limits*). It cannot
  be skipped by configuration.
- **Process per file:** rename to a random name in the same folder, overwrite the content with a zero pass and a random
  pass, flush to disk, then delete permanently. The random rename removes the original name from the folder's directory
  entry before the delete.

### Limits (shown in the confirmation, the README and the USERGUIDE)

- **SSDs and flash storage** remap writes, so an overwrite may not reach the old physical blocks.
- **Journaling and copy-on-write file systems** (NTFS journal, ReFS, many Linux file systems) can keep old data.
- **Shadow copies, backups, cloud sync, and Recycle Bin copies** made before the delete are not touched.
- **Other programs' caches and temporary copies** (an editor's backup, a viewer's temporary file) are not searched.
- The reliable answer for confidential data is full-disk encryption (BitLocker), which the confirmation mentions.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Permanent delete with the read-only clearing and the elevated retry | `FileOps.cpp` `DeleteItems` (permanent), SEC-003 | The final delete step |
| Per-item progress, Esc cancel and error handling | `FileOps.h` handlers, `ProgressUi` | The overwrite passes |
| Recursive walk for folders | `DiskUsage.cpp` / `Search.cpp` walk pattern (no reparse descent) | Folder selections |
| Atomic-ish rename within a folder | `FileOps.cpp` `RenameItem` | The random-name step |
| Confirmation dialog | `Dialog.h` `ShowChoiceDialog` | The mandatory confirmation |

## Design

### D1 — Which files are allowed (pure, `SecureDelete.{h,cpp}`)

- `CheckSecureDelete(entry)` refuses, with a reason, any item that is:
  - a reparse point (junction, symlink): the link is removed, its target is never overwritten (SEC-006);
  - compressed or sparse (an in-place overwrite does not reach all the stored data);
  - encrypted (EFS): overwriting through the file system does not reliably erase the encrypted blocks;
  - read-only: allowed only after the same clearing the permanent delete does, with its own confirmation line.
- Folders are walked recursively, never through reparse points; each file goes through `CheckSecureDelete`; a refused file
  is reported in the outcome and left in place, and its folder is kept.

### D2 — The overwrite (`SecureDelete.cpp`, Win32 I/O)

- Open with `FILE_FLAG_WRITE_THROUGH`, write **zeros**, then **random bytes** (`BCryptGenRandom`, the CNG use already in
  the app), `FlushFileBuffers` after each pass, then close.
- Chunks of 64 KB (the cancel and progress granularity, as FileOps' copy).
- The random rename happens first (same folder, a random 16-character name), so the original name is not left in the
  directory table after the delete.
- Esc during a pass stops; the file is reported as **partially overwritten and not deleted** (never silently claimed as
  erased). Its name is restored if the rename happened and the delete did not.

### D3 — Command and flow (`main.cpp`)

- `DoSecureDelete`: local panels only; refuses while a background job uses the items (FOP-014); shows the confirmation
  with the count and the limits; runs the per-file procedure with progress and Esc; reports each failure through the
  shared `ReportOutcome`; refreshes the panel.
- `CommandId`: none (menu only). **File → Secure delete** menu item.
- Logging: paths and outcomes only (ERR-005).

### D4 — Interaction with other features

- Not undoable (FOP-022 lists it as not undoable).
- Not offered for archive, server or plugin panels.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/SecureDelete.{h,cpp}` (new) | `CheckSecureDelete` (pure), the overwrite procedure |
| `Source/MyCommander/main.cpp` | `DoSecureDelete`, the menu handler |
| `Source/MyCommander/MenuBar.h` | File menu item |
| `Source/MyCommander/Strings.h` | Confirmation text, limits, refusals |
| `Source/MyCommander/Help.cpp` | F1 entry (menu item), limits in one line |
| `Source/MyCommanderTests/SecureDeleteTests.cpp` (new) | Tests below |
| Both `.vcxproj` files | New source and test files; `bcrypt.lib` is already linked (CheckSum.cpp) |

## Tests

- **`CheckSecureDelete` (pure):** each refusal (reparse point, compressed, sparse, encrypted); a read-only file needs the
  clearing; a plain file is allowed.
- **Overwrite on `TempDir`:**
  - after the procedure the original name is gone and the file is deleted;
  - a test hook reads the file's pre-delete bytes through a second handle during the pass and confirms they changed (the
    zero pass ran before the random pass, checked by sampling the first chunk after each pass via the hook);
  - the folder walk does not enter a junction (`test::CreateJunction`) and removes the link only;
  - cancel mid-pass reports "partially overwritten" and keeps the file under its restored name.
- **Confirmation text:** the limits lines are present (a string test on the strings constant).
- **Manual:** one small file on a normal disk; a folder with a junction; a compressed file (refused); a read-only file.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **FOP-024** (with the limits in the row text); version bump; history paragraph;
  FOP-008's note that secure delete exists and its limits.
- `README.md`: a "Secure delete (FOP-024)" section with the limits list, verbatim from the confirmation; the File menu list.
- `USERGUIDE.md`: the same section in plain words, including the advice to use full-disk encryption.
- `Source/MyCommander/Help.cpp`: F1 entry with the one-line limits.
- `CLAUDE.md`: `SecureDelete` in the source layout; the rule that it is menu-only and never offered for providers;
  Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/FOP-024-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `CheckSecureDelete` and tests (pure). 2. The overwrite procedure and its tests on `TempDir`. 3. Command, confirmation,
menu, docs.

## Risks

- **False sense of security:** the largest risk. Mitigations: a confirmation that lists the limits and cannot be skipped,
  the README and USERGUIDE sections, and no key binding.
- **Partial erasure after a cancel or crash:** the file is reported and left in place under its own name, never removed
  half-erased without a message.
- **Disk wear on SSDs:** each secure delete writes the file's size twice more; stated in the help.

## Open questions

1. **Number of passes:** two (zero, random; recommended) or a configurable count?
2. **Folders:** included in v1 (recommended, with the walk rule), or files only?
3. **Read-only files:** allowed after the clearing (recommended), or refused?

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.41.0, version 2.5.0): `SecureDelete.{h,cpp}`,
`SecureDeleteTests.cpp`, `File > Secure delete`, F1 row, docs, MT-058.

Open questions answered with the recommendations: two passes (zero, random), folders included (walk rule), read-only files
allowed after clearing.

Differences from the plan: the F1 row is listed under the file operations as a menu entry; the pass hook `afterPass` is a
member of `SecureHooks` rather than a global test hook; a folder is removed only when it ends up empty (a refused file keeps
its folder); the confirmation is a plain Yes/No dialog whose lines are the limits, not a separate screen.
