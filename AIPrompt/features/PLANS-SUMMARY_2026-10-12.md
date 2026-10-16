# Implementation plans summary — 2026-10-12

Seven plans written on 2026-10-12, all saved in `AIPrompt/features/`. **Status as of 2026-10-13:** DOC-SYNC and
MENU-STUBS are both implemented (see each plan's own Status section); the other five are still as originally
written below.

| Plan | What it covers |
|---|---|
| `DOC-SYNC-plan.md` | **Implemented 2026-10-13.** Records DEC-004 (commands run through `cmd.exe /c`), DEC-005 (copy engine `CopyFileExW`/`MoveFileWithProgressW`) and DEC-006 (Recycle Bin through `SHFileOperationW`). Regenerates the system plan. Adds manual tests MT-025…MT-047. |
| `MENU-STUBS-plan.md` | **Implemented 2026-10-13** (all 4 phases). Fills the empty menu items, each with a new requirement ID: Used Disk Area (FOP-020), Generate Check Code (FOP-021: MD5/SHA, plus verify), Copy Selected Names (SEL-008), Disk Label (NAV-013), Network Connection (NAV-014), Share/Unshare (NAV-015), Full/List view (UI-017). |
| `LIST-007-plan.md` | Each panel watches its folder for outside changes. It refreshes 300 ms after the last change (at least every 2 s during a burst), keeps the selection, cursor and scroll, and waits while a dialog is open. New settings `autoRefresh = on/off/poll` and `autoRefreshPollSeconds`. |
| `FOP-015-plan.md` | A shared pause switch built into the existing cancel checks, so the copy/delete/search code needs no signature changes. `P` pauses in progress boxes. Background jobs get a Paused state and Pause all. Plugins get a safe pause call (plugin API 1.11). |
| `EXT-002-plan.md` | **Implemented 2026-10-14** (all six phases; background jobs stay local-only). One common file-system interface. MyCommander's own panels show ZIP, FTP and SFTP content, so sorting, filter, compare/sync and jobs work there, and you can copy between any two of them. Plugin API 1.12. Six phases; this is the biggest refactor so far. |
| `ARC-004-plan.md` | miniz can't decrypt, so the plan adds its own decryption using Windows' built-in crypto (CNG). It reads WinZip AES and legacy ZipCrypto archives, and packs with AES-256. The password is asked in a masked field, kept only in memory and wiped after use. It never goes to the log, history, config or temporary files, and a test checks this. |
| `SFTP-WRITE-TESTS-plan.md` | Server choices, how to run the seven existing write checks (mkdir, upload, rename, download, Esc mid-upload, delete, rmdir), eight new automated checks, manual checks in the app, and where to record the results. |

## Decisions needed (recommendation first)

1. **DOC-SYNC:**
   - DEC-004: keep `cmd.exe` only, and record PowerShell support as a possible later option.
   - Keep recording each manual test run as a `ManualTests_<nnn>.md` copy.
   - Keep the old system-plan snapshots.
2. **MENU-STUBS:**
   - Keys: `Alt+Shift+Enter` for disk usage, `Ctrl+Shift+C` for copy names, `Ctrl+F7`/`Ctrl+F8` for Full/List view.
   - SHA-256 as the default checksum, in GNU format.
   - Share/Unshare: one administrator (UAC) prompt per action, or instead just open Windows' sharing settings?
   - A `[shared]` marker in the status line?
3. **LIST-007:**
   - On by default.
   - Periodic re-listing (every 10 s) where the folder can't be watched, typically network drives.
   - A small `[no auto-refresh]` indicator for those folders?
4. **FOP-015:**
   - The key: `P`/Pause, or Space?
   - A paused background job keeps its files reserved, so other operations on them still wait.
   - Include the plugin part (phase 4) now?
5. **EXT-002:**
   - Full scope (about six releases of work), or stop after FTP/SFTP and leave ZIP on its own panel?
   - Keep the existing plugin panel API for views that aren't file systems.
6. **ARC-004:**
   - Read and write (AES-256) as planned, or read-only first?
   - ZipCrypto: read only, never write it.
   - Remember an archive's password for the whole session after the first prompt.
7. **SFTP write tests:** these need a writable server, and installing one needs administrator rights, so the user
   sets it up.
   - Which server: Windows OpenSSH Server on this PC, Docker, or a remote server? WSL is currently broken on this
     machine.
   - Add the eight new checks now, or run only the existing seven first?
   - Test a dropped connection manually only.

## Next step

Choose which plan to start with, or answer "go ahead" to accept the recommendations above.
