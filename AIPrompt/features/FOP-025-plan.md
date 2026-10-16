# Implementation Plan — FOP-025 (paste files copied or cut in Explorer)

Source: a new requirement, proposed here. Uses the existing copy and move engine (FOP-005/FOP-010) and the conflict
prompts. Changes one project rule (CON-004 wording in CLAUDE.md), stated below.

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| FOP-025 (proposed) | The user shall be able to paste files that were copied or cut in Windows Explorer into the active folder, as a copy or a move, through the same prompts and safety rules as F5/F6. | P3 | IN-MVP | Not Started |

## Summary

Users copy files in Explorer and expect to paste them in MyCommander. Today they must drag them or re-navigate.

- **Key:** `Alt+Shift+V` (checked free in the key table; `Ctrl+V` is taken by Windows Terminal's paste, so it is not used).
- **Menu:** **File → Paste from Explorer**.
- **Behaviour:**
  - the clipboard must hold a file list (`CF_HDROP`), otherwise a message says so and nothing happens;
  - a clipboard "cut" marker (`Preferred DropEffect` = move) makes it a **move**; otherwise it is a **copy**;
  - the items go through the existing copy or move engine into the active folder, with the same conflict prompts
    (Overwrite, Skip, Rename), the same progress and Esc, and the same error handling (Retry, Skip, Elevate).
- **Refusals:** a provider or plugin panel as destination; a clipboard file list that includes a folder being pasted into
  itself (the existing recursion guard, FOP-010); a source that no longer exists is reported, the rest continue.

## What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| Copy and move engines with conflicts, progress, cancel and elevation | `FileOps.{h,cpp}` `CopyItems`, `MoveItems` | The transfer |
| Copy/move flow in the UI | `main.cpp` F5/F6 flow (`DoCopyOrMove`), `ProgressUi` | Prompts and progress |
| Recursion guard | `PathUtil.h` `IsSameOrWithin` (FOP-010) | Refusing a paste into itself |
| Clipboard text writing | `Clipboard.{h,cpp}` `CopyTextToClipboard` (SEL-008) | Sibling module for reading |
| Background queue option | `main.cpp` `QueueCopyOrMove` (FOP-014) | Optional later |

## Design

### D1 — Reading the clipboard (`Clipboard.{h,cpp}`)

- New: `ReadClipboardFileList(std::vector<std::filesystem::path>& out, bool& isCut)`. It opens the clipboard, reads
  `CF_HDROP` through `DragQueryFileW`, reads the `Preferred DropEffect` format (registered name, the same one Explorer
  uses), and closes the clipboard in every path (RAII guard).
- The clipboard is read only on the key or menu action, never polled.
- **Rule change:** CLAUDE.md states that `CopyTextToClipboard` is the only Win32 clipboard call in the codebase (CON-004).
  This plan adds a read function to the same module. The rule becomes "all clipboard access lives in `Clipboard.cpp`",
  which keeps the intent (one place, one reviewable file) and is written into CLAUDE.md with the change.

### D2 — Pure part (`ClipboardPaste.{h,cpp}`)

- `PlanPaste(sources, destinationFolder, isCut)`: returns, per source folder, the item names and the operation (copy or
  move); drops sources equal to or inside the destination (FOP-010 rule) with a reason; keeps the order.
- Unit-tested without the clipboard.

### D3 — Flow (`main.cpp`)

- `DoPasteFromExplorer`: refuses provider/plugin destinations; reads the clipboard; shows a message for an empty or
  non-file clipboard; plans; runs the copy or move through the existing engines grouped by source folder (as the
  synchronization executor does); reports through `ReportOutcome`; refreshes both panels.
- Confirmation: none for a copy; for a move, the same prompt as F6 (no extra confirmation).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/Clipboard.{h,cpp}` | `ReadClipboardFileList` (the only added Win32 clipboard code) |
| `Source/MyCommander/ClipboardPaste.{h,cpp}` (new) | `PlanPaste` (pure) |
| `Source/MyCommander/main.cpp` | `DoPasteFromExplorer` |
| `Source/MyCommander/KeyMap.{h,cpp}`, `PluginManagerLogic.cpp`, `MenuBar.h`, `Help.cpp`, `Strings.h` | Command, key, menu item, help, messages |
| `Source/MyCommanderTests/ClipboardPasteTests.cpp` (new), `KeyMapTests.cpp` | Tests below |
| Both `.vcxproj` files | New source and test files |

## Tests

- **`PlanPaste` (pure):** groups by source folder; a copy and a cut give copy and move; a source equal to the destination
  is dropped with a reason; a source inside the destination is dropped (FOP-010); order preserved; an empty list.
- **Clipboard read:** not automated (it depends on the real clipboard and the other program's copy); covered by a manual
  test. The parser that turns the `DROPFILES` data into paths is pure only if separated; the plan separates it and tests it
  with a built buffer.
- **Flow on `TempDir`:** through the copy and move engines with the existing fake handlers (already used by FileOps
  tests): a copy leaves the source; a cut removes it; a conflict asks.
- **Key:** `Alt+Shift+V` → `PasteFromExplorer` in the default table and the inventory.
- **Manual:** copy three files in Explorer, paste with `Alt+Shift+V`; cut and paste; paste a folder into itself (refused);
  paste into a provider panel (refused); empty clipboard (message).

## Documentation updates

- `AIPrompt/my-commander.md`: new row **FOP-025**; CON-004's note on clipboard access; version bump; history paragraph.
- `README.md`: keybindings row (`Alt+Shift+V`); a "Paste from Explorer (FOP-025)" section; the File menu list.
- `USERGUIDE.md`: keybindings row and the same section.
- `Source/MyCommander/Help.cpp`: F1 row.
- `CLAUDE.md`: the clipboard rule (CON-004) rewritten as "all clipboard access in `Clipboard.cpp`"; `ClipboardPaste` in the
  source layout; Testing list.
- `AIPrompt/ManualTests.md`: new MT entry.
- `AIPrompt/features/FOP-025-plan.md`: status and differences.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.

## Phasing

1. `PlanPaste` and the buffer parser, with tests (pure). 2. `ReadClipboardFileList`. 3. Flow, key, menu, docs.

## Risks

- **Clipboard contention:** another program may hold the clipboard; the read fails cleanly with a message.
- **Cut semantics:** a cut from Explorer moves files only when the paste happens; the manual test covers it so the user is
  never surprised by a move they did not ask for. The F6 prompt is shown, so the move is always visible.
- **Clipboard rule change:** a deliberate, documented change of CLAUDE.md wording, not a silent exception.

## Open questions

1. **Cut as move:** honour Explorer's cut (recommended, as Explorer does), or always copy?
2. **Queue in the background:** later (FOP-014 extension), not in v1.

## Status

Planned 2026-10-15. **Implemented 2026-10-15** (spec 1.42.0, version 2.6.0): `ClipboardPaste.{h,cpp}`,
`Clipboard.cpp` `ReadClipboardFileList`, `DoPasteFromExplorer`, `Alt+Shift+V`, File menu item, F1 row, tests, MT-059.

Open questions: a cut is honoured as a move (with one confirmation); no background queue in v1.

Differences from the plan:

- `Alt+Shift+V` collided with the `Alt+<letter>` menu mnemonic (Alt+V opens View), which the plan's "checked free" missed;
  `Alt+Shift+<letter>` is now never a menu mnemonic (main loop and `IsMenuChord`).
- `PerformCopyOrMove` takes a source folder path instead of a Panel so pasted groups need no panel.
- `PlanPaste` compares paths lexically (case-insensitive) rather than via `IsSameOrWithin`, keeping it disk-free.
- The move confirmation is one Yes/No for the whole paste, because there is no F6 destination prompt to show.
