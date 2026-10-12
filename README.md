# MyCommander

A keyboard-driven, dual-pane terminal file manager for Windows, inspired by Total Commander. Native C++, built
directly on the Win32 Console API — no third-party TUI library.

Full requirements and design decisions live in [`AIPrompt/my-commander.md`](AIPrompt/my-commander.md). For a
plain, end-user-focused walkthrough with no build/development content — the document meant to be installed
and read alongside the application itself — see [`USERGUIDE.md`](USERGUIDE.md).

## Version

**1.9.1** — bump `Source/MyCommander/Version.h` to change it; the `.rc` file's embedded Windows
file version (visible in Explorer's file Properties > Details tab), `MyCommander.exe --version`,
and the terminal window/tab title (`MyCommander - <version>`, set once at startup) all derive from
that single header, so there is nothing else to update.

## Status

Dual-pane navigation, drive selection, selection, file operations, filename search/filtering, the internal
command line, and a minimal per-user configuration file are implemented and covered by automated tests.

- **Navigation:** two independently navigable panels, long-path/Unicode/UNC-safe enumeration (including
  correctly stopping "go to parent" at a UNC share root, not one level too deep at an unlistable
  `\\server\`), cursor position preserved across refresh, typing or pasting an absolute/relative/UNC path
  into the command line to jump straight there, and a full-screen drive picker (`Alt+F1`/`Alt+F2`) covering
  every fixed disk, removable drive, mapped network share, optical drive, and RAM disk Windows exposes.
- **Location history:** each panel keeps independent, in-memory Back/Forward history (`Alt+Left`/`Alt+Right`);
  navigating to a new location clears that panel's forward history.
- **Location restore:** after a normal exit, each panel's location is restored on the next launch. Explicit
  command-line paths take precedence. The separate session file stores only those two cosmetic paths, is written
  only after the normal input loop exits, and never stores operations, selections, or commands.
- **Panel headers:** each panel always shows its current path and current volume details: root, optional label,
  drive type, and free/total space where Windows can query it.
- **Same-directory sync:** when both panels happen to show the exact same directory, an operation performed via
  one panel (rename, create, delete, attribute toggle, execute, edit, or a plugin's own refresh request) keeps
  the other one in sync automatically, rather than it silently going stale.
- **Large directories:** panel enumeration runs in bounded batches on the input loop, so keys and resizing remain
  responsive while a large or slow directory loads. A `[Loading...]` header marker is shown until sorting finishes.
  Validated at 100,000+ entries (PER-003): only the visible row range is ever rendered regardless of listing
  size, so memory use and redraw cost both stay proportional to entry count rather than growing unbounded — a
  live 100,000-file directory loaded and stayed fully responsive (scrolling, sorting) at under 40 MB working set
  for the whole app.
- **Context-sensitive hints:** the bottom bar changes to match command entry, filters, selections, directories,
  files, empty/inaccessible locations, and shows the currently relevant function-key actions. It grows to as
  many rows as needed to show every command for the active context at the current terminal width — recomputed
  on every redraw, so both a resize and a context change (e.g. selecting an item) take effect immediately —
  rather than silently truncating commands off the edge of a single fixed row.
- **File rows:** name, size, type (file extension), modification time, and attributes
  (read-only/hidden/system/archive) — columns drop out gracefully on a narrow terminal rather than squeezing
  the name unreadably thin; see the keybindings table below for the exact layout.
- **Hidden/system entries:** hidden and system files are excluded by default. `Ctrl+H` independently toggles
  their visibility in the active panel; an active `[Hidden+system]` header marker makes the mode unambiguous.
- **Sorting:** each panel keeps its own sort key (name/extension/size/modification time) and direction, toggled
  with `Ctrl+F3`-`Ctrl+F6`; directories always sort before files.
- **Inaccessible/broken locations:** an unavailable path (removed drive, unreachable network share, permissions
  error) is shown as a clear banner filling that panel's body regardless of which panel is active, and a single
  broken entry (e.g. a dangling reparse point) is marked individually rather than shown as an ordinary file.
  Junctions, symbolic links, and other reparse points are labelled `LINK`/`<REPARSE>` and cannot be entered as
  panel locations, preventing navigation loops; copy, search, and permanent delete also never recurse into them.
- **Selection:** toggle/select-all/invert/clear, with a selected-count and total-size summary in the status bar.
  A selected entry is marked with a leading `*` in the listing, not just a different color, so it's identifiable
  even without color (ACC-001).
- **Mouse (optional):** click to move the cursor and activate a panel, double-click to open, wheel-scroll,
  drag-and-drop a selection between panels to copy it (Shift to move), right-click/drag to select files,
  Ctrl+left-click or Ctrl+right-click to select a range, click a bottom-bar hint to run it, and click a
  dialog option to choose it — never required, every feature stays keyboard-reachable (UI-012/KEY-001,
  IS-0003/IS-0004) — see [Running](#running) above.
- **File operations:** copy, move, rename, create a directory or an empty file (the new item is selected once
  the panel refreshes), and delete — with conflict
  resolution (overwrite/skip/rename/cancel, apply-to-all) for name collisions and, separately, a retry
  dialog (retry/skip/skip all/cancel, plus elevate & retry specifically for a permissions failure — see
  [Privilege elevation](#privilege-elevation) below) for a failure that isn't a name collision (a locked
  file, a full disk, and the like), Recycle Bin by default (permanent delete
  as an explicit separate action), and guards against copying/moving a directory into its own subtree or
  recursing into reparse points/junctions. File attributes can be toggled with `Ctrl+A`. Every reported
  failure names both the affected path and what was being done to it — including the exact nested file
  inside a recursive copy/move/delete, not just the top-level item you selected (ERR-001). Copy, move,
  delete (Recycle Bin and permanent alike, IS-0018 — each selected item is sent to the Recycle Bin on its own,
  so a single failing item gets its own Retry/Skip instead of failing the whole selection), and attribute
  toggling all show live per-item progress and honor `Esc` promptly —
  including mid-file during a large copy, and mid-tree during a large recursive permanent delete — so
  none of them ever run to completion with a frozen screen and no way to interrupt (PER-002). Progress is shown
  as a centered dialog (source/destination, current item, and — for copy/move, once a file is large enough for
  Windows to report incremental progress on it — the transferred-bytes/total and a live transfer speed, FOP-009),
  matching the look of the app's other modal dialogs rather than a one-line status-bar overlay. Progress
  reporting itself is rate-capped (~20 Hz) so it cannot measurably slow a transfer down, no matter how
  often the OS's own copy-progress callback actually fires (PER-005).
- **View/Edit:** a built-in scrollable text viewer with forward search and a hex view (VEE-005), and external-editor launch (`%EDITOR%`,
  falling back to `notepad.exe`). By default (`waitForEditorToClose = false`) `F4` launches the editor and
  returns immediately — MyCommander stays usable right away, and the edited file's panel entry refreshes and
  stays selected automatically once you save, via a Windows file-change notification. Set `waitForEditorToClose
  = true` to restore the original wait-for-close behavior — needed for a console-based `%EDITOR%` sharing this
  same window, which the non-blocking mode doesn't hand off to cleanly.
- **File properties:** `Alt+Enter` (also reachable from the top menu bar's View → Executable properties)
  shows the cursor file's name, size, and modification time, and — for a runnable PE file
  (`.exe`/`.dll`/`.ocx`/`.sys`/`.cpl`/`.scr`/`.drv`) — its embedded version info (description, file/product
  version, product name, copyright) and a best-effort digital-signature check (not signed, or signed with a
  short signer name; correctly follows catalog-signed system files, not just embedded-signed ones).
- **Search/Filter:** a per-panel quick name/wildcard filter (`Ctrl+F`), and a recursive Find Files search
  (`Alt+F7`) whose results are navigable in a picker — jump the active panel straight to a match, or view a
  file result without leaving the picker.
- **Command line and shell:** a persistent internal command line, always visible above the status bar, that
  runs commands in the active panel's directory, plus an option to open a separate interactive shell there — see
  [Command line and shell](#command-line-and-shell) below.
- **Configuration:** a minimal per-user config file controls whether deleting to the Recycle Bin and permanent
  delete each ask for confirmation, what plain `Enter` on a file does (view/edit/execute), whether command-line
  history persists across runs, and an ASCII fallback for frame glyphs. `Ctrl+F11` opens it in the external
  editor and applies your changes live once you save, without a restart; a setting missing from an older file (one
  written before a newer version added it) is added back automatically on the next startup — see
  [Configuration](#configuration) below.
- **Help:** `F1` opens a full-screen, scrollable, in-application reference listing every keyboard shortcut,
  grouped by category — the same information as the keybindings table below, without leaving the app. It opens
  with an "About" section first (product name, version, description, company, copyright), read at runtime from
  the running executable's own embedded Windows file-version resource — the same data Explorer's Properties >
  Details tab shows — rather than a second hand-maintained copy of those strings. The mouse wheel scrolls it too,
  3 rows per notch, matching the dual-pane panels' own wheel granularity.
- **Diagnostic logging:** off by default; an optional, severity-filtered log of app start/exit, config-file
  problems, and failed file operations, never typed command text or credentials — see
  [Diagnostic logging](#diagnostic-logging) below.
- **Privilege elevation:** always runs unprivileged; a Copy/Move/Delete/attribute-toggle failure that's
  specifically access-denied offers an "Elevate & Retry" choice, scoped to that one item via a short-lived
  elevated helper process, never the running session itself — see
  [Privilege elevation](#privilege-elevation) below.
- **Testing:** an automated test project (`MyCommanderTests`) exercises path handling, panel selection and
  sorting, conflict resolution, file-operation planning, text-loading, and search logic directly (TST-001),
  including Unicode filenames, long paths beyond `MAX_PATH`, large files, hidden files, read-only files,
  reparse points, simulated access failures (TST-003), and a 10,000-entry directory load with a working-set
  memory-growth bound (TST-006, partial — large directories are covered; multi-gigabyte transfers aren't yet),
  plus the terminal-UI decision logic for navigation (Tab/panel history/command-history browsing/cursor
  movement) and the two general-purpose dialog primitives (hotkey matching, text-prompt editing) that every
  conflict/error/confirmation/rename prompt in the app is built on (TST-005) — see [Testing](#testing) below.
- **Localization-ready text:** every user-facing dialog title/prompt/label/message is defined in
  `Source/MyCommander/Strings.h`, separate from the logic that decides when to show it (ACC-003) — the same
  groundwork `HintBar.h` and `Theme.h` already laid for hint-bar text and colors. Actual non-English
  translation is a separate, not-yet-started effort (ACC-005).
- **Color themes:** `default` or `highcontrast` (`colorTheme` — see [Configuration](#configuration)). The
  high-contrast theme pushes every pairing the default theme only just cleared WCAG AA on to the maximum
  achievable contrast — plain text and the inactive panel header go from light gray to bright white, and the
  status bar drops its green background entirely (UI-010).
- **Menu bar:** an always-visible top row (`File Selection Commands Network View Settings`) opened with
  `Alt+<letter>` and fully mouse-clickable, offering a second, discoverable path to actions that already have
  their own key (never the *only* way to reach anything, KEY-001) alongside a handful of items reserved for
  features MyCommander doesn't have yet (FTP and the like — these show "Not yet
  implemented" rather than pretending to work) — see [Menu bar](#menu-bar) below.
- **FTP (EXT-005):** a standalone MC Plugin (`Source/FtpPlugin/`) adds Network → "FTP connection" (saved
  connections: connect/edit/delete/new) and "New FTP connection" (host or `ftp://` address, port, user, masked
  password, start folder, passive mode, timeout). A connected server is shown in the active panel: browse, `F3`
  view, `F5`/`F6` download (copy/move) to the other panel, `F5` from the other panel uploads, `F7`/`F2`/`F8`
  create/rename/delete (always confirmed), `Ctrl+R` refresh, conflict choices, progress and `Esc`, reconnect on a
  dropped connection, bounded timeouts. Its own minimal FTP client on Winsock2 (DEC-012: EPSV/PASV or active,
  MLSD or Unix/Windows LIST, UTF-8 names), no third-party code. Passwords: never on screen, in the log or in
  `ftp-connections.conf`; optionally in the Windows Credential Manager (DEC-013). Plain FTP is unencrypted
  (use SFTP below for anything sensitive); FTPS is not supported yet. Tests: `Source/FtpPlugin/FtpPluginTests.cpp`
  (protocol, against an in-memory loopback server) and `FtpPanelTests.cpp` (the panel through a fake host).
- **SFTP (EXT-004):** a standalone MC Plugin (`Source/SftpPlugin/`) adds Network → "SFTP connection", "New SFTP
  connection" (host or `sftp://` address, port 22, user, log in with **password**, **private key file** or the
  **SSH agent**, masked password/passphrase, start folder, timeout) and "Disconnect SFTP" (while connected). The
  server panel, transfers, conflicts, reconnect and `Esc` are the same as FTP's: both plugins share one remote-panel
  implementation (`Source/PluginCommon/`). Host keys are verified (DEC-015): the first connection shows the
  server's `SHA256:` fingerprint (Trust and remember / Once / Cancel), keys you already trust in
  `%USERPROFILE%\.ssh\known_hosts` are accepted, and a **changed** key is always refused. Symbolic links to
  folders open with `Enter` but are never copied into or deleted through. SSH comes from vendored
  [libssh2](https://libssh2.org) 1.11.1 (BSD-3-Clause, DEC-014) built with Windows' own CNG cryptography — no
  OpenSSL. Key files: RSA in PEM format (with or without a passphrase) or ECDSA; Ed25519 and other OpenSSH-format
  keys work through the SSH agent. Passwords and passphrases: never on screen, in the log or in
  `sftp-connections.conf`; optionally in the Windows Credential Manager. Tests: `Source/SftpPlugin/BuildTests.cmd`
  runs `SftpLogicTests` (pure logic), `SftpPanelTests` (the plugin and panel on an in-memory server) and the
  opt-in `SftpIntegrationTests` (a real server, via `MC_SFTP_TEST_*` environment variables).
- **ZIP archives (pack, unpack, and live browsing):** a standalone MC Plugin (`Source/ZipPlugin/`) adds
  "Pack file" and "Unpack Package" commands to the File menu, plus the ability to step into a `.zip` file
  (`Enter`) and browse it like a directory (ARC-001). ZIP-based packages — `.jar`, `.war`, `.ear`, `.apk`,
  `.xpi`, `.vsix`, `.nupkg`, `.whl` — are browsed and unpacked the same way; Office/ODF/EPUB documents are
  not claimed, so `Enter` keeps opening them. Packing offers a dialog with compression level,
  recurse-into-subdirectories (default on), a maximum archive size, and splitting into multiple archives
  when that size would be exceeded; every packed entry keeps its own Windows file attributes, restored on
  unpack; both pack and unpack show a cancellable progress dialog and clear the source panel's selection
  once done, matching Copy/Move's own convention. Unpacking extracts a selection of `.zip` files into the
  other panel's directory (one subfolder per archive when extracting more than one at once). While
  browsing an archive: `Insert`/right-click/Ctrl+click select entries, `Shift+Insert` adds the other panel's
  selection into the archive (pressing `F5` from the *other* panel, with the archive open, does the same
  thing — so Copy onto an open archive packs the selection in rather than copying beside it on disk),
  `F5`/`F6` extract/move the selection to the other panel (name collisions offer
  overwrite/overwrite all/skip/skip all/cancel, the same as the host's own Copy/Move), `Shift+F5` copies
  the current item (file or folder) under a new name or relative path *inside* the archive, `F7` creates a
  directory (a real, persisted empty directory that survives closing and reopening the archive, not just a
  virtual folder), `F8`/`Delete` removes entries from the archive, `Ctrl+A` toggles an attribute, `Ctrl+R`
  reloads the archive from disk, and `F3` views a file without extracting it. Edits are batched in
  memory and the archive is only actually rewritten once, when you leave it — not after every single
  change — with a save/discard/cancel prompt if anything is pending (a `*` after "Name" in the header
  marks unsaved changes); quitting the app with unsaved changes still open auto-saves them as a safety
  net. A third File menu item, "Archive info", shows file/directory counts, uncompressed/compressed size, and
  compression ratio — for a `.zip` file merely selected in a normal panel, or for whichever archive is
  currently open in the active panel — see
  [`AIPrompt/features/FEA-0007-plan.md`](AIPrompt/features/FEA-0007-plan.md).
- **Plugins (MC Plugin, all 3 phases):** native, DLL-based plugins dropped into a `Plugins` folder next to
  `MyCommander.exe` are discovered and loaded automatically, and can add menu items, register keyboard
  shortcuts, show dialogs, log, read/react to the color theme, contribute their own Markdown-rendered
  content to the F1 shortcut-reference screen (each plugin's own block is preceded by a `====` selector
  row so multiple plugins' help text never run together), take over a main panel's own content/input, and
  publish or discover services to talk to each other — see
  [`Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md`](Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md) if
  you want to write one. A problematic plugin can be disabled by id via `disabledPluginIds` in the config
  file, without removing its DLL.

See the SRS's MVP checklist in [`AIPrompt/my-commander.md`](AIPrompt/my-commander.md) and the latest report under
[`3_Reports/`](3_Reports/) for exact per-requirement progress.

## Requirements

- Windows 10/11
- Visual Studio 2022 (Professional or any edition with the "Desktop development with C++" workload), toolset v143
- Windows 10 SDK

## Building

Open `MyCommander.sln` in Visual Studio 2022 and build (`Debug|x64` or `Release|x64`), or from a Developer
Command Prompt / PowerShell:

```powershell
& "C:\Program Files\Microsoft Visual Studio\2022\Professional\MSBuild\Current\Bin\MSBuild.exe" `
    MyCommander.sln /p:Configuration=Release /p:Platform=x64
```

The executable is written to `Build\x64\<Configuration>\MyCommander.exe`.

## Installing

Building the solution also produces `Build\x64\<Configuration>\MyCommanderSetup.exe` — a small,
native C++ installer (`MyCommanderSetup`, a third project in `MyCommander.sln`, INST-001 through
INST-006). It checks the target machine can run MyCommander (Windows version, disk space), installs
per-user to `%LOCALAPPDATA%\Programs\MyCommander` (no administrator rights needed), offers a desktop
shortcut, and registers the app with Windows' "Apps & Features" so it can be uninstalled cleanly
from there. See [`USERGUIDE.md`](USERGUIDE.md#installing) for the end-user-facing walkthrough.

Every build of `MyCommanderSetup` first force-rebuilds `MyCommander` as `Release|x64`
(`Tools/RebuildReleaseForInstaller.ps1`, its own `PreBuildEvent`) and embeds that exe — plus
`USERGUIDE.md`/`LICENSE` — as `RCDATA` resources, so the installer always ships a fresh, optimized
build regardless of which configuration `MyCommanderSetup` itself is built in (INST-006). The same
script also freshly rebuilds and bundles the ZIP plugin (`INST-007`) — the one real, user-facing
plugin today; `ExamplePlugin` is deliberately excluded, since it's a developer-facing template, not
a shipped feature — installed into the new install's own `Plugins` folder. Unlike the core app, a
failed plugin build only warns rather than blocking the installer, since MyCommander is fully usable
with no plugins installed at all. See
[`AIPrompt/features/FEA-0006-plan.md`](AIPrompt/features/FEA-0006-plan.md) for the full design,
including what's deliberately out of scope (all-users/elevated installs, silent/unattended mode,
MSI/WiX/Inno tooling).

`MyCommander.vcxproj`/`MyCommanderTests.vcxproj` link the MSVC runtime statically (`/MT`/`/MTd`)
specifically so the installed `MyCommander.exe` has no Visual C++ Redistributable dependency to
check for or bundle.

## Running

```powershell
Build\x64\Release\MyCommander.exe [leftPath] [rightPath]
```

With no arguments both panels start in the current directory.

Every feature is reachable from the keyboard alone (KEY-001) — mouse support (UI-012, below) is purely
additive, never the only way to do something; every dialog offers a hotkey or arrow-key/Enter/Esc path.

**Mouse (UI-012/IS-0003/IS-0004, optional — never required):** in either file panel, left-click moves the cursor to
the clicked entry and activates that panel if it wasn't already active; double-click additionally opens it —
identical to pressing `Enter` (a directory is entered; a file follows the configured default action, see
[Configuration](#configuration)). The mouse wheel moves the active panel's cursor, regardless of which panel
it's scrolled over. Dragging a selection (or the cursor entry, if nothing is selected) from one panel and
dropping it on the other copies it there — hold Shift while dropping to move instead; a status-bar line shows
while a drag is in progress, and dropping back on the source panel (or outside both) is a no-op. Clicking a
command in the bottom hint bar runs it, exactly as its key press would — a hint naming two different keys as
a slash pair (e.g. `Alt+Left/Right History`) isn't clickable, since a click can't say which one was meant.
Clicking a dialog's `[X]Label` option chooses it, same as its hotkey; clicking anywhere in a plain message
dialog dismisses it. Right-click toggles the clicked entry's selection mark; holding the right button and
dragging extends the selection to every entry the mouse passes over (confined to the panel the drag started
in) — deliberately the right button, not the left, since a plain left-drag already means copy/move above.
Ctrl+left-click selects every entry between wherever the cursor already was and the clicked entry; a
right-click followed by a Ctrl+right-click on a second entry selects the range between those two the same
way. Unlike Explorer, that anchor is always the cursor's current position, not a separately remembered one
that survives across multiple clicks; Ctrl+right-click is a single discrete gesture, not draggable. (This
uses Ctrl, not Shift — classic console host silently swallows a Shift-held mouse click before it ever
reaches the app, on either button, so Shift can't be used for these two gestures there. Shift still works
for the drag-and-drop copy/move above, at the moment of drop, since that's a different kind of event.)
The drive/search pickers, the viewer, and the help screen remain keyboard-only. The top menu bar (see
[Menu bar](#menu-bar) below) is also fully mouse-clickable — click a top entry to open it, click an item to
choose it, or click anywhere else to dismiss an open menu.

The interface adapts live to terminal resizing. The documented minimum supported size is **60x15**
(columns x rows); below that, the two-panel layout no longer has room to draw usably, so MyCommander
shows a plain "Terminal window too small" message instead — only `F10` (quit) is honored until the
terminal is resized back up.

## Implemented commands and actions

This is the complete keyboard-command reference for the current application. It is deliberately
self-contained so it can serve as the source for the separate `AIPrompt/Help.md` document. Copy, move, delete,
and attribute commands use the current selection; when nothing is selected, they use the cursor entry. All
commands are keyboard-only.

### Program and panel navigation

| Input | Action |
|---|---|
| `MyCommander.exe [leftPath] [rightPath]` | Start with zero, one, or two panel paths; without paths, both panels start in the current directory. |
| `MyCommander.exe --version` | Print the application version and exit. |
| `Tab` | Switch the active panel. |
| `↑` / `↓` | Move the cursor one entry. |
| `Page Up` / `Page Down` | Move the cursor one page. |
| `Home` / `End` | Move the command-line cursor to the start/end when it has text; otherwise move to the first / last entry. |
| `Alt+Left` / `Alt+Right` | Go back / forward through the active panel's location history. |
| `Enter` | Run non-empty command-line text; otherwise enter a directory, or use the configured default file action (view, edit, or execute). |
| `Backspace` | Erase the command-line character before the cursor when it has text; otherwise go to the parent directory, with the cursor on the directory just left (also for `Enter` on `..`, `Alt+Left`, and a typed parent path; IS-0019). |
| `Alt+F1` / `Alt+F2` | Open the drive picker for the left / right panel. |
| `Ctrl+D` | Directory hotlist: go to a named favorite folder, add the current one, remove (`Del`) or rename (`F2`) one. See [Directory hotlist](#directory-hotlist-nav-007). |
| `Ctrl+R` | Refresh (re-read) the active panel. |
| `Ctrl+T` | Show the active panel as a directory tree (UI-013); `Ctrl+T` again or `Esc` leaves it. See [Directory tree](#directory-tree-ui-013-part-b). |
| `Ctrl+F1` | Single-panel layout on/off: the active panel alone, full width; `Tab` switches which panel is shown (UI-013). See [Single-panel layout](#single-panel-layout-ui-013-part-a). |
| `Alt+F5` / `Alt+F6` | Queue the selection as a background copy / move (see [Background jobs](#background-jobs-fop-014)). |
| `Alt+F8` / `Alt+Shift+F8` | Queue the selection as a background delete — Recycle Bin / permanent, same confirmation as `F8` / `Shift+F8`. |
| `Alt+Q` | Show the background job queue. |
| `Ctrl+H` | Show or hide hidden and system entries in the active panel. |
| `Alt+F` / `Alt+S` / `Alt+C` / `Alt+N` / `Alt+V` / `Alt+E` | Open the File / Selection / Commands / Network / View / Settings menu — see [Menu bar](#menu-bar) below. |
| `F1` | Open the in-application shortcut reference. |
| `Esc` | Clear non-empty command-line text. |
| `F10` | Quit. |
| `Ctrl+F11` | Open the config file in the external editor; changes apply live once you save (IS-0006). |
| Settings → Edit key bindings | Open `keys.conf` to rebind shortcuts (KEY-005). The keys in these tables are the defaults; see [Key bindings](#key-bindings-key-005). |

### Selection, sorting, filtering, and search

| Input | Action |
|---|---|
| `Insert` | Toggle selection on the cursor entry, then move down. |
| `Shift+Up` / `Shift+Down` | Toggle selection on each entry as the cursor moves over it; retracing your steps toggles the same entries back off. |
| `Numpad +` / `Numpad -` | Select / deselect entries matching a prompted wildcard mask; the default `*` matches everything. |
| `Ctrl+Numpad +` / `Ctrl+Numpad -` | Select all / clear all selections without prompting. |
| `Numpad *` | Invert the selection. |
| `Ctrl+F3` | Sort the active panel by name; press again to reverse the direction. |
| `Ctrl+F4` | Sort by extension/type; press again to reverse the direction. |
| `Ctrl+F5` | Sort by size; press again to reverse the direction. |
| `Ctrl+F6` | Sort by modification time; press again to reverse the direction. |
| `Ctrl+F` | Filter the active panel by name or wildcard; submit an empty filter to clear it. |
| `Alt+F7` | Find files below the active panel: a form with a name mask and optional **containing text** (plain or regular expression, match case, encoding, skip binary). See [Content search](#content-search-src-004src-005). |
| `Shift+F2` | Compare the two panels' folders (name, size, timestamp, or content; optionally with subfolders) and select the differences in both panels. See [Compare folders](#compare-folders-src-006). |
| `Ctrl+Shift+F2` | Synchronize the two panels' folders (one way or both ways) after a preview and a confirmation. See [Synchronize folders](#synchronize-folders-src-007). |
| `Alt+F3` | Show the differences between two text files (File → Compare contents); `Shift+Alt+F3` always uses the built-in viewer. See [Compare contents](#compare-contents-src-008). |

### File actions

| Input | Action |
|---|---|
| `F2` | Rename the cursor entry. |
| `Ctrl+M` | Group rename the selection (or cursor entry) with masks, counters, search/replace and case, after a preview. See [Group rename](#group-rename-fop-017). |
| `F3` | View the cursor file: its default `view` action from [File actions](#file-actions), or the built-in text viewer if none is registered. |
| `Shift+F3` | Choose any registered `view`/`open` action for the cursor file (plus the built-in viewer and the Windows default). |
| `Shift+Enter` | Open the cursor file: its default `open` action, or its Windows file association if none is registered. With text in the command line, runs the command like `Enter`. |
| `F4` | Open the cursor file in the configured external editor (`%EDITOR%`, otherwise `notepad.exe`). |
| `Alt+Enter` | Show the cursor file's properties — size, modification time, and (for a runnable PE file) its version info and digital signature. |
| `F5` | Copy the items to the other panel's current directory. |
| `Shift+F5` | Copy the cursor entry under a new name in the same directory. A full path is used as given; an existing directory (or a name ending in `\`) receives the copy under its original name. |
| `F6` | Move the items to the other panel's current directory. |
| `F7` | Create a directory in the active panel. |
| `Shift+F7` | Create an empty file in the active panel. |
| `F8` / `Delete` | Delete the items to the Recycle Bin (when the command line is empty — `Delete` forward-deletes command-line text otherwise). |
| `Shift+F8` / `Shift+Delete` | Permanently delete the items, bypassing the Recycle Bin. |
| `Ctrl+A` | Toggle one chosen Windows attribute (read-only, hidden, system, or archive) on the items. |
| `F9` | Open a separate interactive shell in the active panel's directory. |

Copy, move, delete, and attribute changes show progress and can be cancelled with `Esc`. Name collisions offer
overwrite, skip, rename, or cancel choices (including apply-to-all); other per-item failures offer retry, skip,
skip-all, or cancel. Access-denied failures also offer an item-scoped **Elevate & Retry** option. Recycle-Bin and
permanent-delete confirmations are configurable.

### Command line

| Input | Action |
|---|---|
| Printable text or paste | Insert text into the always-visible internal command line at the cursor. |
| `Left` / `Right` | Move the command-line cursor one character, when it has text. |
| `Home` / `End` | Move the command-line cursor to the start / end, when it has text. |
| `Delete` | Forward-delete the character at the cursor, when the command line has text. |
| `Enter` with command-line text | If the text names an existing absolute, relative, or UNC directory, navigate the active panel there; otherwise run it through the configured shell in the active panel directory. |
| `Ctrl+Enter` | Insert the active item name, or selected item names, into the command line. |
| `Ctrl+Shift+Enter` | Insert the active item full path, or selected item full paths, into the command line. |
| `Ctrl+Up` / `Ctrl+Down` | Browse command history. |
| Leading space before `Enter` | Run or navigate using that text without recording it in command history. |

Command history is persisted by default and can be disabled with `persistCommandHistory` in the configuration. The
command line, the text-prompt dialog, and form-dialog text fields all share the same minimal line editor: a real
positionable cursor with insert, Backspace, and forward-Delete (`Left`/`Right`/`Home`/`End` move the cursor; word-jump,
in-line selection, and clipboard cut/copy/paste are intentionally not included).

### Commands inside temporary screens

| Screen | Available input |
|---|---|
| Drive picker (`Alt+F1` / `Alt+F2`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` choose a drive; `Enter` opens it; `Esc` or `F10` cancels. |
| Find Files results (`Alt+F7`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` choose a result; `Enter` opens it in the active panel; `F3` views a file result; `Esc` or `F10` closes the picker. |
| Compare results (`Shift+F2`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` choose an item; `Tab` cycles all / differences / left only / right only; `Enter` moves both panels to the item; `F3` / `Shift+F3` view the left / right file; `Alt+F3` shows the differences of an item present on both sides; `F5` continues to Synchronize, pre-filled from this compare; `Esc` or `F10` closes (the selections stay). |
| Synchronize preview (`Ctrl+Shift+F2`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` choose an item; `Space` switches its action off/on; `→` / `←` copy it to the right / left; `Del` deletes it on the target (only with *Delete files only on the target*); `Tab` cycles differences (the default — every item that differs, including ones with no proposed action, such as right-only items in Left → Right) / conflicts / all; `F3` / `Shift+F3` view; `F5` asks for confirmation and synchronizes; `Esc` or `F10` cancels without changing anything. |
| Built-in viewer (`F3`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` scroll; `/` starts a forward search; `n` repeats it; `H` switches between text and hex view; in hex view `↑` / `↓` / `←` / `→` / `Page Up` / `Page Down` / `Home` / `End` move the cursor (the view scrolls to follow it) and `W` cycles the items per row (8, 16, 32, or fit to the window); `Esc`, `F3`, or `F10` closes the viewer. |
| Shortcut reference (`F1`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End`, or the mouse wheel scroll; `Esc`, `F1`, or `F10` closes it. |

In prompts and choice dialogs, use the displayed hotkey or `↑` / `↓` with `Enter`; `Esc` cancels where cancellation
is available. A name pattern with `*` or `?` uses wildcard matching; a pattern without either is a substring match.

Each row shows an attribute column — `R`/`H`/`S`/`A` for Read-only/Hidden/System/Archive, `-` where that
attribute is unset (e.g. `R-S-`). `Ctrl+A` flips one of them (your choice, from a dialog) on every selected
entry, or the cursor entry if nothing is selected; toggling Read-only off a file that's currently blocking
overwrite/delete is the most common reason to reach for it. Like copy/move/delete, a failure here also gets
the retry dialog (retry/skip/skip all/cancel — ERR-003) rather than just being recorded and skipped past.

A directory or file's **type** (its extension, without the leading dot — directories show nothing here,
since `<DIR>` already marks them in the size column), **attributes**, **modification time**
(`YYYY-MM-DD HH:MM`, local time), and **size** are shown after the name, in a configurable order with
configurable widths (`visibleColumns`/`typeColumnWidth`/`mtimeColumnWidth`/`sizeColumnWidth` — see
[Configuration](#configuration); attributes stays a fixed 4 characters, one slot per read-only/hidden/
system/archive flag). On a narrow terminal, columns drop out gracefully one at a time from the end of the
configured order — with the default order (type, attributes, modification time, size), size is the first to
go, then modification time, then attributes, leaving just type; below that, even type drops and only the
name is shown (UI-005). This is a deliberate space trade-off, not a bug.

`Alt+F1`/`Alt+F2` open a full-screen drive picker for the left/right panel respectively — every logical drive
Windows currently exposes (fixed disks, removable media, mapped network shares, optical drives, RAM disks),
each shown with its type, volume label, and free/total space where queryable (NAV-004). `↑`/`↓`/`Page Up`/`Page
Down`/`Home`/`End` move the cursor, `Enter` jumps that panel straight to the chosen drive's root and makes it
the active panel, and `Esc`/`F10` cancel without changing anything.

Each panel keeps its own sort key and direction, independently of the other panel's (NAV-005). `Ctrl+F3`
through `Ctrl+F6` sort the active panel by name, extension, size, or modification time respectively
(`LIST-002`); pressing the same one again reverses the direction, ascending first (`LIST-003`) — the familiar
click-the-same-column-header convention. The panel header shows the current key and direction (e.g. `Size↓`)
next to the path. Directories always sort before files regardless of key, and `..` always stays first.

An unavailable location — a removed drive, an unreachable mapped network share, a permissions error — is shown
as a clearly marked banner filling that panel's own body, not just mentioned in the shared bottom status bar
(which only ever reflects whichever panel is currently active, so the *other* panel silently becoming
unreachable would otherwise look identical to an ordinary empty directory) (LIST-006). Within an otherwise
normal listing, a single broken entry — most commonly a reparse point (junction/symlink) whose target no
longer exists — is marked individually: shown in a distinct color with `<BROKEN>` in place of its size, rather
than silently as an ordinary 0-byte file.

All on-screen text (panel rows, dialog boxes, the command line, status/hint bars) is column-aligned by actual
display width rather than character count, so full-width East Asian characters (CJK ideographs, hiragana,
katakana, fullwidth Latin forms) — which occupy two terminal columns, not one — don't throw off alignment, and
truncation never cuts one of them in half (UI-011). One known limitation, inherent to the raw
`WriteConsoleOutputW` buffer API this app is built on (DEC-002): each screen cell holds exactly one UTF-16 code
unit, so combining marks and supplementary-plane characters (most emoji, encoded as surrogate pairs) can't be
composed into a single glyph the way a modern terminal emulator's own text renderer would — they display as
separate single-width cells instead.

Inside the built-in viewer (`F3`): `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` scroll, `/` searches forward,
`n` repeats the last search, and `Esc`/`F3`/`F10` close it.

**Hex view (VEE-005).** Press `H` in the viewer to switch to a hex dump: a 16-digit offset, the bytes in hex, and
an ASCII column (non-printable bytes shown as `.`). The position carries over both ways, so a line you were reading
is the first row in hex view and vice versa.

Hex view has its own cursor, highlighted in both the hex digits and the ASCII column for the same byte. `↑` / `↓`
move it one row, `←` / `→` move it one byte, `Page Up` / `Page Down` move it one screen, and `Home` / `End` jump it
to the start/end of the file; the view scrolls (and pans, for a row wider than the terminal) only as far as needed
to keep the cursor visible. The status line's offset is always the cursor's own position, not just the first
visible byte. `W` cycles the items per row through 8, 16, 32, and **fit**, which picks the largest of 8, 16, or 32
that fits the window (8 is the default each time the viewer opens); the cursor's byte value doesn't change when you
switch. Only the visible bytes are read, so large files stay responsive.

Search works in both views. A plain query searches for the text (ASCII, case-insensitive; in hex view its UTF-8
bytes), and a query starting with `#` searches for hex bytes: `#4D 5A` or `#4d5a`. In hex view a match is highlighted
until your next key press. The search prompt's title always shows the `#hex` form.

### Content search (SRC-004/SRC-005)

`Alt+F7` opens a form: **File name mask** (default `*`), **Containing text** (empty = the name-only search it always
was), **Regular expression**, **Match case**, **Encoding** (Auto / UTF-8 / ANSI / UTF-16LE / UTF-16BE) and **Skip
binary files** (default on), remembered for the session. Plain text is matched case-insensitively with Unicode case
folding; a regular expression uses the standard library's ECMAScript `std::wregex` (an invalid one is reported before
anything is searched). Matching is line by line; each file is read once in fixed-size chunks (bounded memory) and
reading stops at its first match, which is what the results show: `path  :line:  text`. `F3` opens the viewer at that
line with a plain-text term pre-loaded for `n`. A live progress line counts files; `Esc` is honored between lines.
Unreadable (locked/permission-denied) and binary-skipped files are counted in the results title. Limits that keep a
bad pattern or a huge one-line file from stalling the app (a single `std::regex_search` call can't be interrupted):
plain text looks at a line's first 65 536 characters, a regex at its first 1 024, and a regex gives up on a line after
200 000 backtracking steps (MSVC's `_REGEX_MAX_COMPLEXITY_COUNT`, set in `ContentSearch.cpp` only).

Inside the Find Files results picker (`Alt+F7`): `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` move the cursor,
`Enter` jumps the active panel straight to the selected entry, `F3` opens a file result in the viewer without
leaving the picker, and `Esc`/`F10` close it. A filter or search pattern with no `*`/`?` is treated as a
substring match (e.g. `log` matches `app.log.txt`); with `*`/`?` it's matched as a classic wildcard. A directory
result is shown with a trailing `\`, not just a different color, so it's identifiable even without color
(ACC-001).

### Compare folders (SRC-006)

`Shift+F2` (also Selection → Compare Folders) compares the left and right panels' folders. A form chooses **Compare
by** (Name / Name + size / Name + size + timestamp, the default / Name + size + content), **Include subfolders**
(off), **Timestamp tolerance** (2 s, for FAT/exFAT and network shares), **Ignore exact 1-hour difference** (DST or
FAT local-time shifts) and **Show the results list** (on), remembered for the session.

- Each side lists what its panel would show: hidden/system entries only if that panel shows them, and that panel's
  quick filter applied to files (never folders). Names match case-insensitively. Junctions and symbolic links are
  compared as entries and never followed or read (SEC-006).
- **Content** compares the two files byte by byte in 64 KB chunks and stops at the first difference — exact, with no
  checksum collisions; files of different sizes are never read. With content, timestamps only decide which side is
  newer, they never make equal files "different".
- With subfolders, a folder is identical only if everything inside it is.
- `Esc` cancels (also mid-file); the progress line shows items and bytes read. At most 200 000 items are compared.

Afterwards, like Total Commander, **each panel's selection is replaced** by its differences: items only on that
side, the newer side of each pair, and both sides of a pair that differs without a newer side (or a folder with
differences inside). `F5` straight away therefore copies what's missing or newer to the other side. The results list
marks each item `=` identical, `->` left only, `<-` right only, `>` left newer, `<` right newer, `!=` different, `!`
file vs. folder, `?` unreadable, with both sides' size and time.

### Synchronize folders (SRC-007)

`Ctrl+Shift+F2` (also Commands → Synchronize Folders, or `F5` in the compare results list) makes the two panels'
folders match. A form chooses **Direction** (Left → Right, the default / Right → Left / Both ways), **Compare by**
(name + size + timestamp / name + size + content), **Include subfolders** (on), **Delete files only on the target**
(off; one-way only, to the Recycle Bin) and the timestamp tolerance / 1-hour options from the compare.

| Compare status | Left → Right | Both ways |
|---|---|---|
| only on the left | copy → | copy → |
| only on the right | delete on the right if *Delete files only on the target*, else nothing | copy ← |
| left newer | copy →, overwrite | copy →, overwrite |
| right newer | **conflict** (the target is newer): nothing until you choose | copy ←, overwrite |
| different, neither newer | copy →, overwrite | **conflict** |
| file vs. folder | **conflict** | **conflict** |

Right → Left is the mirror image. A folder on one side only is copied (or deleted) whole; a folder on both sides
never has an action of its own.

Nothing changes until you have seen the **preview**, a list of every action (`->` / `<-` copy, `=>` / `<=` copy and
overwrite, `DEL>` / `<DEL` delete, `??` unresolved conflict), and confirmed a **summary dialog**. The dialog lists the
first overwrites and deletions by name and has *Back to preview* as its default button. It is always shown: no
configuration setting skips it. All copies run before any deletion. Every item is re-checked just before it's touched,
and one that changed since the preview is skipped and reported, never silently overwritten or deleted. Copies go
through the normal copy engine (retry / skip / elevate, `Esc`, timestamps preserved), so a compare afterwards shows
`=`. A compare that hit the 200 000-item cap is refused rather than synchronized partially. Two-way sync has no
history, so a file deleted on one side comes back from the other.

### Compare contents (SRC-008)

`Alt+F3` (also File → Compare contents, or `Alt+F3` on an item in the compare results list) shows the differences
between two text files. Which two: the two files selected in the active panel; or one file selected in each panel;
otherwise the active panel's file (selected or under the cursor) and the file of the same name in the other panel,
or else the other panel's cursor file. The left panel's file is always shown on the left.

The viewer aligns the files **side by side** with line numbers (or **unified**, one column — `U`, automatic below
100 columns). Removed, added and changed lines are coloured and marked `<`, `>`, `~` (so the meaning survives
without colour), and inside a changed line the differing characters are highlighted. `F7` / `Shift+F7` (or `n` /
`p`) jump between differences, `I` ignores case, `W` cycles whitespace handling (exact → trailing ignored → all
ignored), `F3` / `Shift+F3` open the left / right file in the normal viewer at that line, `Esc` closes. Each file's
encoding is detected separately (UTF-8, ANSI, UTF-16 with BOM; CRLF and LF are equal). Identical files just say
so. Binary files, and files over 64 MB or 200,000 lines, get a byte comparison instead: identical, or the first
differing offset.

The diff is pure, unit-tested code (`TextDiff.cpp`: linear-space Myers with common prefix/suffix trimming, a
20,000-edit cap and Esc), drawn by `DiffViewer.cpp`. Setting `diffProgram` in the config file (e.g.
`"C:\Program Files\WinMerge\WinMergeU.exe" "%1" "%2"`) makes `Alt+F3` start that program instead;
`Shift+Alt+F3` still opens the built-in viewer. Merging (copying a block to the other side) is not included
(SRC-008a, later).

### Key bindings (KEY-005)

Settings → Edit key bindings opens `%APPDATA%\MyCommander\keys.conf` (created on first use with every command and
its default key, commented out). Each line is `command = key[, key...]`; `none` unbinds; unlisted commands keep
their defaults; changes apply on save. About 50 main-screen commands are rebindable (`copy`, `move`, `find-files`,
`compare-folders`, `sort-name`, `select-all`, `history-back`, `insert-path`, …), plus plugin shortcuts as
`plugin:<plugin-id>/<commandId>`. Rejected with a warning (the rest of the file still applies): unknown names or
keys, the fixed cursor/editing keys (arrows, PgUp/PgDn, Home/End, Enter, Esc, Backspace, Tab), keys that type a
character (also `Ctrl+Alt`+letter, AltGr), `Alt+F/S/C/N/V/E` (menus), and a key already claimed by an earlier line.
A key given to one command is taken from the command that had it by default; `quit` always keeps a key (`F10` is
restored). The F1 list, the hint bar and the menus show the current keys; the plugin conflict check follows them.

How it works: `KeyMap.{h,cpp}` translates a pressed key into its command's *default* key before `main.cpp`'s
key-dispatch switch (unchanged) sees it; a default key the user moved away is dropped on the main screen but
still reaches tree/plugin panels. Menu items, hint-bar clicks and a plugin's `SimulateKey` inject default keys,
which bypass the translation. Dialogs, the viewer and the result lists keep their own fixed keys.

## Configuration

MyCommander reads a small per-user config file at `%APPDATA%\MyCommander\mycommander.ini`, creating it with
fully-commented defaults the first time it runs if one doesn't already exist. It's plain `key = value` text —
`#` or `;` starts a comment — so it can be opened and edited in any text editor. Press `Ctrl+F11` to open it
directly in the external editor (same as `F4`'s) — not plain `F11`, which most terminal hosts (Windows Terminal
included) reserve as a fullscreen toggle and never pass through to the app; MyCommander keeps running and
watches the file, so once you
save it, every setting it drives (theme, columns, hidden/system-symbol style, directory grouping, log level,
and so on) applies immediately — no restart needed (IS-0006). If a setting's value doesn't parse, the same
warning dialog startup shows for a bad value appears then too. Separately, on every startup, any setting this
version of MyCommander knows about but an older file predates gets appended to the file automatically, with
its own default value and documentation comment, so the file stays complete rather than only defaulted in
memory.

```ini
# Ask Yes/No before sending the selected item(s) to the Recycle Bin (F8).
# Values: true / false (default: true)
confirmRecycleBinDelete = true

# Ask Yes/No before permanently deleting the selected item(s), bypassing
# the Recycle Bin (Shift+F8). Strongly recommended to leave enabled.
# Values: true / false (default: true)
confirmPermanentDelete = true

# What pressing Enter on a file does. F3/F4 always mean View/Edit
# regardless of this setting.
# Values: view / edit / open / execute (default: view)
enterFileAction = view

# Keep directories before files for every sort key. Set false to sort all
# entries together by the active name/type/size/time key.
# Values: true / false (default: true)
groupDirectoriesFirst = true

# Use +, -, and | rather than Unicode box-drawing characters for panel and
# dialog frames. Leave false unless the terminal font lacks those glyphs.
# Values: true / false (default: false)
useBasicSymbols = false

# Save the internal command line's history (Ctrl+Up/Ctrl+Down) so it
# survives across runs, instead of it being session-only.
# Values: true / false (default: true)
persistCommandHistory = true

# Diagnostic log severity. Off by default -- no log file is written at
# all until you opt in. Each level also includes every level above it
# (warning also logs error, and so on).
# Values: off / error / warning / info / debug (default: off)
logLevel = off

# Which optional panel-row columns to show, and in what order (name is
# always shown first).
# Values: a comma-separated list from type, attr, mtime, size, in any
# order, or empty (default: type,attr,mtime,size)
visibleColumns = type,attr,mtime,size

# Column widths in character cells. attr is always a fixed 4 (one slot
# per read-only/hidden/system/archive flag) and isn't configurable.
# Values: an integer, 6-200 (default: 6) / 16-200 (default: 16) / 10-200 (default: 10)
typeColumnWidth = 6
mtimeColumnWidth = 16
sizeColumnWidth = 10

# Color theme: default, or highcontrast for maximum contrast wherever the
# default theme only just passed WCAG AA.
# Values: default / highcontrast (default: default)
colorTheme = default

# What F4 (edit) does while the external editor is open. false (default)
# launches it and returns immediately -- the file's panel entry refreshes
# and stays selected automatically once you save. Set true to restore the
# original behavior (wait for the editor to close before refreshing).
# Values: true / false (default: false)
waitForEditorToClose = false
```

`confirmRecycleBinDelete`/`confirmPermanentDelete` are FOP-008's "configurable safety rules" for destructive
operations: both default to `true` (confirm). `enterFileAction` is VEE-002's configurable default action for
plain `Enter` on a file (with an empty command line) — `view` runs its default `view` action, which is the
built-in viewer unless [File actions](#file-actions) registers another (the default, matching the app's original
hardcoded behavior), `edit` opens it in the external editor, `open` runs its default `open` action (or its
Windows association), and `execute` runs or
opens it through its own Windows file association, the same as double-clicking it in Explorer (VEE-008: this
only ever happens because of that explicit `Enter` press, after you've explicitly opted into it here — never
automatically). A missing file, a missing key, or an unrecognized key are all treated as "use the default"
rather than an error (CFG-003/CFG-004). A recognized key with a value it doesn't understand (`true`/`false`,
`yes`/`no`, `1`/`0` for the booleans; `view`/`edit`/`open`/`execute` for `enterFileAction`) is reported by name and
value in a dialog at startup, and that one setting falls back to its default rather than the app refusing to
start (CFG-005). `groupDirectoriesFirst` is LIST-004's persistent sort-group preference: it defaults to `true`;
set it to `false` to sort directories and files together by the selected sort key. `persistCommandHistory`
(CLI-004) defaults to `true`; set it to `false` to keep command-line history session-only instead of saving it
to `%APPDATA%\MyCommander\history.txt`. `logLevel` (ERR-004) controls the diagnostic log described in
[Diagnostic logging](#diagnostic-logging) below; it defaults to `off`. `visibleColumns`/`typeColumnWidth`/
`mtimeColumnWidth`/`sizeColumnWidth` (UI-005) control which optional panel-row columns show and how wide each
is — see [Running](#running) above for the narrow-terminal column-drop behavior this also governs. `colorTheme`
(UI-010) selects `default` or `highcontrast`. `waitForEditorToClose` (IS-0002) defaults to `false` (F4 returns
immediately; see [View/Edit](#status) above); set `true` to restore the original wait-for-close behavior. There's
no in-app settings UI yet — editing the file is the only way to change these for now.

### File actions

Which program `F3` (view), `Shift+Enter` (open) and the `Shift+F3` chooser run for a file is configured per
extension in a second file, `%APPDATA%\MyCommander\file-actions.conf` (FEA-0009, VEE-009..013) — **not**
`mycommander.ini` (`Ctrl+F11`), which warns about file-action lines put there by mistake. It's created on first
run containing only comments and commented-out examples. Changes apply on the next keypress after you save — no
restart. Each block lists extensions (`.txt`) or wildcard patterns (`*.tar.gz`, `readme*`, `[*]` for everything);
each line under it is one action, `type[*] = description | program | arguments`, where the optional `*` picks
the default of that type within its block:

```ini
[.txt .log .md]
view* = Built-in viewer        | internal:viewer |
view  = Notepad++ (read-only)  | C:\Program Files\Notepad++\notepad++.exe | -ro "%f"
open* = Notepad                | notepad.exe     | "%f"

[.pdf]
open* = Windows default        | system:         |
```

- `type` is `view` or `open`. Blocks are checked top to bottom: the first matching block that has an action of
  the needed type decides (its `*` line, else its first line of that type), so put catch-alls last.
- A 1.11.0 first-run file's `[.txt .log .md .ini .conf]` / `view* = Built-in viewer | internal:viewer |` block
  (which only restated the fallback but shadowed later `.txt`/`.md` blocks) is removed automatically; nothing
  else in the file is touched.
- `program` is an executable path or bare name (found via `PATH`), `internal:viewer` (the built-in viewer),
  `system:` (the Windows file association), or — for `view` actions only — `plugin:<plugin-id>`, a viewer plugin
  (EXT-003).
- **Viewer plugins** (API 1.10, `kMcPluginCapViewer`): a plugin renders a file as text lines that the built-in viewer
  shows (search, hex mode, paging included; output capped at 2,000,000 lines / 256 MB). F3 order: this file's `view`
  default → a plugin claiming `kMcViewDefault` for the name → the built-in viewer. `Shift+F3` lists every claiming
  plugin ("(plugin)"), and Assign File to App can make one the default. A plugin's own `ShowFileViewer` (ZIP/FTP F3)
  also goes through default-claiming viewer plugins, never external programs. `Source/ExamplePlugin/` contains a
  hex-dump viewer (Offer only).
- `arguments` placeholders: `%f` full path, `%n` file name, `%b` name without extension, `%e` extension, `%d`
  directory, `%%` a literal `%`. If none of `%f`/`%n`/`%b` appears, `"%f"` is appended automatically. Quote
  placeholders yourself where a path may contain spaces.
- With nothing registered for a file, `view` falls back to the built-in viewer and `open` to the Windows
  association. A line that can't be read is skipped and reported, never fatal.
- **File → Assign File to App** manages the cursor file's extension from inside MyCommander: set a default,
  remove, or add an action (Type / Description / Program / Arguments / Default), or open the file in the
  external editor. It edits only the affected lines, so your own comments are kept, and it never edits a
  block shared with other extensions: a new action goes into a block for that extension alone, inserted
  above any shared block that lists it so it decides without changing the others ("Default" on a shared
  entry copies it into that extension's own block).
- Programs are started detached, never elevated, and only ever by one of the keys above.

Panel and dialog frames use CP437-compatible Unicode box-drawing characters by default (`─│┌┐└┘├┤┬┴┼`). If a terminal
font cannot render them, set `useBasicSymbols = true` to use the portable ASCII `+`, `-`, and `|` form instead
(UI-014/ACC-006).

Every color pairing in both themes (`default` and `highcontrast` — `Source/MyCommander/Theme.h`) is audited
against the console's default 16-color palette to meet WCAG AA's 4.5:1 minimum contrast ratio (ACC-002/UI-010);
`ColorContrastTests.cpp` re-checks every field of both themes on every test run so a future color change can't
silently regress either one. `highcontrast` goes further, pushing every pairing the default theme only just
cleared AA on to the maximum achievable contrast (~21:1) — see the Status section's Color themes bullet above.

This config file holds preferences only — never panel paths, selection, or in-progress file-operation state.
Panel paths are instead kept in a separate `%APPDATA%\\MyCommander\\session.ini` file (NAV-008), written only after
a normal input-loop exit and read on the next launch when no command-line paths were provided. It stores exactly
the left and right locations; no operation is ever queued, journaled, or auto-resumed, so a crash cannot cause one
to be silently repeated (ERR-006). Command-line history (CLI-004), when `persistCommandHistory` is enabled, lives
in its own separate `%APPDATA%\\MyCommander\\history.txt` — one past command per line, written at the same
normal-exit point as the session file.

## Diagnostic logging

Off by default (ERR-004) — set `logLevel` in the config file above to turn it on. Each level also writes
everything above it in this list:

| Level | What gets logged |
|---|---|
| `off` | Nothing (default) |
| `error` | A failed item from a Copy/Move/Delete/attribute-toggle operation |
| `warning` | Also a config-file problem (a bad line or an unrecognized value, CFG-005) |
| `info` | Also app start and normal exit |
| `debug` | Most verbose (currently the same set as `info`) |

Written to `%APPDATA%\MyCommander\mycommander.log`, one line per event: `[2026-09-22 14:05:09] [ERROR] Copy
failed: sub: cannot place a directory inside itself`. The file is appended to, never rotated or truncated
automatically — delete it yourself if it grows larger than you want.

**ERR-005:** the log never contains typed command-line text, file content, or credentials — only paths, Win32
error text, and config setting names ever reach it. A command you run via the internal command line or `F9`
shell is never itself written to the log, regardless of `logLevel`.

## Privilege elevation

MyCommander always runs as your own current user (SEC-001) and never asks for administrator rights on its
own (SEC-002) — nothing happens at startup, and no feature silently needs elevation to work. The one
exception is explicit and scoped to a single item: if a Copy, Move, Delete, or attribute-toggle operation
fails specifically with **Access is denied**, the "Operation failed" retry dialog gains an extra
`[E]levate & Retry` choice alongside the usual Retry/Skip/Skip all/Cancel (no other kind of failure — a
locked file, a full disk, a name collision — offers it, since elevation can't fix those).

Choosing it triggers a normal Windows UAC consent prompt, then retries **only that one item** with
elevated privileges — not the rest of the selection, and not the running MyCommander session itself, which
stays unprivileged throughout (SEC-003). Under the hood this relaunches `MyCommander.exe` itself as a
short-lived, hidden, single-purpose elevated child process (via `ShellExecuteExW`'s `runas` verb) that
performs exactly that one copy/move/delete/attribute-change and exits immediately; declining the UAC
prompt is treated the same as any other failed retry, so you're returned to Retry/Skip/Skip all/Cancel
for that item.

## Command line and shell

A single-line command line is always visible just above the status bar, prefixed with the active panel's own
path (e.g. `C:\Projects\MyCommander> `). Typing inserts at the cursor; it doesn't interfere with the usual
navigation keys (Page Up/Down, the F-keys, Insert) at all.

- **Editing:** a real positionable cursor (shown as a highlighted character), not just append-at-the-end.
  `Left`/`Right` move it one character; `Home`/`End` jump to the start/end; `Backspace` erases before the
  cursor; `Delete` forward-deletes at the cursor — all only while the command line has text, so these same
  keys fall through to their usual panel-navigation/file-deletion meaning once it's empty (IS-0015 Part 3).
- **Navigating to a path:** type or paste an absolute path (`C:\Projects`), a UNC path (`\\server\share`), or a
  path relative to the active panel's directory (`..\sibling`, `subdir`), then press `Enter`. If it names an
  existing directory, the active panel jumps straight there instead of the text being run as a command (NAV-003).
  Surrounding whitespace and a matching pair of quotes are trimmed first, so pasting a path copied from
  Explorer's address bar or a shortcut's Properties dialog works as-is.
- **Running a command:** type it and press `Enter`. It runs via `%COMSPEC%` (falling back to `cmd.exe`) as
  `cmd /c "<what you typed>"`, with the active panel's directory as its working directory (CLI-002) — so shell
  metacharacters (`|`, `>`, `&&`, `%VARS%`, etc.) are interpreted by `cmd.exe` itself, exactly as they would be
  at a normal command prompt (CLI-006). Output is shown on the real console screen and paused with a "press any
  key to continue" prompt before returning to the panels, since it would otherwise be overwritten immediately.
  One well-known `cmd /c` quirk to be aware of: if your typed command itself contains `"` characters, `cmd.exe`
  can strip quoting in surprising ways — this is a long-standing Windows shell behavior, not something
  MyCommander adds on top.
- **Inserting a name or path:** `Ctrl+Enter` appends the cursor entry's name (or every selected entry's name) to
  the command line; `Ctrl+Shift+Enter` does the same with the full path instead (CLI-003). Neither runs
  anything — they only edit the command line.
- **History:** every run command can be recalled with `Ctrl+Up`/`Ctrl+Down` (CLI-004) and, by default, is saved
  to `%APPDATA%\MyCommander\history.txt` when you quit and reloaded the next time you start MyCommander, so it
  survives across runs, not just for the session. Set `persistCommandHistory = false` in the config file to
  keep history session-only instead. A command typed with a **leading space** is never added to history at
  all, persisted or not — the conventional shell opt-out for a command you don't want remembered (CLI-007).
- **A separate shell:** `F9` opens an interactive `cmd.exe` (or `%COMSPEC%`) session in the active panel's
  directory instead, for anything that needs a real, ongoing shell rather than one command (CLI-005). Exit it
  normally (e.g. type `exit`) to return to MyCommander; both panels refresh afterward.

## Menu bar

An always-visible row at the very top of the screen — `File Selection Commands Network View Settings` — for
anyone who'd rather browse for a command than remember its key. Press `Alt+F`/`Alt+S`/`Alt+C`/`Alt+N`/`Alt+V`/
`Alt+E` to open the matching menu (mnemonics chosen per-letter; `Settings` uses `E`, matching its own "Edit
settings" item, since `S` is already `Selection`'s), or click it. While a menu is open: `Left`/`Right` switch
to the adjacent menu, `Up`/`Down`
move within it, `Enter` (or the highlighted item's own bracketed mnemonic letter) chooses it, `Esc` closes it
without choosing anything, and clicking anywhere outside the open dropdown dismisses it the same way.

Most items reachable from the menu already have their own key — the menu is purely a second, discoverable path
to the exact same action (`Modify Attribs` → `Ctrl+A`, `Find` → `Alt+F7`, `Order by Name`/`Size` →
`Ctrl+F3`/`Ctrl+F5`, `Tree` → `Ctrl+T`, `Single panel` → `Ctrl+F1`, `Command Line` → `F9`, `Edit settings` → `Ctrl+F11`, `About` → `F1`, `Exit` → `F10`), never
the *only* way to reach anything (KEY-001). `Color mode` (under `Settings`) is the one item with logic that's
new here: it toggles `colorTheme` between `default`/`highcontrast`, saves it to the config file, and applies it
immediately.

Several items name features MyCommander doesn't have yet — FTP, disk-usage scanning, and the
like (archive packing/unpacking used to be in this list too, but are now provided by the ZIP plugin's own
File menu items, not built-in stubs). Selecting one of those shows "Not yet implemented" rather than
pretending to work; the menu structure is there so nothing needs to be added later, but no functionality is
claimed before it actually exists.

## Group rename (FOP-017)

`Ctrl+M` (or `File` → `Group Rename`) renames the selection (or the cursor entry) in one go. An options form takes a
**name mask** and an **extension mask** (`[N]` name, `[N2-5]`/`[N3]`/`[N3-]` parts of it, `[E]` extension with the
same ranges, `[C]` counter, `[YMD]`/`[hms]` last-write date/time, `[P]` folder name, `[[`/`]]` literal brackets),
a plain-text case-insensitive **search/replace**, a **case** conversion (lower / UPPER / First letter / Each Word),
and the counter's start, step and digits. The defaults `[N]` / `[E]` change nothing. A **preview** then lists every
`old → new`; problems are marked `!` and block the rename: an invalid name (forbidden character, trailing dot/space,
a reserved Windows name like `CON`/`NUL`, over 255 characters), two items getting the same name, or a clash with an
entry that isn't part of the batch (hidden and filtered ones included). `Enter` renames, `F2` returns to the
options, `Esc` cancels with nothing touched. The options are remembered for the session.

Swaps and chains (`a ↔ b`, `a → b → c`) are ordered, or routed through a temporary `~mcren.<n>.tmp` name, so no
step ever hits a name still in use, and an item left at a temporary name (skipped, failed, or the run cancelled)
is put back under its original name. The run shows the usual animated progress with `Esc`, and a failing item gets
Retry/Skip/Skip all/Cancel; nothing is ever overwritten. A case-only change (`report.txt` → `Report.TXT`) works —
also for plain `F2`, which used to refuse it as "already exists". Not available on plugin panels, and refused for
files a background job is using.

## Background jobs (FOP-014)

`Alt+F5` (copy) and `Alt+F6` (move) queue the selection as a background job instead of blocking the screen. The
destination prompt is the same as F5/F6. The copy keeps running while you navigate. Jobs on the same drive run one
after another; jobs on different drives run in parallel. A name clash or failure in a background job asks you
the same question as F5/F6 does, and the job waits for your answer. `Alt+Q` (or `Commands` → `Job queue`) lists
the jobs with their state and progress; `Enter` on a job asks whether to cancel it. The window title shows how many
jobs are running and queued. Quitting with jobs pending asks whether to wait for them or cancel them and quit.
Jobs are kept in memory only: nothing is resumed after a restart.

`Alt+F8` / `Alt+Shift+F8` (or `Commands` → `Queue delete` for the Recycle Bin variant) queue a Recycle Bin /
permanent delete, with exactly the same confirmation rules as `F8` / `Shift+F8` (`confirmRecycleBinDelete` /
`confirmPermanentDelete`); a failing item gets Retry/Skip/Skip all/Cancel on its own.

Overlap safety: besides "one job per drive", a job that touches the same files or folders as an active job — or
as an overlapping job queued before it — waits, whatever drive it's on, so a delete queued after a copy of the same
folder never runs before or during that copy; unrelated jobs behind it still start. Synchronous `F5`/`F6`/`Shift+F5`
(and drag-and-drop), `F8`/`Shift+F8`, `F2`, `Ctrl+A`, `F7`/`Shift+F7` refuse paths an active job is using, with a
message naming the job (wait, or cancel it with `Alt+Q`); navigation, viewing and search are never blocked. Matching
is lexical (case-insensitive, no filesystem access), and a copy's whole destination folder counts as in use.
Background jobs never offer *Elevate & Retry* (SEC-003: elevation stays an explicit, one-item, foreground action) —
skip the item and redo it with plain `F5`/`F6`/`F8`.

## Single-panel layout (UI-013, Part A)

`Ctrl+F1` (or `View` → `Single panel`) shows only the active panel, across the whole width; `Ctrl+F1` again
brings back both panels. The hidden panel keeps its folder, cursor, selection and any content (ZIP, FTP/SFTP,
tree), and the bottom border names it (` Other panel: D:\Backup `), so `F5`/`F6` (copy/move "to the other
panel"), `Shift+F2` (compare) and `Ctrl+Shift+F2` (synchronize) keep working — their prompts show the
destination. `Tab` switches which panel is shown, and `Alt+F1`/`Alt+F2` pick a drive and show that panel. Dialogs
keep the single-panel background, the mouse only reaches the visible panel, and plugin and tree panels simply get
the full width. The layout is remembered across restarts in the session file (`singlePanel = yes|no`, written on
a normal exit with the panel folders, NAV-008), and the key can be rebound in `keys.conf` (`single-panel`).
Implemented with one shared, unit-tested geometry (`ComputePanelLayout` in `Navigation.cpp`) used by drawing and
every mouse hit test. `View` → `Full`/`List` stay reserved for detailed/brief listings.

## Directory tree (UI-013, Part B)

`Ctrl+T` (or `View` → `Tree`) turns the active panel into a directory tree of its drive's (or UNC share's)
folders instead of a file listing. The tree starts already expanded down to the panel's current folder, with
that folder selected. Its subfolders are loaded only when you expand them, so large drives don't stall the
screen. Reparse points (junctions, symbolic links) are listed and marked `<REPARSE>`, but the tree never
expands through one on its own. Folders that can't be read are marked `<BROKEN>`.

Inside the tree: `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` move the cursor, `→` expands the cursor folder,
`←` collapses it (or moves to its parent if it's already collapsed), `+` and `-` (numpad or main keyboard — recognized
by the typed character, so e.g. `Shift+3` on a Hungarian layout works too, IS-0018) expand and
collapse it without moving the cursor, `Enter` (or a double-click) goes to the
cursor folder and returns the panel to an ordinary listing there, and `Esc` or `Ctrl+T` again leaves the tree
without navigating. The other panel stays a normal listing throughout, so it's still available as a copy or
move destination. The tree itself only ever offers folders, so file operations still happen on an ordinary
listing. Its expanded/collapsed state is not kept between sessions.

## Directory hotlist (NAV-007)

`Ctrl+D` (or `Commands` → `Directory Hotlist`) lists your named favorite folders, opening with the current folder's
own entry selected when it is one. `Enter` takes the active panel there; the last row, **+ Add current folder**, saves where the active panel is now under a name you type
(pre-filled with the folder's own name — a folder already in the list isn't added twice). `Del` removes a
favorite (after a Yes/No) and `F2` renames one; the list stays open for further edits. The list is stored in
`%APPDATA%\MyCommander\favorites.conf`, plain UTF-8 `name = path` lines in list order (the first `=` splits a
line, so a path may contain `=`), saved immediately after every change and re-read every time the hotlist opens —
edit it by hand to reorder. A line that can't be read is reported and skipped, never fatal. A favorite on an
unplugged drive or offline share shows the usual inaccessible-location banner, and one that is itself a
junction/symbolic link isn't entered (NAV-009) — favorite its real target instead. While a panel shows a ZIP
archive or the tree, `Ctrl+D` goes to that content like every other key.

## Testing

`MyCommanderTests` is a second console-app project in the same solution — no third-party test framework, just a
small self-registering runner. Build the solution as above, then run:

```powershell
Build\x64\Debug\MyCommanderTests.exe
```

Exit code is `0` iff every test passed. Tests run entirely inside a fresh `%TEMP%` subdirectory and clean up
after themselves.

## License

MIT — see [`LICENSE`](LICENSE).

Copyright (C) Attila Gallai 1985 - 2026 (attila@tux-net.hu)
