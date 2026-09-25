# MyCommander Manual Test Plan

## Purpose

This human test plan covers the implemented MyCommander MVP. It is based on `README.md`, `AIPrompt/my-commander.md`, current source code, automated-test coverage, and project changelogs. It complements the automated `MyCommanderTests` suite.

For every test, record **Pass**, **Fail**, **Blocked**, or **Not run**. A blocked test requires an unavailable environment capability (for example, a UNC share); record why. For failure, record observed behavior, affected path, and evidence.

## Test run record

| Field | Value |
|---|---|
| Build/version | |
| Executable path | |
| Windows version / terminal | |
| Tester / date | |
| Fixture root | |

## Safety and test data

Use only a disposable root such as `%TEMP%\MyCommanderManualTest`; never run destructive cases against user, production, or system data.

1. Create `Left` and `Right` beneath the fixture root.
2. Under `Left`, create directories `Alpha`, `Beta`, `Nested\Deep`, `EmptyDir`, and `CopySource\Child`.
3. Create `alpha.txt`, `beta.log`, `report 2026.txt` (containing **unique viewer needle**), `same.txt` (containing **LEFT VERSION**), and `Nested\Deep\needle.txt`. Create `CopySource\Child\child.txt`.
4. Under `Right`, create `Destination` and a `same.txt` containing **RIGHT VERSION**.
5. Add `árvíztűrő-日本語.txt`, `ＡＢＣ.txt`, a 20 MB+ `large.bin`, and disposable Hidden/System files.
6. Optional fixtures: a junction/symlink, a UNC share, a path longer than `MAX_PATH`, UTF-8/UTF-16/ANSI text files, and a 100,000-file directory.
7. Start with the fixture paths:

```powershell
Build\x64\Release\MyCommander.exe "$env:TEMP\MyCommanderManualTest\Left" "$env:TEMP\MyCommanderManualTest\Right"
```

Restore modified settings in `%APPDATA%\MyCommander\mycommander.ini` after testing.

## Startup, screen, and input

### MT-001 — Startup forms and version

**Requirements:** CON-001, CON-002, CLI-001, UI-001, UI-002, LIST-001, PER-001

**Description:** Verify zero/one/two path startup and the version action.

**Steps:**
1. Run `MyCommander.exe --version`.
2. Run the executable with no paths, then with `Left` only, then with `Left` and `Right`.
3. Repeat one startup form in Windows Terminal and, if available, the legacy Console Host.
4. Inspect the initial screen.

**Expected result:** Version prints and exits. Every startup form opens two panels without crashing in each available supported terminal; supplied paths populate the matching panels, and exactly one panel is active.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-002 — Help, keyboard-only use, and exit

**Requirements:** KEY-001, KEY-003, KEY-004, KEY-006

**Description:** Verify the shortcut reference and key-only closing paths.

**Steps:**
1. Press `F1`; scroll with arrows, `Page Down`, `Home`, and `End`.
2. Close it separately with `F1`, `Esc`, and `F10`.
3. With an empty command line, quit once with `Esc` and once with `F10`.

**Expected result:** Help lists shortcuts, scrolls, and closes by every documented key. No mouse is needed.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-003 — Normal panel layout and contextual information

**Requirements:** UI-003, UI-004, UI-007, UI-008, ACC-001

**Description:** Verify headers, rows, text status indicators, command line, and hints.

**Steps:**
1. Inspect both headers, volume information, and the bottom command/hint areas.
2. Move over a directory, a file, and a selected file; press `Tab` and repeat.
3. At a wide terminal size inspect type, attributes, modification time, and size columns.

**Expected result:** Each panel shows its own location and available volume information. Directories show `<DIR>`, selections have a text `*` marker, attributes use `R/H/S/A`, and hints change with context.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-004 — Resize, Unicode, frames, columns, and themes

**Requirements:** UI-005, UI-006, UI-010, UI-011, UI-014, ACC-002, ACC-006

**Description:** Verify visual adaptations and supported configuration appearance options.

**Steps:**
1. Resize from wide to narrow, below 60×15, then back to usable size.
2. Inspect the Unicode fixture names and column alignment.
3. Open a prompt with `F7` and inspect default box frames; cancel it.
4. Set `useBasicSymbols = true`, restart, and verify ASCII `+ - |` frames; restore false.
5. Set `visibleColumns` and valid widths to alternate values, restart, then restore defaults.
6. Set `colorTheme = highcontrast`, restart, inspect readability, then restore `default`.

**Expected result:** Columns drop gracefully on narrow screens. Below minimum size only the warning and `F10` work. Unicode aligns correctly for terminal-supported glyphs. Default frames are box drawing; fallback frames are ASCII. Both themes remain readable and configured columns take effect.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-005 — Keyboard dialogs and text prompts

**Requirements:** UI-009, KEY-001

**Description:** Verify all dialog primitives work without a mouse.

**Steps:**
1. Open `F7`, type a temporary name, use `Backspace`, then `Esc`.
2. Trigger a disposable-file delete confirmation.
3. Select its choice once by displayed hotkey and once using arrows plus `Enter`; cancel a prompt with `Esc`.

**Expected result:** Text prompts edit normally and choice dialogs are fully controllable with hotkeys, arrows, `Enter`, and `Esc`.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

## Navigation, listing, and selection

### MT-006 — Directory navigation and independent panel state

**Requirements:** NAV-001, NAV-002, NAV-005, VEE-001, LIST-005

**Description:** Verify navigation and that panels retain separate location/cursor/sort/selection state.

**Steps:**
1. Open `Alpha` with `Enter`, return with `Backspace`.
2. On left, sort with `Ctrl+F5`, select an item with `Insert`, and position cursor.
3. Press `Tab`; navigate right into `Destination`, set another sort, and move its cursor.
4. Switch repeatedly and leave/re-enter a location.

**Expected result:** Enter/parent works. Each panel retains its own state, and refresh/navigation preserves a valid cursor.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-007 — Typed paths, history, drives, and restored locations

**Requirements:** NAV-003, NAV-004, NAV-006, NAV-008

**Description:** Verify path input, per-panel back/forward, drive choice, and normal-exit session restore.

**Steps:**
1. Type `Nested\Deep`, then an absolute `Left` path, pressing `Enter` each time.
2. Navigate to `Alpha`, then `Beta`; use `Alt+Left`/ `Alt+Right`. Navigate to a new location and confirm Forward is cleared.
3. Open `Alt+F1` and `Alt+F2`, navigate picker controls, cancel once, and select a valid drive once.
4. Leave distinct panel paths, exit with `F10`, restart without arguments, then restart with explicit paths.

**Expected result:** Existing absolute/relative paths navigate instead of executing. Back/Forward is independent per panel. The requested picker changes only its target panel. Normal exit restores paths only; explicit arguments override them.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-008 — Sorting, grouping, filtering, and hidden files

**Requirements:** LIST-002, LIST-003, LIST-004, NAV-010, SRC-003

**Description:** Verify sort choices/direction, grouping configuration, quick filters, and hidden/system visibility.

**Steps:**
1. Press each of `Ctrl+F3` through `Ctrl+F6` twice, inspecting order and direction marker.
2. Confirm `..` remains first and directories first by default.
3. Set `groupDirectoriesFirst = false`, restart, sort, confirm interleaving, then restore true.
4. Use `Ctrl+F` with `log`, `*.txt`, and empty input.
5. Verify hidden/system entries are absent, press `Ctrl+H`, inspect their entries/header marker, and toggle back.

**Expected result:** All sort keys work; second press reverses order. Grouping follows configuration. No-wildcard filters match substrings, wildcard filters use `*`/`?`, and empty clears. Hidden/system visibility toggles per active panel.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-009 — Reparse points, UNC, long paths, and inaccessible locations

**Requirements:** CON-005, NAV-009, LIST-006, SEC-006

**Description:** Verify special-path safety and error presentation.

**Steps:**
1. If available, locate fixture junction/link; inspect its label and press `Enter`.
2. If available, navigate to a UNC share root, enter a child, then press `Backspace` at the share root.
3. If available, navigate and list a long-path fixture.
4. Safely direct one panel to an unavailable/removable/inaccessible fixture location.

**Expected result:** Links/reparse points are labelled and cannot be entered. Parent navigation never rises above a UNC share root. Long paths work. Broken/unavailable locations show a clear panel-local message and do not crash.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-010 — Selection commands and target rule

**Requirements:** SEL-001, SEL-002, SEL-003, SEL-004, SEL-005

**Description:** Verify individual, all, inverse, clear, mask selection and operation target semantics.

**Steps:**
1. Mark two items with `Insert`; inspect count and total size.
2. Use `Numpad *`, `Ctrl+Numpad -`, and `Ctrl+Numpad +`.
3. Use `Numpad -` with `*.txt`, then `Numpad +` with `*.log`.
4. Clear selection, copy only cursor `alpha.txt` using `F5`; then select two files and copy again.

**Expected result:** Every selection command works and status reports selected count/size. Operations use only the cursor item when selection is empty, otherwise all selected items.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

## File operations and safety

### MT-011 — Create directory/file and rename

**Requirements:** FOP-003, FOP-004, FOP-005, ERR-001

**Description:** Verify creation, rename, and nonfatal duplicate-name errors.

**Steps:**
1. Press `F7`, create `MadeDir`.
2. Press `Shift+F7`, create empty `made.txt`.
3. Place cursor on it, press `F2`, rename to `renamed.txt`.
4. Attempt an existing name for creation or rename.

**Expected result:** Directory/file creation and cursor-entry rename succeed. Duplicate failure names the operation/path and leaves the application usable.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-012 — Copy/move, recursive trees, metadata, and progress

**Requirements:** FOP-001, FOP-002, FOP-009, FOP-013, PER-004, PER-005

**Description:** Verify file/tree transfer and progress details.

**Steps:**
1. Set right panel to `Destination`; copy `alpha.txt` and `CopySource` from left with `F5`.
2. Create `move-me.txt` and `MoveTree\inside.txt`; move them with `F6`.
3. Copy/move `large.bin` and inspect the progress dialog.
4. Compare source/destination content, size, modified time, and applicable attributes.

**Expected result:** Files and complete trees transfer to the opposite panel. Move removes source only after success. Progress identifies source/destination/item/count and, for large files, bytes and speed. Applicable metadata is retained.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-013 — Collisions, summaries, and subtree protection

**Requirements:** FOP-010, FOP-011, SEC-005, SEC-007

**Description:** Verify collision decisions, final outcomes, and prevention of copying/moving into a descendant.

**Steps:**
1. Copy `Left\same.txt` onto `Right\same.txt`; separately choose Skip, Rename (`same-renamed.txt`), Overwrite, and Cancel.
2. Select two colliding disposable files and use an apply-to-all option.
3. Inspect the final completed/skipped/failed summary.
4. Select `CopySource`, point other panel to `CopySource\Child`, then try `F5` and `F6`.

**Expected result:** Dialogs clearly identify targets. Each collision choice behaves as selected; summary reports outcomes. Recursive self-targeting is refused without creating a runaway tree.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-014 — Recycle Bin, permanent delete, and confirmations

**Requirements:** FOP-006, FOP-007, FOP-008, SEC-007

**Description:** Verify distinct deletion modes and configurable confirmation.

**Steps:**
1. Delete `recycle-me.txt` with `F8`; cancel, then confirm; check Recycle Bin.
2. Delete `permanent-me.txt` with `Shift+F8`; cancel, then confirm; check it is not in Recycle Bin.
3. Set each delete confirmation setting false, restart, perform disposable deletes, then restore true.

**Expected result:** Both dialogs clearly identify the affected target. Cancel preserves files. Recycle deletion uses Recycle Bin where supported; permanent delete bypasses it. Settings control confirmation.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-015 — Cancellation, attributes, and retry recovery

**Requirements:** FOP-012, PER-002, CON-005, ERR-001, ERR-002, ERR-003

**Description:** Verify safe cancellation, all attribute choices, and error-recovery choices.

**Steps:**
1. Start a large copy, move, and permanent delete of separate disposable data; press `Esc` while each progress dialog is active.
2. Use `Ctrl+A` to set and clear Read-only, Hidden, System (if allowed), and Archive; use `Ctrl+H` as needed.
3. Safely create a locked/no-write fixture condition. Attempt a supported operation, then in separate attempts choose Retry after fixing it, Skip, Skip all for multiple items, and Cancel.

**Expected result:** Esc is handled promptly; completed work stays valid and application remains responsive. Attributes change as chosen. Failure dialogs name operation and exact path; Retry/Skip/Skip all/Cancel behave correctly without a crash.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-016 — Access-denied elevation retry (environment-dependent)

**Requirements:** SEC-001, SEC-002, SEC-003, ERR-003

**Description:** Verify narrowly scoped UAC retry behavior.

**Steps:**
1. Create an administrator-protected disposable test item, never a system item.
2. Attempt a supported Copy/Move/Delete/attribute action that produces Access denied.
3. Verify **Elevate & Retry** appears. Decline UAC once; repeat and accept only if authorized.
4. Verify only that item is retried and the main application remains non-elevated.

**Expected result:** Startup never elevates. Elevate & Retry appears only for access denied, declines safely, and approved elevation is limited to the one retry helper action.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

## View, search, command line, and shell

### MT-017 — Viewer navigation, search, encodings, and large text

**Requirements:** VEE-003, VEE-004, UI-011

**Description:** Verify the built-in viewer.

**Steps:**
1. Press `F3` on `report 2026.txt`.
2. Use arrows, paging, `Home`, `End`; press `/` to search **unique viewer needle**, then `n`; search a missing term.
3. Close independently by `Esc`, `F3`, and `F10`.
4. If fixtures exist, view UTF-8, UTF-16 BOM, ANSI, and large text; page near its end.

**Expected result:** Content displays, navigation/search works, and every close path works. Supported encodings are readable and a large text file can be paged without a crash or arbitrary early-data cap.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-018 — External edit and configured Enter behavior

**Requirements:** VEE-002, VEE-006, VEE-008

**Description:** Verify external editor and view/edit/execute default file action.

**Steps:**
1. With default setting, press `Enter` on `alpha.txt`.
2. Press `F4` and close the external editor.
3. Set `enterFileAction = edit`, restart, and press Enter on a disposable text file.
4. Set `enterFileAction = execute`, restart, and press Enter only on a harmless associated disposable file, such as a text file.
5. Restore `enterFileAction = view`.

**Expected result:** Default Enter views; F4 uses `%EDITOR%` or Notepad. Plain Enter follows view/edit/execute configuration, while F3/F4 remain view/edit. Execute/open occurs only after explicit Enter.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-019 — Recursive Find Files and result actions

**Requirements:** SRC-001, SRC-002, SEC-006, PER-002

**Description:** Verify recursive name/wildcard search and picker controls.

**Steps:**
1. Press `Alt+F7`, search `needle`, then `*.txt`.
2. Navigate results with arrows, pages, Home, and End.
3. On a file result press `F3`, close viewer, then press `Enter` on file/directory results.
4. Search a missing pattern. If using a large fixture, begin a search and press `Esc`.

**Expected result:** Matching files/directories are found recursively, directories have a text trailing `\`, F3 views a file result, Enter navigates the active panel, and no-match/cancel are clear. Search does not traverse reparse points.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-020 — Command line, quoting, history, privacy, and separate shell

**Requirements:** CLI-002, CLI-003, CLI-004, CLI-005, CLI-006, CLI-007, SEC-004

**Description:** Verify commands run in the active path and command/history semantics.

**Steps:**
1. In left panel type `cd`, press Enter, and inspect output. Run `echo hello > command-output.txt`; verify file is in left path.
2. Select `report 2026.txt`; inspect `Ctrl+Enter` and `Ctrl+Shift+Enter` insertion, then clear with Esc.
3. Run two harmless commands; browse with `Ctrl+Up/Down`; quit/restart and verify persistence.
4. Run a unique command with a leading space; restart and verify it is not recalled.
5. Set `persistCommandHistory = false`, run a unique command, restart, verify absence, then restore true.
6. Press `F9`, run `cd`, then `exit`.

**Expected result:** Commands use active directory and normal `cmd.exe` metacharacter behavior. Paths/names are inserted safely. History follows persistence setting and excludes leading-space commands. F9 opens a shell in active path and returns cleanly.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

## Configuration, recovery, logging, and scale

### MT-021 — Configuration creation and tolerant parsing

**Requirements:** CFG-001, CFG-002, CFG-003, CFG-004, CFG-005

**Description:** Verify readable configuration, defaults, unknown keys, and invalid-value reporting.

**Steps:**
1. Back up `%APPDATA%\MyCommander\mycommander.ini`; remove/rename it; start and quit application.
2. Inspect the recreated commented plain-text configuration.
3. Add `unknownSetting = value`, restart.
4. Set `enterFileAction = invalid`, restart, inspect the message and resulting default.
5. Restore the backup.

**Expected result:** A documented per-user config with defaults is created. Unknown keys do not block startup. A recognized invalid setting names its key/value and only that setting falls back to default.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-022 — Safe session state after normal/abnormal end

**Requirements:** NAV-008, CLI-004, ERR-006

**Description:** Verify only safe UI state persists and file operations never replay.

**Steps:**
1. Leave panels at distinct paths, make a selection, run a harmless command, and quit normally.
2. Inspect `%APPDATA%\MyCommander\session.ini` and optional `history.txt`.
3. Restart with no paths and inspect restored state.
4. If safe with disposable data, cancel/abnormally close during an in-progress copy; restart and inspect fixture before any new action.

**Expected result:** Session contains only left/right paths written on normal exit. Selections and operations are not restored. History is separate. No interrupted or queued action is automatically replayed.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-023 — Diagnostic logging and command privacy

**Requirements:** ERR-004, ERR-005

**Description:** Verify severity configuration and that command text is not logged.

**Steps:**
1. Set `logLevel = off`, restart, perform an action, and check no new log activity.
2. Set `logLevel = info`, restart/exit, inspect `%APPDATA%\MyCommander\mycommander.log`.
3. Set `error`, cause a safe fixture failure, then inspect log operation/path context.
4. Run ` echo MANUAL_SECRET_TOKEN_7d4a`; search the log for that exact token.
5. Restore `logLevel = off`.

**Expected result:** Logging is opt-in and follows configured severity. Error logs contain useful file-operation context. Typed command text does not occur in diagnostic logs.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

### MT-024 — Large directory responsiveness

**Requirements:** LIST-008, PER-002, PER-003

**Description:** Verify incremental enumeration and responsive input on large listings.

**Steps:**
1. Navigate to the optional 100,000-file fixture; if unavailable, use the largest safe generated directory and record count.
2. Observe loading marker.
3. During loading press Tab, arrows, resize terminal, and later use sorting.
4. Complete loading and navigate first-to-last.

**Expected result:** A loading marker appears when applicable. Input/resize remain responsive, enumeration completes without crash, and the large directory can be navigated/sorted. Record entry count and observed responsiveness.

**Test result:** ☐ Pass ☐ Fail ☐ Blocked ☐ Not run. Notes/evidence: __________

## Cleanup and release decision

1. Restore changed configuration and remove test-only logs/history only if project policy requires it.
2. Remove the disposable fixture only after results/evidence are captured.
3. Summarize all failed/blocked IDs and environment limitations.
4. Treat any suspected data-loss defect as a release blocker (TST-007).
