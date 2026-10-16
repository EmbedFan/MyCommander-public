# Implementation Plan — the remaining "Not yet implemented" menu items

Source: FEA-0005's menu list (`AIPrompt/additional_feature_requests.md`). These items exist in `MenuBar.h` as
`stub(...)` and say "Not yet implemented" (MENU-003). None of them has a requirement ID yet; the new IDs are proposed
below.

| Menu | Item | Stub location |
|---|---|---|
| File | Used Disk Area (D) | `MenuBar.h:108` |
| File | Generate Check Code (E) | `MenuBar.h:111` |
| Selection | Copy Selected Names to Clipboard (C) | `MenuBar.h:122` |
| Commands | Disk Label (D) | `MenuBar.h:129` |
| Network | Network Connection (N), Share Folder (S), Unshare Folder (U) | `MenuBar.h:144-146` |
| View | Full (F), List (L) | `MenuBar.h:154-155` |

## Implementation check (2.0.0.368)

- **Code that doesn't exist yet:** clipboard, hashing (BCrypt hash), network-drive (`WNet*`), share (`NetShare*`) or
  volume-label (`SetVolumeLabelW`) code. `grep` finds none of them.
- **Reusable:**
  - `Panel::SelectionOrCursor()`, `SelectedSizeBytes()`, `BuildStatusLine` (SEL-005);
  - the `EnumerateDrives`/`DriveInfo` drive picker (NAV-004);
  - `ShowFormDialog`, `ShowListChoice`, `AnimatedProgressDialog` + `PeekEscapePressed` (PER-002);
  - `DirCompare`'s reparse-safe tree walk pattern (SEC-006);
  - the elevated single-operation helper (`Elevation.h`: `ElevatedAction` + hidden `--elevated-*` flags, SEC-003);
  - `KeyMap` commands; `MenuItemKind` for menu-only actions (as Assign File to App and Edit key bindings already
    are).
- **Panel drawing:** one row per entry (`DrawPanel`, `main.cpp:534`). `PanelLayout` (UI-013) and `HitTestPanel` give
  the geometry.

## Proposed requirements (spec, with this plan's implementation)

| ID | Requirement (summary) | Priority |
|---|---|---|
| FOP-020 | Calculate the space used by the selection (folders recursively, with file and folder counts), with progress and Esc; reparse points not followed | P3 |
| FOP-021 | Generate and verify checksums (MD5, SHA-1, SHA-256, SHA-512) for the selection; write `.sha256`-style files compatible with `sha256sum`/Total Commander; verify such a file | P3 |
| SEL-008 | Copy the selected names to the clipboard: names, full paths, or full paths with size and date | P3 |
| NAV-013 | Show and change a drive's volume label | P3 |
| NAV-014 | Map and disconnect network drives, and list the current connections | P3 |
| NAV-015 | Share and unshare a folder (local SMB share), through one explicit elevation per operation | P3 |
| UI-017 | Brief view: names only, in several columns per panel; per panel, toggled with Full (detailed) | P3 |

## Design

### 1. Used Disk Area — FOP-020

- **Action:** for the selection, or the cursor folder, a recursive size scan. It uses the `FindFirstFileExW` walk
  pattern from `DirCompare`, never descends into reparse points (SEC-006), and counts inaccessible folders.
- **Progress:** `AnimatedProgressDialog` showing the current folder and running totals; Esc stops it and shows a
  partial result marked as such.
- **Result dialog:**
  - total bytes and allocated size (cluster-rounded: `GetCompressedFileSizeW` for compressed or sparse files, or
    rounded to the cluster size from `GetDiskFreeSpaceW`);
  - file and folder counts, and "N folders could not be read";
  - with more than one item, a **per-item list** sorted by size (`ShowListChoice`); Enter goes to that item.
- **Pure core:** `DiskUsage.{h,cpp}`, with `SumTree(root, cancel, progress) -> {bytes, allocated, files, folders,
  unreadable}` and its own tests. A 100,000-file tree must stay responsive (PER-002).
- **Key and menu:** File → Used Disk Area. Default key `Alt+Shift+Enter` (Total Commander's), as a new `KeyMap`
  command `disk-usage`.

### 2. Generate Check Code — FOP-021

- **Algorithms:** CNG `BCryptOpenAlgorithmProvider` / `BCryptHashData`, streamed in 1 MB chunks with cancel and byte
  progress: MD5, SHA-1, SHA-256 (default) and SHA-512.
- **Create form:**
  - algorithm;
  - "one checksum file for all" (`<folder>.sha256`) or "one file per file" (`<name>.sha256`);
  - target: the other panel's folder or the same folder.
- **File format:** the GNU/Total Commander text format, `<hex>␠*<relative name>`, in UTF-8, so `sha256sum -c` can
  check it.
- **Verify:** when the cursor is on a `.md5`/`.sha1`/`.sha256`/`.sha512` file, the same menu item offers **Verify**.
  It reads the file, hashes each listed file and shows OK / FAILED / MISSING with counts, in a results list.
- **Pure core:** `CheckSum.{h,cpp}`, with `HashFile`, `FormatSumLine`, `ParseSumFile` (both `*` and space forms, CRLF
  and LF) and `VerifyPlan`, plus their own tests. The test vectors are the empty string and "abc" for every
  algorithm.
- **Key and menu:** File → Generate Check Code, menu-only (`MenuItemKind::CheckCode`), no default key.

### 3. Copy Selected Names to Clipboard — SEL-008

- **Choice dialog:** **N**ames / **F**ull paths / full paths with **D**etails (tab-separated size and date). The
  last choice is remembered for the session.
- **Clipboard:** `OpenClipboard` / `EmptyClipboard` / `SetClipboardData(CF_UNICODETEXT)`, with CRLF line endings,
  for the selection or the cursor entry (`..` excluded).
- **Code:** the `Clipboard.{h,cpp}` wrapper is the only Win32 clipboard code (CON-004). The text building,
  `FormatNamesForClipboard`, is pure and tested.
- **Plugin panels** (ZIP/FTP/SFTP): out of scope (they own their content); the item says so.
- **Key and menu:** Selection → Copy Selected Names to Clipboard. Default key `Ctrl+Shift+C`, as a new command
  `copy-names`.

### 4. Disk Label — NAV-013

- **Dialog:** the active panel's volume (UI-003 `VolumeInfo`), its current label pre-filled in a text prompt, then
  `SetVolumeLabelW(root, newLabel)`. An empty label removes it.
- **Errors:** `ERROR_ACCESS_DENIED` (system/fixed drives need admin) offers **Elevate & Retry**, through a new
  `ElevatedAction::SetVolumeLabel` (`--elevated-label <root> <label>`). It's the same single-operation UAC pattern
  as SEC-003; the app itself is never elevated (SEC-001/002).
- **Not supported:** network and read-only volumes show the Win32 error.
- **Key and menu:** Commands → Disk Label, menu-only.

### 5. Network Connection — NAV-014

- **Connections list** (`ShowListChoiceWithKeys`): the current connections from `WNetOpenEnumW(RESOURCE_CONNECTED)`,
  shown as `Z:  \\server\share  (persistent)`, plus a last row **+ Map network drive**.
  - **Enter** opens the drive in the active panel.
  - **Del** disconnects (`WNetCancelConnection2W`, with a confirmation; a second confirmation if files are open).
- **Map form:**
  - drive letter (a choice of the free letters);
  - `\\server\share`;
  - "Reconnect at sign-in" (`CONNECT_UPDATE_PROFILE`);
  - optional user name and a **masked password** (`FormField::masked`). The password is never logged or stored.
    Windows itself may keep it if the user ticks "remember", which uses `CONNECT_SAVECRED` and is documented.

  On success the active panel goes to the new drive.
- **Code:** `NetDrives.{h,cpp}` holds the `WNet*` calls; a pure `FormatConnectionRow` and the UNC validation are
  tested. Linked with `mpr.lib`; no elevation needed.
- **Key and menu:** Network → Network Connection, menu-only.

### 6. Share Folder / Unshare Folder — NAV-015

- **Why it is different:** creating or removing an SMB share needs administrator rights. SEC-002 forbids elevating
  for ordinary operations, so each share action is **one explicit, confirmed, elevated operation**: a new
  `ElevatedAction::ShareFolder` / `UnshareFolder` runs `NetShareAdd` / `NetShareDel` (level 2/502) in the existing
  elevated helper. The UAC prompt is the confirmation; the main process stays unelevated.
- **Share form:** share name (defaults to the folder name, validated), description, "Read only" or "Full access"
  for Everyone. Full access is not the default.
- **Unshare:** lists the shares of the current folder (`NetShareEnum` needs no admin) and asks for confirmation.
- **Status line:** shows `[shared as NAME]` for a shared folder (optional, open question 4).
- **Key and menu:** Network → Share Folder / Unshare Folder, menu-only.

### 7. View Full / List — UI-017 (brief view)

- **Per-panel mode** `Full` (today's detailed rows) or `Brief`. Brief draws names only, in columns
  `ceil(entries / rows)`, each `max(12, min(longest name, width/2))` wide, cut via `TextWidth.h`.
- **Brief-mode keys:**
  - **←/→** move one column;
  - **↑/↓** move one entry (wrapping into the next column);
  - **PgUp/PgDn** move one page of columns.

  Selection, the quick filter and the sort order are unchanged.
- **Geometry:** a pure `BriefLayout(entryCount, rows, width, longestName) -> {columns, columnWidth}` and an
  `EntryAt(col, row)` mapping in `Navigation.cpp`, both tested. `HitTestPanel` uses it for mouse clicks in Brief
  mode.
- **Persistence:** remembered per panel in the Session file (as UI-013's single-panel flag). Plugin and tree panels
  ignore it.
- **Keys and menu:** View → Full / List (the existing mnemonics F/L). New commands `view-full` and `view-brief`,
  default **`Ctrl+F7` / `Ctrl+F8`**. Both are free today. Total Commander's `Ctrl+F1`/`Ctrl+F2` are taken: `Ctrl+F1`
  is the single-panel toggle (UI-013).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/DiskUsage.{h,cpp}`, `CheckSum.{h,cpp}`, `Clipboard.{h,cpp}`, `NetDrives.{h,cpp}`, `NetShares.{h,cpp}` (new) + tests | Cores and Win32 wrappers |
| `Source/MyCommander/Navigation.{h,cpp}` | `BriefLayout`, `EntryAt`, Brief hit test |
| `Source/MyCommander/Elevation.{h,cpp}` | `SetVolumeLabel`, `ShareFolder`, `UnshareFolder` actions + argv dispatch (tested like the existing actions) |
| `Source/MyCommander/main.cpp` | The 9 actions, the brief drawing and keys |
| `Source/MyCommander/MenuBar.h`, `KeyMap.{h,cpp}`, `PluginManagerLogic.cpp` | Real items, new commands, shortcut inventory |
| `Source/MyCommander/Session.{h,cpp}` | Per-panel view mode |
| Both `.vcxproj` | New sources; `mpr.lib`, `netapi32.lib`, `bcrypt.lib` |
| `Help.cpp`, `README.md`, `USERGUIDE.md`, `CLAUDE.md`, spec, changelog, `Version.h` | Docs; FOP-020/021, SEL-008, NAV-013…015, UI-017 |

## Tests

- **Pure:**
  - `DiskUsageTests`: a `TempDir` tree with a junction (not followed) and an unreadable folder; cancel;
  - `CheckSumTests`: known vectors for 4 algorithms, format and parse round trip, a verify plan with a missing file
    and a mismatch;
  - `ClipboardFormatTests`;
  - `NetDrives` UNC validation;
  - `BriefLayout`/`EntryAt`/hit test;
  - `Elevation` argv dispatch for the 3 new actions (no real UAC);
  - `Session` view-mode round trip;
  - `KeyMap` new defaults.
- **Manual:**
  - clipboard paste into Notepad;
  - label change on a USB stick, and on C: (UAC);
  - map and disconnect a share;
  - share and unshare (UAC);
  - Brief mode with 1,000 entries, the mouse and a narrow window.

## Phasing

1. Copy names, Used Disk Area, Check Code. Local, no elevation, the most useful.
2. View Full/List (Brief mode).
3. Disk Label and Network Connection.
4. Share / Unshare (elevated actions).

Each phase is shippable: items not yet done stay stubs (MENU-003).

## Open questions (resolved 2026-10-13, per the recommendations above)

1. **Keys:** `Alt+Shift+Enter` (disk usage), `Ctrl+Shift+C` (copy names), `Ctrl+F7`/`Ctrl+F8` (Full/Brief) — used
   as proposed. The others stay menu-only.
2. **Checksum default:** SHA-256, GNU format — used as proposed.
3. **Share / Unshare:** implemented with one UAC prompt per action (the recommended option), via
   `Elevation.h`'s new `ShareFolder`/`UnshareFolder` actions.
4. **Status line `[shared]` marker:** not implemented — out of scope for this pass; `Unshare Folder` itself
   already lists a folder's current shares on demand (`NetShareEnum`, no admin needed), which covers the
   same need without a persistent status-line addition. Noted here as a possible small follow-up, not a gap
   in what was asked.

## Status

Planned 2026-10-12. **Implemented 2026-10-13** — all four phases. New modules: `Clipboard.{h,cpp}`,
`DiskUsage.{h,cpp}`, `CheckSum.{h,cpp}`, `NetDrives.{h,cpp}`, `NetShares.{h,cpp}`, each with its own
`*Tests.cpp`. Full solution build succeeds (0 errors); `MyCommanderTests.exe` reports 811 test cases, 0
failed. Spec rows FOP-020/021, SEL-008, NAV-013/014/015, UI-017 all marked Implemented in
`AIPrompt/my-commander.md` (v1.29.0). See `3_Reports/ChangeLogs/changelog_2026-10-13.md` for the full
change breakdown. Not committed yet.
