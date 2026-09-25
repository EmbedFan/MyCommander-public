# MyCommander

A keyboard-driven, dual-pane terminal file manager for Windows, inspired by Total Commander. Native C++, built
directly on the Win32 Console API — no third-party TUI library.

Full requirements and design decisions live in [`AIPrompt/my-commander.md`](AIPrompt/my-commander.md). For a
plain, end-user-focused walkthrough with no build/development content — the document meant to be installed
and read alongside the application itself — see [`USERGUIDE.md`](USERGUIDE.md).

## Version

**0.1.56** — bump `Source/MyCommander/Version.h` to change it; the `.rc` file's embedded Windows
file version (visible in Explorer's file Properties > Details tab) and `MyCommander.exe
--version` both derive from that single header, so there is nothing else to update.

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
  permanent delete, and attribute toggling all show live per-item progress and honor `Esc` promptly —
  including mid-file during a large copy, and mid-tree during a large recursive permanent delete — so
  none of them ever run to completion with a frozen screen and no way to interrupt (PER-002). Progress is shown
  as a centered dialog (source/destination, current item, and — for copy/move, once a file is large enough for
  Windows to report incremental progress on it — the transferred-bytes/total and a live transfer speed, FOP-009),
  matching the look of the app's other modal dialogs rather than a one-line status-bar overlay. Progress
  reporting itself is rate-capped (~20 Hz) so it cannot measurably slow a transfer down, no matter how
  often the OS's own copy-progress callback actually fires (PER-005).
- **View/Edit:** a built-in scrollable text viewer with forward search, and external-editor launch (`%EDITOR%`,
  falling back to `notepad.exe`). By default (`waitForEditorToClose = false`) `F4` launches the editor and
  returns immediately — MyCommander stays usable right away, and the edited file's panel entry refreshes and
  stays selected automatically once you save, via a Windows file-change notification. Set `waitForEditorToClose
  = true` to restore the original wait-for-close behavior — needed for a console-based `%EDITOR%` sharing this
  same window, which the non-blocking mode doesn't hand off to cleanly.
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
build regardless of which configuration `MyCommanderSetup` itself is built in (INST-006). See
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
The drive/search pickers, the viewer, and the help screen remain keyboard-only.

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
| `Home` / `End` | Move to the first / last entry. |
| `Alt+Left` / `Alt+Right` | Go back / forward through the active panel's location history. |
| `Enter` | Run non-empty command-line text; otherwise enter a directory, or use the configured default file action (view, edit, or execute). |
| `Backspace` | Remove one command-line character when it has text; otherwise go to the parent directory. |
| `Alt+F1` / `Alt+F2` | Open the drive picker for the left / right panel. |
| `Ctrl+H` | Show or hide hidden and system entries in the active panel. |
| `F1` | Open the in-application shortcut reference. |
| `Esc` | Clear non-empty command-line text. |
| `F10` | Quit. |
| `Ctrl+F11` | Open the config file in the external editor; changes apply live once you save (IS-0006). |

### Selection, sorting, filtering, and search

| Input | Action |
|---|---|
| `Insert` | Toggle selection on the cursor entry, then move down. |
| `Numpad +` / `Numpad -` | Select / deselect entries matching a prompted wildcard mask; the default `*` matches everything. |
| `Ctrl+Numpad +` / `Ctrl+Numpad -` | Select all / clear all selections without prompting. |
| `Numpad *` | Invert the selection. |
| `Ctrl+F3` | Sort the active panel by name; press again to reverse the direction. |
| `Ctrl+F4` | Sort by extension/type; press again to reverse the direction. |
| `Ctrl+F5` | Sort by size; press again to reverse the direction. |
| `Ctrl+F6` | Sort by modification time; press again to reverse the direction. |
| `Ctrl+F` | Filter the active panel by name or wildcard; submit an empty filter to clear it. |
| `Alt+F7` | Recursively find files and directories by name or wildcard below the active panel. |

### File actions

| Input | Action |
|---|---|
| `F2` | Rename the cursor entry. |
| `F3` | View the cursor file in the built-in text viewer. |
| `F4` | Open the cursor file in the configured external editor (`%EDITOR%`, otherwise `notepad.exe`). |
| `F5` | Copy the items to the other panel's current directory. |
| `F6` | Move the items to the other panel's current directory. |
| `F7` | Create a directory in the active panel. |
| `Shift+F7` | Create an empty file in the active panel. |
| `F8` | Delete the items to the Recycle Bin. |
| `Shift+F8` | Permanently delete the items, bypassing the Recycle Bin. |
| `Ctrl+A` | Toggle one chosen Windows attribute (read-only, hidden, system, or archive) on the items. |
| `F9` | Open a separate interactive shell in the active panel's directory. |

Copy, move, delete, and attribute changes show progress and can be cancelled with `Esc`. Name collisions offer
overwrite, skip, rename, or cancel choices (including apply-to-all); other per-item failures offer retry, skip,
skip-all, or cancel. Access-denied failures also offer an item-scoped **Elevate & Retry** option. Recycle-Bin and
permanent-delete confirmations are configurable.

### Command line

| Input | Action |
|---|---|
| Printable text or paste | Add text to the always-visible internal command line. |
| `Enter` with command-line text | If the text names an existing absolute, relative, or UNC directory, navigate the active panel there; otherwise run it through the configured shell in the active panel directory. |
| `Ctrl+Enter` | Insert the active item name, or selected item names, into the command line. |
| `Ctrl+Shift+Enter` | Insert the active item full path, or selected item full paths, into the command line. |
| `Ctrl+Up` / `Ctrl+Down` | Browse command history. |
| Leading space before `Enter` | Run or navigate using that text without recording it in command history. |

Command history is persisted by default and can be disabled with `persistCommandHistory` in the configuration.

### Commands inside temporary screens

| Screen | Available input |
|---|---|
| Drive picker (`Alt+F1` / `Alt+F2`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` choose a drive; `Enter` opens it; `Esc` or `F10` cancels. |
| Find Files results (`Alt+F7`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` choose a result; `Enter` opens it in the active panel; `F3` views a file result; `Esc` or `F10` closes the picker. |
| Built-in viewer (`F3`) | `↑` / `↓`, `Page Up` / `Page Down`, `Home` / `End` scroll; `/` starts a forward search; `n` repeats it; `Esc`, `F3`, or `F10` closes the viewer. |
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

Inside the Find Files results picker (`Alt+F7`): `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` move the cursor,
`Enter` jumps the active panel straight to the selected entry, `F3` opens a file result in the viewer without
leaving the picker, and `Esc`/`F10` close it. A filter or search pattern with no `*`/`?` is treated as a
substring match (e.g. `log` matches `app.log.txt`); with `*`/`?` it's matched as a classic wildcard. A directory
result is shown with a trailing `\`, not just a different color, so it's identifiable even without color
(ACC-001).

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
# Values: view / edit / execute (default: view)
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
plain `Enter` on a file (with an empty command line) — `view` opens it in the built-in viewer (the default,
matching the app's original hardcoded behavior), `edit` opens it in the external editor, and `execute` runs or
opens it through its own Windows file association, the same as double-clicking it in Explorer (VEE-008: this
only ever happens because of that explicit `Enter` press, after you've explicitly opted into it here — never
automatically). A missing file, a missing key, or an unrecognized key are all treated as "use the default"
rather than an error (CFG-003/CFG-004). A recognized key with a value it doesn't understand (`true`/`false`,
`yes`/`no`, `1`/`0` for the booleans; `view`/`edit`/`execute` for `enterFileAction`) is reported by name and
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
path (e.g. `C:\Projects\MyCommander> `). Typing appends to it; it doesn't interfere with the usual navigation
keys (arrows, Page Up/Down, the F-keys, Insert) at all.

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
