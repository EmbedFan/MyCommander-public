# Fix Plan — IS-0018

Source: `AIPrompt/issue_fix_requests.md`

> When delete one run more files it should use a dialog with animation like the F5, copy function does.
> The TreeView use the Numpad: +/-, simple - but simple + is not working for expanding

## Summary

Two independent problems:

1. **Recycle Bin delete (F8 / Delete) shows no progress dialog.** Copy (F5) and permanent delete
   (Shift+F8) both show the animated progress dialog; the default Recycle Bin delete does not.
2. **In the tree view, the main keyboard's `+` doesn't expand a folder**, while `-` collapses and the
   numpad `+`/`-` both work. On the Hungarian keyboard layout, `+` is typed as `Shift+3`, and the
   tree only looks at *virtual key codes*, never at the typed character.

## Part 1 — Animated progress for Recycle Bin delete

### Current behavior

- `DoDelete` (`main.cpp:3065`) already builds a `ProgressUi` (the same animated dialog Copy/Move
  use, `main.cpp:987`) and passes `onItemStart`/`cancelPoll`/`onError` to `FileOps::DeleteItems`.
- But `DeleteItems` (`FileOps.cpp:679`) only uses them on the **permanent** path, which deletes item
  by item.
- The **Recycle Bin** path (`permanent == false`, the default for F8 and Delete) sends the whole
  selection to **one** batched `SHFileOperationW(FO_DELETE, FOF_ALLOWUNDO | FOF_SILENT …)` call.
  `onItemStart` is never called, so `ProgressUi` never creates its `AnimatedProgressDialog`. The
  screen just freezes until the shell returns. This is documented as a deliberate trade-off in
  `FileOps.h`'s `DeleteItems` comment ("the Recycle Bin path is a single batched shell call with
  no per-item progress or retry point").
- **Hidden correctness bug** in the same branch: if the batched call returns any error, *every*
  item is reported as failed (`FileOps.cpp:704-706`), even though the shell may have recycled most
  of them before hitting the one that failed. The summary dialog then lies, saying N items failed.

### Design

Make the Recycle Bin path item-by-item, exactly like the permanent path's loop:

- For each item:
  - call `onItemStart(name, index, total)`. This draws and animates the dialog, and returning
    false means Esc, which cancels before the next item;
  - then recycle just that item with a one-item `SHFileOperationW` (same flags as today, so still
    silent and undoable);
  - run it through the existing `RunWithRetry`, so a failure offers **Retry / Skip / Skip all /
    Cancel** (`onError`, FOP-010), the same as permanent delete.
- **Elevation:** the Recycle Bin path passes `win32Error = 0`, so *Elevate & Retry* is not offered
  (SEC-003 keeps elevation to the permanent path, unchanged).
- **During a long single item** (sending a big folder to the Recycle Bin), the
  `AnimatedProgressDialog`'s own 250 ms renderer thread keeps the spinner moving while the blocking
  shell call runs. That's the property that makes the ZIP and F5 dialogs animate during long single
  operations too.
- **Outcome is now exact per item:**
  - `succeeded` counts the items actually recycled;
  - `failed` lists only the ones that failed, each with its own shell error text
    (`FormatRecycleBinDeleteFailed`);
  - `fAnyOperationsAborted` on one item still means cancelled.
- **Cost:** one shell call per top-level *selected* item, not per file inside a folder. Even
  hundreds of selected items are fine; the per-call overhead is small next to the Recycle Bin's own
  work.

### Files touched

- `Source/MyCommander/FileOps.cpp`: the `!permanent` branch of `DeleteItems` becomes a loop sharing
  the permanent loop's shape: `onItemStart`, `RunWithRetry`, outcome bookkeeping.
- `Source/MyCommander/FileOps.h`: update `DeleteItems`' doc comment. `onItemStart`, `cancelPoll`
  between items, and `onError` now apply to both paths; `onElevate` stays permanent-only.
- `main.cpp`: no change needed. `DoDelete` already wires `ProgressUi`. Its PER-002 comment says the
  progress is permanent-only and gets reworded.

### Test plan (`FileOpsTests.cpp`)

The Recycle Bin can be exercised safely: the tests run in `%TEMP%`, and the recycled test files
simply land in the user's Recycle Bin. The existing `Delete_RecycleBin_RemovesItemFromOriginalLocation`
(`FileOpsTests.cpp:892`) already does exactly this and keeps passing unchanged.

- `Delete_RecycleBin_CallsOnItemStartPerItem_IS0018`: three files, and `onItemStart` is called 3
  times with done = 0, 1, 2 and total = 3.
- `Delete_RecycleBin_CancelBeforeSecondItem_LeavesTheRest_IS0018`: `onItemStart` returns false on
  the second item. The first file is gone, the second and third still exist, and
  `outcome.cancelled` is set.
- `Delete_RecycleBin_LockedFile_FailsOnlyThatItem_IS0018`: lock the middle file with
  `LockExclusive` (an existing helper). Answering Skip gives `succeeded == 2`, `skipped == 1`,
  `failed` empty. With no `onError`, `failed` has exactly 1 entry, naming the locked file. This is
  the regression guard for the "everything failed" bug.
- Manual check: select several files and a large folder, press F8, confirm. The animated dialog
  shows "Deleting", an item counter and the spinner, and Esc stops between items.

## Part 2 — Tree view: typing-area `+` expands

### Current behavior

- `ClassifyTreeKey(virtualKeyCode, ctrlHeld)` (`DirectoryTree.cpp:139`) maps `VK_ADD`/`VK_OEM_PLUS`
  to *Expand* and `VK_SUBTRACT`/`VK_OEM_MINUS` to *Collapse*.
- **Numpad `+`/`-`** always arrive as `VK_ADD`/`VK_SUBTRACT`, so they work.
- **The typing-area keys are layout-dependent.** On the US layout, `+` is `Shift` + the `=` key
  (`VK_OEM_PLUS`), so it works. On the **Hungarian** layout:
  - `-` has its own key (`VK_OEM_MINUS`), so it works;
  - `+` is `Shift+3`, reported as virtual key `'3'` with the character `'+'`, so it gives `None` and
    does nothing.

  That is exactly the report: "simple `-` works, simple `+` does not".
- The character is never available to the tree. `IPanelContent::OnKey(WORD virtualKeyCode, DWORD
  controlKeyState)` (`Panel.h:33`) doesn't pass `uChar.UnicodeChar`. The host call site
  (`main.cpp:3527`) has it in `record.Event.KeyEvent.uChar.UnicodeChar` but drops it.

### Design

Decide by the **typed character** first, falling back to the virtual key. This works on every
layout:

- `IPanelContent::OnKey` gains a third parameter, `wchar_t unicodeChar`. There are only two
  implementers:
  - `DirectoryTreeContent` (`main.cpp:660`) passes it to `ClassifyTreeKey`;
  - `PluginManager`'s private `PanelContentAdapter` (`PluginManager.cpp:323`) ignores it. The
    **plugin ABI is unchanged**: `McPlugin_OnPanelKey` keeps its `(side, virtualKeyCode,
    controlKeyState)` signature, so no SDK or API version bump is needed.
- `main.cpp:3527` passes `record.Event.KeyEvent.uChar.UnicodeChar`.
- `ClassifyTreeKey(virtualKeyCode, ctrlHeld, unicodeChar = 0)`:
  - if `unicodeChar == L'+'`, return *ExpandCursor*;
  - if `unicodeChar == L'-'`, return *CollapseCursor*;
  - otherwise use the existing virtual-key switch, unchanged.

  The default argument keeps existing callers and tests compiling.

### Files touched

- `Source/MyCommander/Panel.h`: the `IPanelContent::OnKey` signature, plus its doc comment.
- `Source/MyCommander/DirectoryTree.{h,cpp}`: the `ClassifyTreeKey` signature and the character
  check.
- `Source/MyCommander/main.cpp`: `DirectoryTreeContent::OnKey` and the call site.
- `Source/MyCommander/PluginManager.cpp`: the adapter's override signature (ignores the char).

### Test plan (`DirectoryTreeTests.cpp`)

- `ClassifyTreeKey_TypedPlusAndMinusOnAnyLayout_IS0018`:
  - `ClassifyTreeKey('3', false, L'+')` gives `ExpandCursor` (Hungarian `Shift+3`);
  - `ClassifyTreeKey(VK_OEM_MINUS, false, L'-')` gives `CollapseCursor`;
  - `ClassifyTreeKey('3', false, L'3')` gives `None`, so a plain digit still does nothing.
- The existing `ClassifyTreeKey_PlusAndMinusOnBothKeyboards` keeps passing unchanged, thanks to
  the default argument.
- Manual check on a Hungarian layout:
  - in the tree (Ctrl+T), `Shift+3` expands the cursor folder;
  - `-` collapses it;
  - numpad `+`/`-` still work.

## Docs, spec, version

- `AIPrompt/my-commander.md`:
  - update the `FOP-009`/`PER-002` notes: the animated progress now covers the Recycle Bin delete;
  - update `UI-013` (tree view): `+`/`-` work by typed character on any keyboard layout;
  - add an IS-0018 history line.
- `USERGUIDE.md` / `README.md`:
  - the delete paragraph says both delete kinds show live progress with Esc to cancel;
  - the tree view keys say "`+` / `-` (numpad or main keyboard, any layout)".
- `Help.cpp`: the tree row text, if it names keys. Keep it in sync with README per CLAUDE.md.
- `Version.h`: PATCH bump (bug fixes).
- Changelog entry.

## Risk / scope notes

- **Part 1, behavior change:** a selection with one failing item now recycles the rest and asks
  Retry/Skip, instead of reporting everything as failed. This is strictly more accurate. The
  per-item shell calls are each still `FOF_ALLOWUNDO` (Recycle Bin) and `FOF_SILENT`
  (MyCommander's own dialog, not Explorer's), so nothing new appears from the shell.
- **Part 2:** the interface change is host-internal, with two implementers; there are no plugin
  ABI consequences.
- **Out of scope** (each a natural follow-up if wanted):
  - the ZIP plugin panel's own keys use virtual keys too. A `+` typed via `Shift+3` there isn't
    relevant today, since its `+`-style keys are `Ctrl+Numpad`, but extending the plugin ABI to
    carry the character would be an SDK change of its own;
  - queuing deletes in the background (FOP-014 only queues copy/move).

## Status

Implemented 2026-10-07 as planned (version 1.15.1). Not anticipated by the plan: `JobWorker.cpp`
(FOP-014's background jobs) also calls `DeleteItems`, and already passes `onItemStart`/`onError`, so
queued Recycle Bin deletes get per-item progress and per-item errors too, with no change of their own.
