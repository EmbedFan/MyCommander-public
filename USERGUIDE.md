# MyCommander User Guide

MyCommander is a fast, keyboard-driven, dual-pane file manager for Windows that runs entirely in
your terminal — no mouse required, though one is fully supported if you prefer it. If you've used
Total Commander, Far Manager, or Norton Commander before, the layout will feel immediately
familiar: two side-by-side file listings, a command line, and a row of function-key actions
along the bottom.

This guide covers everything you need to use MyCommander day to day. For the full technical
specification and design history, see the project's repository instead — this document only
covers using the application.

## Contents

- [Installing](#installing)
- [Starting MyCommander](#starting-mycommander)
- [The screen layout](#the-screen-layout)
- [Getting around](#getting-around)
- [Selecting files](#selecting-files)
- [Working with files](#working-with-files)
- [Viewing and editing files](#viewing-and-editing-files)
- [Finding files](#finding-files)
- [The command line and running programs](#the-command-line-and-running-programs)
- [Using the mouse](#using-the-mouse)
- [Sorting and columns](#sorting-and-columns)
- [Choosing a drive](#choosing-a-drive)
- [In-app help](#in-app-help)
- [Settings](#settings)
- [If something needs administrator rights](#if-something-needs-administrator-rights)
- [Diagnostic log](#diagnostic-log)
- [Keyboard shortcut reference](#keyboard-shortcut-reference)
- [Troubleshooting](#troubleshooting)

## Installing

Run `MyCommanderSetup.exe` and follow the prompts. It's a small, self-contained console program —
no separate download or extra software required. It will:

1. Check that your machine can actually run MyCommander (Windows 10 or later, enough free disk
   space) and stop with a clear message if it can't, before changing anything.
2. Propose an install folder — `%LOCALAPPDATA%\Programs\MyCommander` by default — which you can
   accept by pressing Enter, or replace by typing a different path. This is a per-user location,
   so installing never needs an administrator prompt.
3. Copy MyCommander, this guide, and the license into that folder.
4. Ask whether to create a desktop shortcut (yes by default).
5. Register MyCommander with Windows so it shows up correctly under **Settings → Apps** (or the
   classic **Programs and Features**), with a working **Uninstall** button.

If you run the installer again while MyCommander is already installed, it detects that and asks
whether to install over it (an upgrade) instead of doubling anything up.

**Uninstalling:** use Windows' own **Settings → Apps → MyCommander → Uninstall** (or **Programs
and Features**) — no need to hunt down the install folder yourself. It removes the installed
files, the desktop shortcut, and the registration, cleanly.

MyCommander itself keeps no state in the install folder — your settings, command history, and
panel locations always live under `%APPDATA%\MyCommander\` regardless of where the application
itself is installed, so those aren't touched by install or uninstall (see [Settings](#settings)
below).

## Starting MyCommander

Run `MyCommander.exe` from a terminal, or launch it directly. You can optionally tell it where
each panel should start:

```
MyCommander.exe [left-folder] [right-folder]
```

Leave both off and each panel opens wherever it was left the last time you exited normally — or
your current folder, the very first time you run it.

Run `MyCommander.exe --version` to print the installed version and exit without opening the
interface (useful for checking what you have installed). You can also see the full version and
copyright information any time from inside the app — see [In-app help](#in-app-help) below.

MyCommander needs a terminal window of at least **60 columns by 15 rows**. If your window is
smaller than that, it shows a short message asking you to resize instead of trying to squeeze
the interface in unreadably; only quitting (`F10`) works until you do.

## The screen layout

From top to bottom, the screen has:

- **Two file panels**, side by side. Each shows its own current folder, that drive's free/total
  space, and a list of files and folders. Exactly one panel is "active" at a time (shown with a
  highlighted header) — most actions apply to whichever one is active.
- **A command line**, always visible just above the bottom bar, prefixed with the active panel's
  path. You can type into it at any time without it getting in the way of navigating.
- **A status line**, which appears only when there's something to say — for example, how many
  items are currently selected and their total size.
- **A hint bar**, showing the function-key commands available right now. It changes depending on
  what you're doing (browsing a folder, typing a command, filtering, and so on), and automatically
  grows to as many rows as it needs so nothing is ever cut off, however small your terminal font
  is.

## Getting around

| Key | What it does |
|---|---|
| `Tab` | Switch which panel is active. |
| `↑` / `↓` | Move the cursor up or down one item. |
| `Page Up` / `Page Down` | Move a page at a time. |
| `Home` / `End` | Jump to the first or last item. |
| `Enter` | Open a folder, or run whatever's typed on the command line. On a file with nothing typed, it does whatever your [settings](#settings) say (view, edit, or run it). |
| `Backspace` | Go up to the parent folder — or, if you've typed something on the command line, erase the last character. |
| `Alt+←` / `Alt+→` | Go back / forward through the places you've visited in the active panel, like a web browser's back button. |
| `Ctrl+H` | Show or hide hidden and system files in the active panel. |

Typing or pasting a full path (like `C:\Projects` or `\\server\share`), or a path relative to
where you are (like `..\sibling` or `subfolder`), into the command line and pressing `Enter`
jumps the active panel straight there instead of trying to run it as a command.

## Selecting files

Selected items are marked with a leading `*` and show a running total (count and size) in the
status line.

| Key | What it does |
|---|---|
| `Insert` | Toggle selection on the current item, then move down — handy for selecting several items one at a time. |
| `Numpad +` / `Numpad -` | Select / deselect items matching a wildcard pattern you type (e.g. `*.txt`); just pressing `*` and Enter selects everything. |
| `Ctrl+Numpad +` / `Ctrl+Numpad -` | Select everything / clear the selection, no prompt. |
| `Numpad *` | Invert the selection — everything selected becomes unselected and vice versa. |

Copy, move, delete, and attribute-change actions apply to your current selection; if nothing is
selected, they apply to whichever item the cursor is on.

## Working with files

| Key | What it does |
|---|---|
| `F2` | Rename the current item. |
| `F5` | Copy the selection (or current item) to the other panel's folder. |
| `F6` | Move the selection (or current item) to the other panel's folder. |
| `F7` | Create a new folder. |
| `Shift+F7` | Create a new, empty file. |
| `F8` | Delete to the Recycle Bin. |
| `Shift+F8` | Delete permanently, bypassing the Recycle Bin. |
| `Ctrl+A` | Toggle a Windows attribute (read-only, hidden, system, or archive) on the selection. |

A newly created folder or file is automatically selected once it appears in the listing, so you
can immediately act on it (rename it, for instance).

**Copying and moving** show a live progress dialog with the current item and, for large files, a
transfer speed. Press `Esc` at any point to cancel — it takes effect promptly, even in the middle
of a large file or deep inside a folder tree.

**Name clashes** (copying something that already exists at the destination) offer Overwrite,
Skip, Rename, or Cancel, with an "apply to all remaining items" option so you don't have to answer
the same question repeatedly.

**Other failures** — a locked file, a full disk, and the like — offer Retry, Skip, Skip All, or
Cancel instead. If the failure is specifically because of a permissions problem, you'll also see
an **Elevate & Retry** option; see [If something needs administrator rights](#if-something-needs-administrator-rights).

**Deleting** asks for confirmation by default, both for the Recycle Bin and for permanent delete —
you can turn either confirmation off in [Settings](#settings) if you don't want to be asked.

## Viewing and editing files

| Key | What it does |
|---|---|
| `F3` | View the current file in the built-in viewer (fast, read-only, works on files of any size). |
| `F4` | Open the current file in your external text editor. |

Inside the viewer: `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` scroll, `/` starts a forward text
search, `n` repeats it, and `Esc`, `F3`, or `F10` close the viewer.

`F4` uses whatever editor your `EDITOR` environment variable names, or Notepad if it isn't set.
By default it launches the editor and hands control straight back to you — MyCommander stays
usable immediately, and the file's entry in the panel refreshes and stays selected on its own as
soon as you save. (If you'd rather MyCommander wait until you close the editor, there's a setting
for that — see [Settings](#settings).)

## Finding files

| Key | What it does |
|---|---|
| `Ctrl+F` | Filter the active panel's listing by name or wildcard; submit an empty filter to clear it. |
| `Alt+F7` | Search recursively for files and folders by name or wildcard, starting from the active panel's folder. |

Search results open in their own scrollable list: `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` move
through them, `Enter` jumps the active panel straight to the selected result, `F3` previews a file
result without leaving the list, and `Esc` or `F10` close it. A pattern containing `*` or `?` is
matched as a wildcard; a plain pattern with neither is matched as a substring anywhere in the name
(so `log` finds `app.log.txt`).

## The command line and running programs

The line just above the status bar is always live — you can type into it any time, and it doesn't
interfere with navigating with the arrow keys or function keys.

- **Run a program or shell command:** type it and press `Enter`. It runs in the active panel's
  folder, so relative paths, pipes (`|`), redirection (`>`), and environment variables all work
  exactly as they would at an ordinary command prompt. The output is shown on the full screen and
  waits for a key press before returning you to MyCommander.
- **Open a real interactive shell instead:** press `F9` for a full `cmd.exe` session in the active
  panel's folder — useful for anything longer than a single command. Type `exit` to return to
  MyCommander; both panels refresh afterward.
- **Insert a name into the command line:** `Ctrl+Enter` adds the current (or every selected)
  item's name; `Ctrl+Shift+Enter` adds the full path instead. Neither one runs anything by itself.
- **Recall previous commands:** `Ctrl+↑` / `Ctrl+↓` browse your command history. By default it's
  saved between sessions; put a single space at the very start of a command to keep that one out
  of history.

## Using the mouse

Mouse support is entirely optional — everything in MyCommander can always be done from the
keyboard alone — but it's there if you'd rather use it:

- **Click** an item to move the cursor to it and make that panel active; **double-click** to open
  it, same as pressing `Enter`.
- **Scroll the wheel** to move the cursor up/down, over either panel.
- **Drag and drop** a selection (or the current item) from one panel onto the other to copy it;
  hold **Shift** while dropping to move instead.
- **Right-click** an item to toggle its selection; hold the button and drag to extend the
  selection over everything the pointer passes.
- **Ctrl+left-click**, or a right-click followed by **Ctrl+right-click**, selects every item
  between where the cursor was and where you clicked.
- **Click a command** shown in the bottom hint bar to run it, the same as pressing its key.
- **Click a dialog's option** to choose it, the same as its hotkey; clicking anywhere in a plain
  message dialog dismisses it.

The drive picker, the search-results list, the file viewer, and the help screen all support
scrolling the mouse wheel too. Drag-and-drop, right-click selection, and clicking dialog options
are specific to the two main file panels.

## Sorting and columns

| Key | What it does |
|---|---|
| `Ctrl+F3` | Sort the active panel by name. |
| `Ctrl+F4` | Sort by file type/extension. |
| `Ctrl+F5` | Sort by size. |
| `Ctrl+F6` | Sort by modification time. |

Press the same key again to reverse the sort direction. Each panel remembers its own sort key and
direction independently, and the panel header shows which one is active (e.g. `Size↓`). Folders
always sort before files.

Each row also shows the file's type, attributes, modification date/time, and size, dropping
columns gracefully (starting with size, then time, then attributes) if your terminal window is
too narrow to fit them all — never squeezing the file name down to unreadable.

## Choosing a drive

| Key | What it does |
|---|---|
| `Alt+F1` / `Alt+F2` | Open a full-screen drive picker for the left / right panel. |

Every fixed disk, removable drive, mapped network share, optical drive, and RAM disk currently
available shows up, each with its type, label, and free/total space. `↑`/`↓`/`Page Up`/
`Page Down`/`Home`/`End` choose one, `Enter` jumps that panel straight to its root, and `Esc` or
`F10` cancel without changing anything.

## In-app help

Press `F1` at any time for a full-screen, scrollable shortcut reference covering everything in
this guide, without leaving the app. It opens with an "About" section showing the installed
version, a short description, and copyright information, read directly from the application file
itself — the same information Windows Explorer shows if you right-click `MyCommander.exe`,
choose Properties, and open the Details tab.

`↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End`, or the mouse wheel, scroll it; `Esc`, `F1`, or `F10`
close it.

## Settings

MyCommander keeps a small settings file at:

```
%APPDATA%\MyCommander\mycommander.ini
```

It's created automatically, with every setting explained in a comment, the first time you run the
app. It's plain text, so you can open it in any editor — or press **`Ctrl+F11`** inside
MyCommander to open it directly in your external editor. Save your changes and MyCommander applies
them immediately, with no restart needed.

The settings available:

| Setting | What it controls | Default |
|---|---|---|
| `confirmRecycleBinDelete` | Ask before sending items to the Recycle Bin (`F8`) | `true` |
| `confirmPermanentDelete` | Ask before permanently deleting items (`Shift+F8`) | `true` |
| `enterFileAction` | What plain `Enter` does on a file: `view`, `edit`, or `execute` | `view` |
| `groupDirectoriesFirst` | Keep folders listed before files for every sort order | `true` |
| `useBasicSymbols` | Use plain `+`/`-`/`\|` instead of Unicode box-drawing characters for frames, for terminal fonts that don't have them | `false` |
| `persistCommandHistory` | Save command-line history between runs | `true` |
| `logLevel` | Diagnostic log detail: `off`, `error`, `warning`, `info`, or `debug` | `off` |
| `visibleColumns` | Which optional columns to show, and in what order | `type,attr,mtime,size` |
| `typeColumnWidth` / `mtimeColumnWidth` / `sizeColumnWidth` | Widths (in characters) for those columns | `6` / `16` / `10` |
| `colorTheme` | `default`, or `highcontrast` for maximum readability | `default` |
| `waitForEditorToClose` | Whether `F4` waits for your editor to close before refreshing | `false` |

If a line in the file has a typo or an unrecognized value, MyCommander tells you at startup and
falls back to that one setting's default rather than refusing to start. If a future update adds a
new setting, your existing file gets it appended automatically (with its default value and
explanation) the next time you start MyCommander, so it never falls behind.

Your two panels' current folders are remembered separately, in their own file, and restored the
next time you start MyCommander normally (this doesn't apply if you gave folder paths on the
command line, or if you exited abnormally).

## If something needs administrator rights

MyCommander always runs with your own normal user account — it never asks for administrator
rights on its own, and nothing about starting it up requires them.

If a Copy, Move, Delete, or attribute change fails specifically because of **Access is denied**,
the retry dialog offers an extra **Elevate & Retry** option alongside the usual choices. Choosing
it shows the normal Windows administrator consent prompt, then retries **only that one item**
with elevated rights — not the rest of your selection, and not the rest of your MyCommander
session, which keeps running normally throughout. Declining the prompt is treated the same as any
other failed retry.

## Diagnostic log

Off by default. If you turn it on (via `logLevel` in [Settings](#settings)), MyCommander writes
one line per event to:

```
%APPDATA%\MyCommander\mycommander.log
```

covering things like failed file operations, configuration problems, and app start/exit — never
anything you've typed on the command line, file contents, or credentials. The file is only ever
appended to; delete it yourself if it grows larger than you'd like.

## Keyboard shortcut reference

### Navigation

| Key | Action |
|---|---|
| `Tab` | Switch active panel |
| `↑` / `↓` | Move cursor |
| `Page Up` / `Page Down` | Move a page at a time |
| `Home` / `End` | Jump to first / last entry |
| `Alt+←` / `Alt+→` | Go back / forward through location history |
| `Enter` | Open a folder, run the command line, or run the default file action |
| `Backspace` | Parent folder, or edit the command line if it has text |
| `Alt+F1` / `Alt+F2` | Choose a drive for the left / right panel |

### Selection

| Key | Action |
|---|---|
| `Insert` | Toggle selection on the cursor entry and move down |
| `Numpad +` / `Numpad -` | Select / deselect entries matching a wildcard |
| `Ctrl+Numpad +` / `Ctrl+Numpad -` | Select all / clear selection |
| `Numpad *` | Invert selection |

### File operations

| Key | Action |
|---|---|
| `F2` | Rename |
| `F5` | Copy |
| `F6` | Move |
| `F7` | New folder |
| `Shift+F7` | New empty file |
| `F8` | Delete (Recycle Bin) |
| `Shift+F8` | Delete permanently |
| `Ctrl+A` | Toggle a file attribute |

### Viewing & editing

| Key | Action |
|---|---|
| `F3` | View the current file |
| `F4` | Edit the current file externally |
| `F9` | Open a shell in the active folder |

### Sorting & filtering

| Key | Action |
|---|---|
| `Ctrl+F3` | Sort by name |
| `Ctrl+F4` | Sort by type |
| `Ctrl+F5` | Sort by size |
| `Ctrl+F6` | Sort by modification time |
| `Ctrl+F` | Filter by name/wildcard |
| `Ctrl+H` | Toggle hidden/system entries |
| `Alt+F7` | Find files recursively |

### Command line

| Key | Action |
|---|---|
| `Ctrl+Enter` | Insert name(s) into the command line |
| `Ctrl+Shift+Enter` | Insert full path(s) into the command line |
| `Ctrl+↑` / `Ctrl+↓` | Browse command history |
| `Esc` | Clear the command line |

### Other

| Key | Action |
|---|---|
| `F1` | Show the in-app shortcut reference (with version/about info) |
| `F10` | Quit |
| `Ctrl+F11` | Open the settings file in your external editor |

## Troubleshooting

**The window says "Terminal window too small."** MyCommander needs at least 60 columns by 15
rows. Resize your terminal window (or reduce its font size) until the message clears.

**A shortcut key doesn't seem to do anything.** A few terminal hosts intercept certain keys before
they ever reach an app — most notably, plain `F11` is commonly reserved by the terminal itself for
fullscreen, which is why MyCommander uses `Ctrl+F11` for opening settings instead. If a key isn't
working, try holding `Ctrl` with it, or check whether your terminal has it bound to something else.

**Ctrl+left-click / Ctrl+right-click range selection doesn't work.** This can happen under the
classic Windows console host (rather than Windows Terminal); a modifier-held mouse click can be
swallowed before it reaches any app. Using Windows Terminal avoids this.

**A setting I changed doesn't seem to apply.** Make sure you saved the file after editing it —
MyCommander watches for the save and applies changes immediately, but only once the file is
actually written to disk. If a value has a typo, MyCommander reports it in a dialog at the next
startup and uses that setting's default instead.

**I want to check which version I have installed.** Run `MyCommander.exe --version` from a
terminal, or press `F1` inside the app — the very top of the shortcut reference shows the
installed version, alongside a short description and copyright information.

---

MyCommander is MIT-licensed, open-source software. Copyright (C) Attila Gallai 1985 - 2026.
