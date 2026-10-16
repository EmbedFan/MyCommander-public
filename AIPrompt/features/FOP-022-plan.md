# Implementation Plan — FOP-022 (undo the last file operation)

Source: a new requirement, proposed here. Not in the spec or earlier plans. Depends on FOP-023 (restore from the Recycle
Bin) for undoing a delete to the Recycle Bin.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| FOP-022 (proposed) | The application shall undo the most recent reversible file operations of the session, in reverse order, only when the affected items are still in the state the operation left them, and shall say clearly when an operation cannot be undone. | P2 | IN-MVP | Not Started |

## Summary

A mistaken rename, move or delete is the most common file-manager accident. **Ctrl+Z** reverses the last operation. This
is safe only with strict rules, so the plan is deliberately narrow.

**Undoable (in v1):**

| Operation | Undo |
|---|---|
| Rename (F2, group rename) | Rename back |
| Move (F6, synchronous) | Move back to the source folder |
| Copy (F5, Shift+F5 copy-as) | Move the created copies to the Recycle Bin, only if each is still byte-identical in size and time to what was copied |
| Delete to the Recycle Bin (F8) | Restore from the Recycle Bin (FOP-023) |
| Attribute change (Ctrl+A) | Set the attributes back |
| New folder / new empty file (F7, Shift+F7) | Remove it, only if still empty |

**Not undoable (stated, never silently):** permanent delete (Shift+F8), anything that overwrote a file, elevated
operations, background jobs (FOP-014; a later extension), archive and server (provider) operations, plugin operations,
and any operation whose items changed after it ran.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Rename, move, copy, delete, attribute engines and their outcomes | `FileOps.{h,cpp}`, `main.cpp` `ReportOutcome` | The operations to record |
| Recycle Bin delete | `FileOps.cpp` `DeleteItems` (FOP-008 path) | The delete undo's counterpart |
| Session-only state, never persisted | `Session.cpp` precedent (ERR-006) | The undo stack's lifetime |
| Busy check against background jobs | `main.cpp` `RefuseIfBusyWithJob` (FOP-014) | Refusing undo while a job uses the items |
| Confirmation dialogs | `Dialog.h` `ShowChoiceDialog` | Confirming an undo |

## Design

### D1 — The journal (`UndoJournal`, pure)

- A bounded, in-memory stack of entries (cap 20). Each entry records: the operation kind, the exact items (full paths
  before and after), and the **expected state after the operation** (size and last-write time per item, attributes for
  attribute changes, emptiness for a new folder).
- Never written to disk. Lost at exit, by design (ERR-006: nothing that could be replayed after a crash).
- A new operation that cannot be undone (permanent delete, overwrite, elevated) **clears the stack** and the status line
  says so, rather than leaving an undo that would now be out of order.

### D2 — Verify, then reverse (`UndoJournal` + `FileOps`)

- Before undoing, every item is checked against its recorded expected state. If one differs (edited, moved, deleted
  since), the undo is **refused as a whole**, with a message naming the first changed item. Nothing is partly reversed.
- Reversal uses the existing engines: rename and move back through `RenameItem`/`MoveItems`; copies out through
  `DeleteItems` (Recycle Bin); deletes through FOP-023's restore; attributes through `ToggleAttributes` with the recorded
  set; created folders and files through `DeleteItems` when still empty.
- A reversal is itself **not** pushed on the stack (no redo in v1); it is removed from the stack.

### D3 — Command and key

- `Ctrl+Z` → `CommandId::Undo` / `undo`; **File → Undo** with the operation's name ("Undo: rename a.txt"). The hint bar
  shows the next undo's name when there is one.
- With an empty stack: a short message, no beep, no change.
- Undo asks for confirmation for a delete restore and for a copy removal (the two that remove items from view); rename,
  move, attributes and empty-folder removal run after the same confirmation form, one prompt per undo.

### D4 — Recording

- Each recording call site (F2, group rename, F5/F6, F7/Shift+F7, F8, Ctrl+A) records **only after the operation
  succeeded**, from the outcome, listing only the items that actually changed. Failed and skipped items are not in the
  entry.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/UndoJournal.{h,cpp}` (new) | Entries, expected-state check, bounded stack (pure) |
| `Source/MyCommander/FileOps.{h,cpp}` | Nothing changes behaviour; outcomes already carry what is needed (verify in phase 1) |
| `Source/MyCommander/main.cpp` | Recording at each call site, `DoUndo`, the File menu item, the hint text |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `HintBar.h`, `Help.cpp`, `Strings.h` | Command, key, menu item, hint and help |
| `Source/MyCommanderTests/UndoJournalTests.cpp` (new), `KeyMapTests.cpp` | Tests below |

## Tests

- **Journal (pure):** push and pop order; the cap drops the oldest; an entry is refused when any expected state differs;
  a non-undoable operation clears the stack; a reversal removes its entry.
- **Round trips on `TempDir`:** rename then undo; move then undo; copy then undo (the copies go to the Recycle Bin; the
  test uses the fake bin from FOP-023's test seam, since a real Recycle Bin is not available to automated tests); attribute
  change then undo; new empty file then undo; a folder that gained a file is not removed.
- **Refusal:** an item edited after the operation refuses the whole undo, and nothing changes.
- **Busy check:** a background job using the items refuses undo.
- **Key:** `Ctrl+Z` → `Undo`; in the inventory.
- **Manual:** rename, move and delete a few files, undo each; edit one file after a move and confirm the refusal.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **FOP-022**; version bump; history paragraph; the "Not undoable" list in the row.
- `README.md`: keybindings row (`Ctrl+Z`); a "Undo (FOP-022)" section with the table of what is and is not undoable; the
  File menu list.
- `USERGUIDE.md`: keybindings row; the same table in plain words; the note that undo is lost at exit.
- `Source/MyCommander/Help.cpp`: F1 row for `Ctrl+Z`.
- `CLAUDE.md`: `UndoJournal` in the source layout (session-only, never persisted, the ERR-006 reasoning); Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/FOP-022-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `UndoJournal` and tests (pure). 2. Recording at the call sites, verify-and-reverse for rename, move, attributes and
empty-folder removal. 3. Copy undo and delete undo (needs FOP-023). 4. Command, key, menu, hint, docs.

## Risks

- **Undo that does the wrong thing:** the expected-state check is the whole safety story. A file edited between the
  operation and the undo is detected by size and time; an edit that keeps both is not. The UI says undo checks size and
  time, not content.
- **Recycle Bin copies:** copy undo sends the copies to the Recycle Bin, so it can be undone itself; the message says so.
- **Expectation of a full undo:** users will expect every action to undo. The refusal messages and the table in the docs
  set the scope honestly.

## Open questions

1. **Scope:** the table above (recommended), or also background jobs in v1?
2. **Undo depth:** 20 (recommended).
3. **Redo:** out of scope in v1.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.39.0 in part, completed in 1.40.0 / version 2.4.0): phases 1, 2 and 4, copy
undo, and delete undo once FOP-023 (restore from the Recycle Bin) landed. Open questions answered with the plan's recommendations (the table's scope, depth 20, no redo).

What exists: `UndoJournal.{h,cpp}` (pure; `PathFingerprint`, `CheckUndo`, `PerformUndo`), `UndoJournalTests.cpp`
(10 cases: order and cap, round trips for rename/move/copy/attributes/new folder/new file, refusal when edited or when
the original name is taken, whole-entry refusal, fingerprint), `Ctrl+Z` / `File > Undo`, recording at F2, group
rename, F5/F6/Shift+F5/drag, Ctrl+A and F7/Shift+F7 (local panels only), manual test MT-053.

Differences from the plan, and why:

- **Delete to the Recycle Bin is undoable** (`UndoKind::Delete`, added with FOP-023): items are found in the bin lazily at
  undo time by original path, a deletion time not before the operation start (2 s slack) and, for files, size; all
  items are verified before any is restored. A **permanent** delete clears the journal.
- (Earlier, before FOP-023:) delete was not undoable and cleared the journal. The clearing rule keeps
  Ctrl+Z from reversing something older than the delete. When FOP-023 lands, Delete can become a recorded kind.
- **Outcomes did not carry per-item results**, contrary to the plan's "verify in phase 1" note. `FileOps` was not
  changed; instead each call site snapshots what exists at the destination before the operation and records only what
  is new afterwards. An operation that changed something that already existed (overwrite, merge) or was interrupted
  across volumes, or any elevated step, clears the journal instead.
- **Group rename** is recorded only for a plain batch; a chain (a to b, b to c), a swap or a case-only rename clears
  the journal. **Case-only single renames** are not recorded.
- **No hint-bar text and a static menu label** ("Undo"): the confirmation dialog names the operation instead.
- **Labels follow an undone rename/move** (LIST-009), which the plan did not mention.
- A refused undo **drops** its entry (it can never become valid again), so older entries stay reachable.
- The status line shows a short transient note ("Undone: ...", "Undo list cleared ...") until the next key press.

Not verified: the interactive flow (confirmation, Recycle Bin, cursor placement) -- see MT-053.
