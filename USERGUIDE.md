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
  - [Renaming many files at once](#renaming-many-files-at-once)
- [Viewing and editing files](#viewing-and-editing-files)
- [Choosing which program opens a file](#choosing-which-program-opens-a-file)
- [Finding files](#finding-files)
- [The command line and running programs](#the-command-line-and-running-programs)
- [The menu bar](#the-menu-bar)
- [Connecting to an FTP server](#connecting-to-an-ftp-server)
- [Connecting to an SFTP (SSH) server](#connecting-to-an-sftp-ssh-server)
- [Comparing two text files](#comparing-two-text-files)
- [Single-panel layout](#single-panel-layout)
- [Directory tree](#directory-tree)
- [Using the mouse](#using-the-mouse)
- [Sorting and columns](#sorting-and-columns)
- [Choosing a drive](#choosing-a-drive)
- [Favorite folders (directory hotlist)](#favorite-folders-directory-hotlist)
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
| `Home` / `End` | Jump to the first or last item — or, if you've typed something on the command line, jump its cursor to the start/end instead. |
| `Enter` | Open a folder, or run whatever's typed on the command line. On a file with nothing typed, it does whatever your [settings](#settings) say (view, edit, open, or run it). |
| `Shift+Enter` | Open the current file with its "open" program — see [Choosing which program opens a file](#choosing-which-program-opens-a-file). |
| `Backspace` | Go up to the parent folder, with the cursor on the folder you just left — or, if you've typed something on the command line, erase the character before the cursor. |
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
| `Ctrl+M` | Rename many items at once, by pattern — see [Renaming many files at once](#renaming-many-files-at-once). |
| `F5` | Copy the selection (or current item) to the other panel's folder. |
| `Shift+F5` | Make a copy of the current item under a new name, in the same folder. Type just a name for a copy next to the original, a full path to copy it somewhere specific, or an existing folder name (or a name ending in `\`) to copy it into that folder. |
| `F6` | Move the selection (or current item) to the other panel's folder. |
| `F7` | Create a new folder. |
| `Shift+F7` | Create a new, empty file. |
| `F8` / `Delete` | Delete to the Recycle Bin (`Delete` forward-deletes command-line text instead if it has any). |
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
you can turn either confirmation off in [Settings](#settings) if you don't want to be asked. Both
kinds then show the same animated progress dialog as copying, counting the items as they go; press
`Esc` to stop before the next item. If one item can't be deleted (for example, a file another
program has open), you're offered Retry, Skip, Skip All, or Cancel for that item alone, and the
rest are still deleted.

**Changing only upper/lower case** with `F2` works too — for example `report.txt` to `Report.TXT`.

### Renaming many files at once

Select the files (or folders) to rename and press `Ctrl+M` — or choose **File → Group Rename**.
A form asks how the new names should be built; a preview then shows every change before anything
is renamed.

**The form** (`↑`/`↓` move between fields, `←`/`→` change the case choice, type to edit):

| Field | What it does | Default |
|---|---|---|
| Name mask | Builds the new name (before the last dot) | `[N]` (the old name) |
| Extension mask | Builds the new extension (after the dot); leave it empty for no extension | `[E]` (the old extension) |
| Search for / Replace with | Replaces text in the new name (not case-sensitive, every occurrence) | empty (no replacement) |
| Case | unchanged, lower case, UPPER CASE, First letter capital, Each Word Capital | unchanged |
| Counter start / step / digits | Numbering for `[C]` | 1 / 1 / 1 |

With the defaults nothing changes, so opening the form never does any harm.

**What you can put in the masks** (shown for `Holiday Photo.JPG`, counter 7 with 3 digits):

| Write | You get |
|---|---|
| `[N]` | `Holiday Photo` — the name without extension |
| `[N2-5]` | `olid` — characters 2 to 5 |
| `[N3]` / `[N3-]` | `l` — just character 3 / `liday Photo` — from character 3 to the end |
| `[E]`, `[E1-2]` | `JPG`, `JP` — the extension, or part of it |
| `[C]` | `007` — the counter |
| `[YMD]` / `[hms]` | `20261007` / `153012` — the file's last-modified date / time |
| `[P]` | `Pictures` — the name of the folder it's in |
| `[[` / `]]` | a literal `[` / `]` |

Anything else you type is copied as it is.

**The preview** lists `old name → new name` for every item. Lines marked `!` are problems that
stop the rename until you fix them: an invalid name (for example one containing `?`, ending in a
dot, or a name Windows reserves such as `CON` or `NUL`), two items that would get the same name,
or a name another file in the folder already has. Press `Enter` to rename, `F2` to go back and
change the options, or `Esc` to cancel — nothing is renamed until you press `Enter`.

**Examples:**

- **Number your photos by date:** name mask `[YMD]_[C]`, counter digits `3` —
  `IMG_4512.JPG` becomes `20261007_001.JPG`, the next one `20261007_002.JPG`, and so on.
- **Add a prefix:** name mask `Holiday-[N]` — `beach.jpg` becomes `Holiday-beach.jpg`.
- **Change the extension:** extension mask `txt` — `notes.log` becomes `notes.txt`.
- **Replace text:** search for `draft`, replace with `final` — `Report draft.docx` becomes `Report final.docx`.
- **Tidy the case:** Case *lower case* — `IMG_0001.JPG` becomes `img_0001.jpg`.
- **Renumber files that already use those numbers:** select `1.txt`, `2.txt` and `3.txt`, name
  mask `[C]`, counter start `2` — they become `2.txt`, `3.txt` and `4.txt`. The new names overlap
  the old ones, and MyCommander renames them in a safe order (using a short-lived temporary name
  where two names would trade places), so nothing is ever overwritten.

While renaming you see the usual progress dialog; `Esc` stops after the current item. If an item
can't be renamed (for example, a file another program has open), you get Retry, Skip, Skip All, or
Cancel for that item. An item never ends up stuck under a temporary name — if something goes wrong
it gets its old name back. Your options are remembered until you quit MyCommander.

## Viewing and editing files

| Key | What it does |
|---|---|
| `F3` | View the current file — in the built-in viewer (fast, read-only, works on files of any size), unless you've chosen a different viewer for that kind of file. |
| `Shift+F3` | Pick from every program set up for the current file (see [below](#choosing-which-program-opens-a-file)). |
| `Shift+Enter` | Open the current file in its "open" program — by default, whatever Windows normally opens it with. |
| `F4` | Open the current file in your external text editor. |

Inside the viewer: `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` scroll, `/` starts a forward text
search, `n` repeats it, and `Esc`, `F3`, or `F10` close the viewer.

**Hex view.** Press `H` in the viewer to see the file as bytes: an offset, the bytes in hex, and a text
column on the right (non-printable bytes show as `.`). `H` again goes back to text, at the same place in the
file.

Hex view has a cursor, highlighted in both the hex digits and the text column for the byte it's on. Use
`↑`/`↓`/`←`/`→`/`Page Up`/`Page Down`/`Home`/`End` to move it; the window scrolls on its own to keep the cursor
in view, including sideways when a row is wider than your window. The offset shown at the bottom is always
where the cursor is, not just the first byte on screen. Press `W` to change how many bytes go on a row: 8, 16,
32, or **fit**, which uses the largest of those that fits your window (8 each time you open the viewer) — the
cursor stays on the same byte when you switch.

Search also works in hex view. Type text to find it, or type `#` followed by hex bytes to find those bytes:
`#4D 5A` finds the bytes `4D 5A`. A match is highlighted until you press another key.

`F4` uses whatever editor your `EDITOR` environment variable names, or Notepad if it isn't set.
By default it launches the editor and hands control straight back to you — MyCommander stays
usable immediately, and the file's entry in the panel refreshes and stays selected on its own as
soon as you save. (If you'd rather MyCommander wait until you close the editor, there's a setting
for that — see [Settings](#settings).)

**File properties:** `Alt+Enter` shows a box with the current file's name, size, and modification
time. For a runnable file (`.exe`, `.dll`, and similar), it also shows the file's embedded version
information — description, file version, product name and version, copyright — and checks whether
the file is digitally signed, showing either "Not signed" or the signer's name. (This also covers
Windows system files signed via a catalog, not just files with their own embedded signature.)
Press any key or click to close it. The same action is available from the menu bar under
**View → Executable properties**.

## Choosing which program opens a file

You can tell MyCommander which program to use for each kind of file — for example, view `.log`
files in Notepad++, open PDFs in your favorite PDF reader, or open images in an image editor. Each
kind of file can have several programs set up for it, of two types:

- **view** — what `F3` uses. With nothing set up, `F3` uses the built-in viewer, exactly as before.
- **open** — what `Shift+Enter` uses. With nothing set up, `Shift+Enter` opens the file with
  whatever Windows normally uses for it (the same as double-clicking it in Explorer).

One program of each type can be marked as the **default** — that's the one `F3` or
`Shift+Enter` runs. To use any of the others, press **`Shift+F3`**: it lists every program set up
for the current file, plus the built-in viewer and "Windows default", and you pick one with
`↑`/`↓` and `Enter` (or a click). The current defaults are marked with `*`.

Nothing ever runs on its own: a program only starts when you press one of these keys. Programs
start in the background — MyCommander stays usable straight away — and never with administrator
rights.

### The easy way: File → Assign File to App

1. Put the cursor on a file of the kind you want to set up — say, `report.pdf`.
2. Open the **File** menu (`Alt+F`) and choose **Assign File to App**. You'll see every program
   already set up for `.pdf` files, plus **+ Add a new action...** and an option to edit the
   settings file by hand.
3. Choose **+ Add a new action...** and fill in the form (`↑`/`↓` move between fields,
   `←`/`→` change the Type, type to edit text):

   | Field | Example | Meaning |
   |---|---|---|
   | Type | `open` | `view` (for `F3`) or `open` (for `Shift+Enter`) |
   | Description | `SumatraPDF` | The name shown in lists — optional |
   | Program | `C:\Program Files\SumatraPDF\SumatraPDF.exe` | The program to run — a full path, or just its name if it's on your `PATH` |
   | Arguments | `"%f"` | What to pass to it; `%f` is replaced with the file's full path |
   | Default for this type | `[x]` | Make this the one `Shift+Enter` uses |

4. Press `Enter` to save. From now on, `Shift+Enter` on any `.pdf` file opens it in SumatraPDF.

Choosing an existing entry in the same list lets you make it the **Default** or **Remove** it
(removing asks first). Adding never changes other file types: new entries go into a block for
that one extension only. An entry marked `[shared with .txt .log ...]` comes from a block that
several file types share, so making it the default or removing it affects all of them.

### The settings file

Everything above is stored in a plain text file you can also edit directly:

```
%APPDATA%\MyCommander\file-actions.conf
```

**This is not the main settings file.** `Ctrl+F11` opens `mycommander.ini`, and file actions
written there are ignored. MyCommander warns about them and points you here instead. To open
`file-actions.conf`, use **File → Assign File to App → Edit file-actions.conf in the external
editor**, or open the path above in any editor.

It's created the first time you run MyCommander, containing only explanatory comments and
commented-out examples, so nothing is set up until you add your own blocks. Save it and the change
applies the next time you press `F3` or `Shift+Enter`, with no restart. If a line can't be
understood, MyCommander tells you which line and why, skips just that line, and keeps going.

The file is made of **blocks**. Each block starts with the kinds of files it applies to, in square
brackets, followed by one line per program:

```ini
[.txt .log .md]
view* = Built-in viewer       | internal:viewer |
view  = Notepad++ (read-only) | C:\Program Files\Notepad++\notepad++.exe | -ro "%f"
open* = Notepad               | notepad.exe     | "%f"
```

Each program line has the form `type = description | program | arguments`:

- **type** is `view` or `open`. Put a `*` right after it (`view*`, `open*`) to make that line the
  default for its type in this block.
- **description** is the name shown in the `Shift+F3` list. It can be left empty.
- **program** is the program to run — a full path, or a name Windows can find on its own (like
  `notepad.exe`). Special values:
  - `internal:viewer` — MyCommander's own built-in viewer.
  - `system:` — whatever Windows normally opens the file with.
  - `plugin:<plugin id>` — a **viewer plugin** (for `view` lines only), for example
    `view* = Hex dump | plugin:com.mycommander.exampleplugin |`. Its output appears in
    MyCommander's own viewer, so search and scrolling work as usual. If that plugin isn't
    installed, you're told and the built-in viewer opens instead.

**Viewer plugins** can also offer themselves without any setup: `Shift+F3` lists every installed
viewer plugin that can show the file (marked "(plugin)"), and a plugin made for a particular
format may become what `F3` opens for it — but only while this file doesn't set a `view` default
for that file, because your own choice here always wins. **File → Assign File to App** has a
"Use a viewer plugin as the default view" entry when one can show the file.
- **arguments** are passed to the program. You can use these placeholders:

  | Placeholder | Replaced with | For `C:\Docs\report.final.pdf` |
  |---|---|---|
  | `%f` | Full path | `C:\Docs\report.final.pdf` |
  | `%n` | File name | `report.final.pdf` |
  | `%b` | File name without its extension | `report.final` |
  | `%e` | Extension, without the dot | `pdf` |
  | `%d` | The file's folder | `C:\Docs` |
  | `%%` | A literal `%` sign | `%` |

  Put quotes around a placeholder whenever the path might contain spaces: `"%f"`, not `%f`. If
  you don't use `%f`, `%n`, or `%b` at all, MyCommander adds `"%f"` at the end for you — so an
  empty arguments field simply passes the file to the program.

The `[...]` line can list:

- **extensions**, like `.txt` or `.JPG` (upper/lower case doesn't matter);
- **wildcard patterns**, like `*.tar.gz` or `readme*`, matched against the whole file name;
- **exact names**, like `Makefile` or `.gitignore`;
- **`[*]`**, which matches every file.

**Which program runs** when a file matches more than one block: blocks are checked from the top
of the file down, and the **first matching block that has a `view` line** decides what `F3` runs.
Within that block, the line marked `*` is used, or the block's first `view` line if none is
marked. The same applies to `open` lines for `Shift+Enter`. The `*` is therefore optional; it only
matters when one block has several programs of the same type. Put specific blocks first and a
catch-all `[*]` block last. `Shift+F3` always lists the programs from every matching block.

### A complete real example: Markdown files in Edge and Notepad++

Goal: on any `.md` file, **`F3` shows it in Microsoft Edge** and **`Shift+Enter` opens it in
Notepad++ for editing**.

**1. Find the two programs' real paths.** On a standard Windows 11 installation they are:

| Program | Path |
|---|---|
| Microsoft Edge | `C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe` |
| Notepad++ (64-bit) | `C:\Program Files\Notepad++\notepad++.exe` |

To check, browse to the folder in MyCommander: the `.exe` should be listed there. Notepad++ is
installed under `C:\Program Files (x86)\Notepad++\` instead if you installed the 32-bit version.

**2a. Set it up from the menu.** Put the cursor on any `.md` file (for example `README.md`), open
**File → Assign File to App**, choose **+ Add a new action...**, and fill in:

| Field | Value |
|---|---|
| Type | `view` |
| Description | `Edge` |
| Program | `C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe` |
| Arguments | `"%f"` |
| Default for this type | `[x]` |

Press `Enter`, choose **+ Add a new action...** again, and add the second one:

| Field | Value |
|---|---|
| Type | `open` |
| Description | `Notepad++` |
| Program | `C:\Program Files\Notepad++\notepad++.exe` |
| Arguments | `"%f"` |
| Default for this type | `[x]` |

**2b. …or type it into the file yourself.** The two steps above add this block to
`%APPDATA%\MyCommander\file-actions.conf` (with single spaces around each `|`). You can also
write it there directly. Open that file, **not** `mycommander.ini` (`Ctrl+F11`), and add, below
the comments:

```ini
[.md]
view = Edge      | C:\Program Files (x86)\Microsoft\Edge\Application\msedge.exe | "%f"
open = Notepad++ | C:\Program Files\Notepad++\notepad++.exe | "%f"
```

With only one `view` and one `open` line in the block, no `*` is needed. A `*` is harmless if you
add one anyway.

**If you already have other blocks that list `.md`,** the first one from the top decides, so put
this block above them. Assign File to App does that for you: it always adds to a block for that
one extension, placed above any block shared with other file types, and never edits the shared
block itself, so other file types keep their programs.

**If your file is from MyCommander 1.11.0's first run,** it may end with this block:

```ini
[.txt .log .md .ini .conf]
view* = Built-in viewer | internal:viewer |
```

That block only repeats the built-in fallback, but being near the top it used to win over your own
`.txt` and `.md` blocks. MyCommander now removes exactly this block automatically the next time it
starts or you press `F3`. Nothing else in the file is touched. You can also simply delete those
two lines yourself.

Spaces in a program path are fine. Quotes around the program are optional:
`"C:\Program Files (x86)\…\msedge.exe"` works too. The `"%f"` quotes
matter, though: they keep a file path with spaces (like `C:\My Notes\todo.md`) in one piece.

**3. Use it.** With the cursor on `README.md`:

| Key | What happens |
|---|---|
| `F3` | Edge opens and shows `README.md`. |
| `Shift+Enter` | Notepad++ opens `README.md`, ready to edit. |
| `Shift+F3` | A list of all four choices — `* view Edge`, `view Built-in viewer`, `* open Notepad++`, `open Windows default` (`*` marks the defaults) — handy for a quick look in the built-in viewer without leaving MyCommander. |
| `Enter` | The same as `F3` (Edge), unless you set `enterFileAction = open` in your [settings](#settings). |

MyCommander stays usable while Edge and Notepad++ are open. When you save in Notepad++, the file's
size and date in the panel update the next time the panel is refreshed (`Ctrl+R`).

**About how Edge shows Markdown.** Edge has no built-in Markdown renderer. Without help it shows
the raw text, or may offer to download the file instead. For formatted headings, lists and links,
install a Markdown viewer extension from the Edge Add-ons store and turn on its **Allow access to
file URLs** option (Edge → Extensions → the extension's Details). Once that's set up, `F3`
shows the rendered page.

**Making `F4` use Notepad++ too.** File actions cover viewing (`F3`) and opening (`Shift+Enter`);
`F4` (edit) is separate and always uses the program named by the `EDITOR` environment variable
(Notepad if it isn't set). To make `F4` use Notepad++ for every file, run this once in a terminal:

```
setx EDITOR "C:\Program Files\Notepad++\notepad++.exe"
```

Then close and reopen the terminal and MyCommander, since a new setting only reaches newly
started programs.

**The same idea for other files.** Add more extensions to the block's first line, like
`[.md .markdown .txt]`, to give all of them the same pair of programs.

### Examples

**View text files in Notepad++, but keep the built-in viewer one key away.** `F3` opens Notepad++
in read-only mode; `Shift+F3` still offers the built-in viewer:

```ini
[.txt .log .ini .md]
view* = Notepad++ (read-only) | C:\Program Files\Notepad++\notepad++.exe | -ro "%f"
```

**Open PDFs in a specific reader, with Windows' own choice as a second option:**

```ini
[.pdf]
open* = SumatraPDF      | C:\Program Files\SumatraPDF\SumatraPDF.exe | "%f"
open  = Windows default | system: |
```

**Images: view them in IrfanView, open them in Paint for editing:**

```ini
[.jpg .jpeg .png .gif .bmp]
view* = IrfanView | C:\Program Files\IrfanView\i_view64.exe | "%f"
open* = Paint     | mspaint.exe | "%f"
```

**Compressed archives, using a wildcard pattern.** `*.tar.gz` matches `backup.tar.gz` but not
`notes.gz`, which an extension entry `.gz` would also catch:

```ini
[*.tar.gz *.7z .rar]
open* = 7-Zip File Manager | C:\Program Files\7-Zip\7zFM.exe | "%f"
```

**Files named `README` (with or without an extension) in a Markdown viewer:**

```ini
[readme*]
view* = Markdown preview | C:\Tools\mdview.exe | "%f"
```

**Using other placeholders — compile a C file into an `.exe` of the same name, in its own
folder:**

```ini
[.c]
open = Compile with gcc | gcc.exe | "%f" -o "%d\%b.exe"
```

For `C:\src\hello.c`, this runs `gcc.exe "C:\src\hello.c" -o "C:\src\hello.exe"`.

**A catch-all: offer Notepad for anything not covered above.** Keep this block last. Any block
above it that matches the file and has an `open` line decides first. For every other file, this
becomes the `open` program:

```ini
[*]
open = Notepad | notepad.exe | "%f"
```

**Use the program's real `.exe`.** The program must be an executable (`.exe`), not a `.cmd`/`.bat`
script. Some tools' command-line names are really scripts — VS Code's `code`, for example, is
`code.cmd` — so point at the actual executable instead (replace `<you>` with your user name):

```ini
[.json .yaml]
open* = VS Code | C:\Users\<you>\AppData\Local\Programs\Microsoft VS Code\Code.exe | "%f"
```

Prefer windowed programs. A text-mode (console) program would share MyCommander's own window
while it runs, so its output would mix with MyCommander's screen.

**Make `Enter` open files instead of viewing them.** By default, `Enter` on a file does the same
as `F3`. To make it do the same as `Shift+Enter` instead, set this in your
[settings](#settings) file:

```ini
enterFileAction = open
```

A few things to know:

- `Enter` on a `.zip` file still browses into it (when the ZIP plugin is installed); use
  `Shift+Enter` or `Shift+F3` to open it in another program instead.
- `Shift+Enter` with something typed on the command line runs the command, the same as `Enter`.
- If a program can't be started (a typo in its path, for instance), MyCommander shows a message
  naming the program and the reason.

## Finding files

| Key | What it does |
|---|---|
| `Ctrl+F` | Filter the active panel's listing by name or wildcard; submit an empty filter to clear it. |
| `Alt+F7` | Find files below the active panel's folder — by name, and optionally by the text inside them. |

`Alt+F7` opens a short form (`↑`/`↓` move between fields, `Space` or `←`/`→` change a choice):

| Field | What it does | Default |
|---|---|---|
| File name mask | Which files and folders to look at. A pattern with `*` or `?` is a wildcard; plain text is matched anywhere in the name (so `log` finds `app.log.txt`) | `*` |
| Containing text | Text to look for **inside** files. Leave it empty to search by name only | empty |
| Regular expression | Treat the text as a regular expression (for example `TODO\|FIXME` or `^\s*class `) | off |
| Match case | Upper/lower case must match exactly | off |
| Encoding | How to read the files: **Auto** (recognizes UTF-8 and UTF-16 by their marker, otherwise UTF-8 or your Windows ANSI code page line by line), or force UTF-8, ANSI, UTF-16LE or UTF-16BE | Auto |
| Skip binary files | Don't look inside programs, images and other non-text files | on |

The form remembers your last search until you quit MyCommander.

While a content search runs, the top line shows how many files have been searched and which one is
being read; `Esc` stops it promptly, even in the middle of a big file.

**Results** open in their own scrollable list: `↑`/`↓`/`Page Up`/`Page Down`/`Home`/`End` move
through them, `Enter` jumps the active panel straight to the selected result, and `Esc` or `F10`
close it. For a content search, each file is shown with its **first matching line** —
`C:\src\main.cpp  :42:  // TODO tidy up` — and `F3` opens it in the viewer **at that line**, with
the text ready so `n` jumps to the next occurrence. (After a regular-expression search, `n` isn't
pre-loaded, because the viewer's own search is plain text.) The title also says how many files
were skipped because they couldn't be read (another program has them locked, or no permission) or
look binary.

Good to know:

- Text is matched line by line — a match never spans two lines.
- Upper/lower case is ignored by default, accented letters included (`árvíztűrő` finds
  `ÁRVÍZTŰRŐ`).
- Very long lines are searched in their first 65 536 characters, or 1 024 for a regular
  expression; this keeps unusual files (minified code, data dumps) from slowing the search to a
  crawl.
- Folders are never content results — only files.

## Comparing folders

Press `Shift+F2` (or choose **Selection → Compare Folders**) to compare the folder in the left
panel with the folder in the right panel. A short form asks how:

| Field | What it does | Default |
|---|---|---|
| Compare by | **Name** (only "is it on both sides?"), **Name + size**, **Name + size + timestamp**, or **Name + size + content** (reads the files and compares them byte by byte) | Name + size + timestamp |
| Include subfolders | Also compare everything inside the subfolders | off |
| Timestamp tolerance (seconds) | Times this close count as equal. USB sticks (FAT/exFAT) and many network drives round times to 2 seconds | 2 |
| Ignore exact 1-hour difference | Treat files exactly one hour apart as equal — the classic daylight-saving / FAT shift | off |
| Show the results list | Show the full list afterwards; off just selects the differences and tells you the counts | on |

The form remembers your choices until you quit MyCommander. `Esc` stops a long comparison — even
in the middle of reading two big files.

**What you get:** the differences are **selected in both panels** (your previous selections are
replaced):

- files and folders that exist **only on that side**;
- the **newer** file, where both sides have it but the times differ;
- **both** files, where they differ but neither is newer (for example, a different size with the
  same time);
- with subfolders on, a folder that has **any difference inside it**.

Identical items are left unselected.

**Example — bring a backup up to date:** left panel `D:\Work`, right panel `E:\Backup\Work`. Press
`Shift+F2`, `Enter`. The left panel now has everything that's new or changed since the last backup
selected; press `F5` to copy exactly those to the backup and confirm overwriting the older copies.
Press `Shift+F2` again — everything is now `=`.

**The results list** shows one row per item: a status, the name (with its subfolder path), then
the size and time on the left and on the right.

| Mark | Meaning |
|---|---|
| `=` | Identical |
| `->` | Only on the left |
| `<-` | Only on the right |
| `>` | The left one is newer |
| `<` | The right one is newer |
| `!=` | Different, neither is newer (or a folder with differences inside) |
| `!` | A file on one side, a folder with the same name on the other |
| `?` | Couldn't be read (another program has it locked, or no permission) |

`Tab` switches between **all**, **differences** (shown first when there are any), **left only**
and **right only**. `Enter` moves both panels to the chosen item (into its subfolder, if it's
deeper); `F3` views the left file and `Shift+F3` the right one; `Alt+F3` shows the differences
between the two files (see [Comparing two text files](#comparing-two-text-files)); `Esc` closes the
list and keeps the selections.

Good to know:

- Each side compares what its panel shows: hidden and system files only if that panel shows them
  (`Ctrl+H`), and only the files matching that panel's filter (`Ctrl+F`).
- Upper/lower case in names doesn't matter — `Readme.md` and `README.md` are the same file.
- Links and junctions are compared by name and time but never followed or read.
- **Content** is exact (no "checksum" guesswork) and only reads files whose sizes are equal; files
  that are equal byte for byte are identical even if their times differ.
- In the results list, `F5` goes straight on to **Synchronize** (next section) with the same
  settings.

## Synchronizing folders

Press `Ctrl+Shift+F2` (or choose **Commands → Synchronize Folders**, or press `F5` in the compare
results) to make the two panels' folders match. **Nothing is changed until you've looked at a
preview and confirmed it.**

| Field | What it does | Default |
|---|---|---|
| Direction | **Left → Right** (make the right side a copy of the left), **Right → Left**, or **Both ways** (each side gets what it's missing and the newer version of each file) | Left → Right |
| Compare by | **Name + size + timestamp**, or **Name + size + content** (byte-by-byte; slower, exact) | timestamp |
| Include subfolders | Synchronize the whole tree | on |
| Delete files only on the target | **Mirror** mode: files that exist only on the target side are deleted (to the **Recycle Bin**). One-way only | off |
| Timestamp tolerance / Ignore exact 1-hour difference | As in [Comparing folders](#comparing-folders) | 2 s / off |

**The preview** lists every planned action:

| Mark | Action |
|---|---|
| `->` / `<-` | Copy to the right / to the left |
| `=>` / `<=` | Copy and **overwrite** the older file |
| `DEL>` / `<DEL` | Delete on the right / left (Recycle Bin) |
| `??` | A conflict MyCommander won't decide for you — nothing happens unless you choose |
| (blank) | Nothing to do |

The top line totals everything up. Change anything you like before going ahead:

- `Space` switches an item's action off (or back on);
- `→` / `←` copy that item to the right / left instead — this is how you settle a conflict;
- `Del` deletes it on the target (only in mirror mode, and only for files that exist only there);
- `Tab` switches between **differences** (the default: everything that differs, including items
  nothing will happen to — for example, in Left → Right, a file that exists only on the right;
  press `←` on it to copy it back), **conflicts** and **everything**;
- `F3` and `Shift+F3` view the left / right file.

Press `F5` to continue, or `Esc` to walk away with nothing changed.

**The confirmation** then spells out how much is copied each way and lists the files that will be
**overwritten** or **deleted** by name. Its default button is **Back to preview**, so a stray
`Enter` never starts it; press `S` (or move to *Synchronize* and press `Enter`) to go ahead. This
question is always asked — there is no setting to turn it off.

While it runs you see the usual copy progress, and `Esc` stops it. All copies happen first and
deletions last, so stopping early never leaves you with files deleted but not copied. Anything
that changed after you looked at the preview is skipped and listed at the end ("changed since the
preview"), never overwritten or deleted on a guess.

**When MyCommander leaves something alone (a conflict):**

- in a one-way sync, the target has the **newer** file — maybe someone edited the backup;
- in a two-way sync, both files differ but neither is newer;
- a file on one side has the same name as a folder on the other.

**Example — keep a backup up to date:** left `D:\Work`, right `E:\Backup\Work`. Press
`Ctrl+Shift+F2`, keep **Left → Right**, tick **Delete files only on the target**, `Enter`. The
preview shows new files `->`, changed files `=>` and files you deleted from `D:\Work` as `DEL>`.
`F5`, check the list, `S`. Afterwards `Shift+F2` shows everything `=`, and the deleted files are in
the Recycle Bin if you need them back.

**Example — a laptop and a USB stick, both edited:** **Both ways**. Files new on either side are
copied across and the newer version of each changed file wins. If the same file was changed on
both sides with the same time, it shows as `??`: view both with `F3` / `Shift+F3`, then pick a
direction with `→` or `←`.

Good to know:

- **Two-way sync can't see deletions.** If you delete a file on one side, the next two-way sync
  copies it back from the other side. Use a one-way sync with *Delete files only on the target*
  when you want deletions carried over.
- Very large trees: if the comparison reaches its 200 000-item limit, MyCommander refuses to
  synchronize rather than act on half a picture — filter the panels (`Ctrl+F`) or pick a
  subfolder.
- It won't synchronize a folder with one inside it, or folders a background job is still working
  on.

## Comparing two text files

Press `Alt+F3`, or choose **File → Compare contents**, to see what's different between two text
files. MyCommander picks the two files like this:

- **two files selected** in the active panel: those two;
- **one file selected in each panel**: those two;
- otherwise the file under the cursor (or the one selected file) in the active panel, and the
  **file with the same name** in the other panel — or, if there's none, the file under the other
  panel's cursor.

The file from the left panel is always shown on the left. In the [folder comparison](#comparing-folders)
results, `Alt+F3` compares the left and right copy of the chosen file.

The two files are shown **side by side**, line by line, with line numbers. Removed lines (only on
the left) are red and marked `<`, added lines (only on the right) are green and marked `>`, and
changed lines are yellow and marked `~`, with the characters that actually changed highlighted.
On a narrow window — or when you press `U` — the comparison is shown in one column instead, with
`-` and `+` in front of the lines.

| Key | What it does |
|---|---|
| `↑`/`↓`, `Page Up`/`Page Down`, `Home`/`End` | Scroll both files together |
| `←`/`→` | Scroll sideways (long lines) |
| `F7` / `Shift+F7` (or `n` / `p`) | Next / previous difference |
| `U` | Side by side ↔ one column |
| `I` | Ignore upper/lower case on or off |
| `W` | Whitespace: exact → ignore trailing spaces → ignore all spaces and tabs |
| `F3` / `Shift+F3` | Open the left / right file in the viewer at this line |
| `Esc`, `F10` or `q` | Close |

The bottom line shows how many lines were removed (`-`), added (`+`) and changed (`~`), and the
options in use. Each file's encoding is recognized on its own (UTF-8, ANSI, or UTF-16 with a byte
order mark), and Windows (CRLF) and Unix (LF) line endings count as the same. If the files are
identical, you just get a message saying so.

**Binary and very large files** (over 64 MB or 200,000 lines) are compared byte by byte instead:
you're told whether they're identical, or at which byte they first differ. A comparison of very
different files can take a moment; `Esc` stops it.

**Your own comparison program.** If you prefer a tool such as WinMerge, add a line like this to
the configuration file (`Settings` → `Edit settings`):

```
diffProgram = "C:\Program Files\WinMerge\WinMergeU.exe" "%1" "%2"
```

`%1` and `%2` are replaced by the left and right file. `Alt+F3` then starts that program, and
`Shift+Alt+F3` still opens the built-in comparison.

## The command line and running programs

The line just above the status bar is always live — you can type into it any time, and it doesn't
interfere with navigating with the arrow keys or function keys.

- **Edit it like a real text field:** it has a positionable cursor, not just an append-at-the-end
  box. `←` / `→` move the cursor one character, `Home` / `End` jump it to the start/end,
  `Backspace` erases the character before it, and `Delete` forward-deletes the one at it — all
  only while the command line has text, so these keys still do their usual panel job once it's
  empty.
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

## The menu bar

An always-visible row across the very top of the screen — `File Selection Commands Network View
Settings` — for browsing to a command instead of remembering its key. Press `Alt+F`, `Alt+S`,
`Alt+C`, `Alt+N`, `Alt+V`, or `Alt+E` to open the matching menu (`Settings` uses `E`, matching its
own "Edit settings" item, since `S` is already `Selection`'s), or just click it.

While a menu is open:

| Key | What it does |
|---|---|
| `←` / `→` | Switch to the adjacent menu |
| `↑` / `↓` | Move within the open menu |
| `Enter` | Choose the highlighted item |
| A bracketed letter (e.g. the `M` in `[M]odify Attribs`) | Jump straight to and choose that item |
| `Esc` | Close the menu without choosing anything |

Clicking anywhere outside an open menu closes it the same way `Esc` does.

Most menu items already have their own keyboard shortcut elsewhere — the menu is just a second,
browsable way to reach the exact same action (for example, `Modify Attribs` is the same as
`Ctrl+A`, and `Find` is the same as `Alt+F7`). **File → Assign File to App** sets up which
programs view and open each kind of file (see
[Choosing which program opens a file](#choosing-which-program-opens-a-file)). A few items — mostly
under `Network` — name features MyCommander doesn't have yet. Choosing one of those shows "Not yet
implemented" rather than pretending to work.

## Browsing ZIP archives

With the ZIP plugin installed, press `Enter` on a ZIP file to browse its contents. The same works
for packages that are ZIP files inside: `.jar`, `.war`, `.ear`, `.apk`, `.xpi`, `.vsix`, `.nupkg`
and `.whl` (File → Unpack Package extracts them too). Office and e-book files (`.docx`, `.xlsx`,
`.odt`, `.epub`, …) are ZIP files as well, but `Enter` keeps opening them in their own program.
Changing a signed package (`.jar`, `.apk`, `.vsix`) inside MyCommander breaks its signature, as
with any ZIP tool.
Use `Insert` or right-click to mark entries; right-drag marks multiple entries,
and `Ctrl`-click selects a range. Selected entries are yellow; the cursor has a
blue background. `F1` includes the ZIP shortcuts in the application's help.

`F3` views the current file. `F5` extracts the selection to the other panel's
directory, and `F6` extracts and removes successfully copied files from the ZIP.
`Ctrl+A` toggles stored file attributes. `F8` confirms deletion from the archive.
`Shift+F5` copies the current item under a new name (or relative path) inside the ZIP.
In choice dialogs, use the displayed hotkey or arrows and `Enter`; `Esc` cancels.
`Backspace` goes to the parent archive folder, or confirms leaving the ZIP at its root.

## Connecting to an FTP server

With the FTP plugin installed (the installer includes it), the **Network** menu has these items:

- **New FTP connection** opens a form: a name for the connection, the **host** (you can also paste a
  whole address like `ftp://bob@ftp.example.com:2121/pub` there — it's split into the other
  fields), **port** (21), **user** (`anonymous` for public servers), **password** (shown as `***`),
  the folder to start in, **passive mode** (leave it on unless the server says otherwise), a
  **timeout**, and whether to **save the connection** and **save the password**.
- **FTP connection** lists your saved connections: pick one, then **Connect**, **Edit**,
  **Delete** or **New connection**.
- **Disconnect FTP** appears while a panel is connected to a server and disconnects it (the active
  panel's server if it shows one, otherwise the other panel's) — the same as `Backspace` at the
  server's top folder, without having to go there first.

**About passwords:** plain FTP sends your password over the network **unencrypted** — use it for
public or test servers, not for anything sensitive. MyCommander never shows a password, never
writes it to its log, and never stores it in its files. If you tick *Save the password*, it's kept
in the **Windows Credential Manager** (Control Panel → Credential Manager → Windows Credentials,
as `MyCommander/FTP/<name>`); otherwise you're asked each time you connect. Every login password
prompt has the same **Save the password (Windows Credential Manager)** box: tick it once and you
won't be asked again (this also saves the connection, if it wasn't saved yet); untick it to make
MyCommander forget a stored password.

Once connected, the **active panel shows the server** (its address is on the top line). It works
like a folder:

| Key | What it does |
|---|---|
| `Enter` | Open a folder; on a file, view it |
| `Backspace` (or `Enter` on `..`) | Parent folder — at the server's top folder it asks whether to disconnect |
| `F3` | View the file (a temporary copy, deleted afterwards) |
| `F5` | **Download** the selected files and folders to the other panel's folder |
| `F6` | Download, then delete them on the server (asks first) |
| `F5` in the **other** panel | **Upload** its selection into the server folder you're looking at |
| `F7` | Create a folder on the server |
| `F2` | Rename |
| `F8` / `Delete` | Delete on the server — always asks; there's no Recycle Bin on a server |
| `Ctrl+R` | Re-read the folder |
| `Insert`, `Shift+↑/↓`, `Num+`/`Num-`/`Num*`, right-click | Select, as in a normal panel |
| `Esc` | Stop a running download or upload |

If a file already exists you can overwrite, skip or rename it (or answer for all of them). A
download is written to a temporary file first, so a failed or stopped download never leaves half a
file behind or damages the file you already had. If the connection drops, MyCommander offers to
**reconnect** and stays in the same folder. A server that stops answering gives up after the
timeout instead of freezing the window.

**Linked folders.** A symbolic link to a folder on the server opens with `Enter` like any folder.
Copying never goes into it, and deleting it removes only the link, never what it points to. A
download skips linked folders, and the log says which. The same rule applies on SFTP servers.

Saved connections live in `%APPDATA%\MyCommander\ftp-connections.conf` (plain text, no passwords).
Not supported yet: encrypted FTP (FTPS), resuming an interrupted transfer, and copying directly
between two servers. For an encrypted connection, use SFTP (next section).

## Connecting to an SFTP (SSH) server

SFTP transfers files over an encrypted SSH connection, so it's the safe choice for anything
that matters. With the SFTP plugin installed (the installer includes it), the **Network** menu
has:

- **New SFTP connection**: a form with a name, the **host** (or a whole address such as
  `sftp://bob@example.com:2222/srv` — it's split into the other fields), **port** (22), **user**
  (your Windows user name by default), **Log in with**, the **key file**, the **password / key
  passphrase** (shown as `***`), the folder to start in, a **timeout**, and whether to **save the
  connection** and **save the password**.
- **SFTP connection**: your saved connections. Pick one, then **Connect**, **Edit**, **Delete** or
  **New connection**.
- **Disconnect SFTP**: shown while a panel is connected; disconnects it.

**Log in with** offers three ways to log in:

- **Password**: the account's password. If you leave it empty in the form, you're asked when you
  connect. Servers that ask questions instead (for example a one-time code) are answered the same
  way, each in its own prompt.
- **Key file**: a private key file, by default the first of `id_ecdsa`, `id_rsa` or `id_ed25519`
  in your `.ssh` folder. The built-in SSH library can use:
  - **RSA keys in PEM format** (`-----BEGIN RSA PRIVATE KEY-----`), with or without a passphrase;
  - **ECDSA keys** without a passphrase.

  If the key has a passphrase, you're asked for it.

  **Your key file may not work directly.** Recent `ssh-keygen` versions save keys in the newer
  OpenSSH format by default, including RSA keys such as a typical `.ssh\id_rsa`. To check yours,
  open it in a text editor and look at the first line:

  - `-----BEGIN RSA PRIVATE KEY-----`: works as a key file;
  - `-----BEGIN OPENSSH PRIVATE KEY-----`: works only if it's an ECDSA key without a passphrase.

  Newer-format RSA keys and **Ed25519** keys don't work as a key file. MyCommander says so when you
  connect and suggests two fixes:

  - **Use the SSH agent** (next item). This works with every key type.
  - **Convert an RSA key once** to the older PEM format: `ssh-keygen -p -m PEM -f <your key file>`.
    This rewrites the file in place; the key itself doesn't change, so servers keep accepting it.
    Make a copy of the file first if you're unsure.
- **SSH agent**: the keys held by the Windows **OpenSSH Authentication Agent** service (add a key
  once with `ssh-add`) or by Pageant. This works with every key type, and MyCommander never sees
  the private key. It's the default for a new connection while an agent is running.

  The Windows agent service is often **disabled**. To turn it on, open **Services**, find
  **OpenSSH Authentication Agent**, set *Startup type* to **Automatic**, and click **Start**. Or
  run this in an administrator PowerShell:

  ```
  Set-Service ssh-agent -StartupType Automatic
  Start-Service ssh-agent
  ```

  Changing a service needs administrator rights, once; MyCommander itself never asks for them. Then
  add your key in a normal terminal, entering its passphrase if it has one:
  `ssh-add $env:USERPROFILE\.ssh\id_rsa`. The agent keeps the key across restarts.

**The server's identity (host key).** The first time you connect to a server, MyCommander shows
its key fingerprint, such as `SHA256:OzvpQxRUzSfV9F/ECMXbQ7B7zbK0aTngrhFCBUno65c`. Compare it with
the one the server's administrator gives you, then choose:

- **Trust and remember**: saved in `%APPDATA%\MyCommander\known_hosts`;
- **Once**: just this connection;
- **Cancel**.

Servers you already trust in Windows' own `ssh` (`%USERPROFILE%\.ssh\known_hosts`) connect without
asking. MyCommander only reads that file, never changes it.

If a server's key ever **changes**, the connection is **refused** with a warning showing the new
fingerprint and the file and line holding the old one. A changed key can mean someone is
intercepting the connection; it can also mean the server was simply reinstalled. If you're sure
the change is expected, delete that line, then connect again: there's deliberately no "connect
anyway" button.

**Passwords and passphrases** are never shown, logged or written to MyCommander's files. *Save
the password* keeps one in the **Windows Credential Manager** (as `MyCommander/SFTP/<name>`), and
every login prompt offers it, the same as for FTP.

Once connected, the server panel works **exactly like the FTP panel** (see the key table above):
browse, `F3`, `F5`/`F6` download, `F5` in the other panel to upload, `F7`/`F2`/`F8`, `Ctrl+R`,
conflicts, reconnect and `Esc`. The panel also shows Unix permissions. **Linked folders** follow
the same rule as on FTP: a symbolic link to a folder opens with `Enter`, but copying never goes
into it, and deleting it removes only the link, never what it points to.

Saved connections live in `%APPDATA%\MyCommander\sftp-connections.conf` (plain text, no
passwords). Not supported yet: changing permissions, resuming an interrupted transfer, background
transfers, copying directly between two servers, and SSH jump hosts or `~/.ssh/config` settings.

## Single-panel layout

Press `Ctrl+F1`, or choose `View` → `Single panel`, to show only the active panel across the whole window —
handy for long file names or a narrow window. Press `Ctrl+F1` again to get both panels back.

The other panel isn't closed, just hidden: it keeps its folder, cursor and selection (and an open ZIP file or
FTP/SFTP server). The bottom border shows where it is, for example ` Other panel: D:\Backup `, because it's
still the target of everything that works "with the other panel":

- `F5`/`F6` copy or move there (the prompt shows the folder, so you can check or change it);
- `Shift+F2` compares the two folders, `Ctrl+Shift+F2` synchronizes them.

`Tab` switches which panel is shown. `Alt+F1` and `Alt+F2` choose a drive for the left or right panel and show
that panel. MyCommander remembers the layout the next time you start it. To use a different key, add a
`single-panel = ...` line to `keys.conf` (`Settings` → `Edit key bindings`).

## Directory tree

Press `Ctrl+T`, or choose `View` → `Tree`, to show the active panel as a tree of its drive's folders instead
of a file list. The tree opens already expanded down to the folder you were in, with that folder selected.
Subfolders load when you expand them. Junctions and symbolic links are marked `<REPARSE>` and aren't expanded
automatically. Folders that can't be read are marked `<BROKEN>`.

`↑`/`↓`, `Page Up`/`Page Down`, and `Home`/`End` move the cursor. `→` expands the cursor folder, and `←`
collapses it (or moves to its parent if it's already collapsed). `+` and `-` expand and collapse it without moving
the cursor — on the numpad or the main keyboard, whatever your keyboard layout (on a Hungarian keyboard,
`Shift+3` types `+` and works too). `Enter` or a double-click goes to the cursor
folder and returns the panel to a normal file list there. `Esc` or `Ctrl+T` again leaves the tree without moving.
The other panel stays a normal listing, so you can still copy or move into it. The expanded state isn't saved
between sessions.

## Background jobs

`Alt+F5` (copy) and `Alt+F6` (move) queue the selection to run in the background, so you can keep working while it
copies. They ask for the destination the same way F5 and F6 do. Jobs on the same drive run one after another; jobs
on different drives run together. If a copy meets a name clash or a failure, it stops and asks you what to do, just
like F5. `Alt+Q` shows the job list. Press `Enter` on a job to cancel it. The window title shows how many jobs are
running and queued. If you press `F10` while jobs are still running, you can wait for them or cancel them and quit.
Jobs aren't saved, so they don't continue after a restart.

`Alt+F8` queues a delete to the Recycle Bin, and `Alt+Shift+F8` a permanent delete. They ask the same
confirmation as `F8` / `Shift+F8` (unless you've turned that confirmation off in [Settings](#settings)). If an item
can't be deleted, the job stops and offers Retry, Skip, Skip All, or Cancel for that item.

**Jobs never step on each other.** A job that uses the same files or folders as another job waits until that one is
finished, whatever drive they're on. For example, if you queue a copy of a folder and then a delete of that same
folder, the delete waits until the copy is done. Jobs on unrelated files still run straight away.

**Files a job is using are protected.** While a job is running, the ordinary keys `F5`, `F6`, `Shift+F5`, `F8`,
`F2`, `Ctrl+A`, `F7` and `Shift+F7` refuse to touch its files and folders. A message names the job; wait for it to
finish, or cancel it with `Alt+Q`. Moving around, viewing and searching are never blocked.

**Administrator rights:** a background job never asks for them. If an item fails with "Access is denied", skip it
and run that item again with the ordinary `F5`, `F6` or `F8`, which offers **Elevate & Retry**.

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
- **Click a top menu entry** to open it, **click an item** to choose it, or **click anywhere else**
  to close an open menu without choosing anything.

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

## Favorite folders (directory hotlist)

Keep a list of folders you use often and jump to any of them in two keystrokes.

| Key | What it does |
|---|---|
| `Ctrl+D` | Open the directory hotlist (also **Commands → Directory Hotlist**) |

In the hotlist:

| Key | What it does |
|---|---|
| `↑` / `↓` / `Page Up` / `Page Down` / `Home` / `End`, mouse wheel | Move |
| `Enter` or a click on a favorite | Go there — the active panel opens that folder |
| `Enter` on **+ Add current folder** | Add the folder the active panel shows now, under a name you type |
| `Del` | Remove the favorite under the cursor (asks first) |
| `F2` | Rename the favorite under the cursor |
| `Esc` | Close |

When the folder you're in is already a favorite, the hotlist opens with that entry selected.

**Adding a favorite:** go to the folder, press `Ctrl+D`, choose the last row,
**+ Add current folder (…)**, and confirm or change the suggested name (the folder's own name).
If the folder is already in the list, MyCommander tells you its name instead of adding it twice.
After adding, removing, or renaming, the list stays open so you can keep going.

**The favorites file.** The list is saved straight away in a plain text file:

```
%APPDATA%\MyCommander\favorites.conf
```

One favorite per line, `name = path`, in the order they're listed:

```ini
# MyCommander directory hotlist (NAV-007). Ctrl+D opens it; Enter goes there.
Projects  = C:\munka\2026
Downloads = C:\Users\attil\Downloads
Team      = \\server\public\team
Árvíztűrő = D:\Dokumentumok\Árvíztűrő
```

To reorder the list, edit the file and save it; the next `Ctrl+D` shows the new order. A name
can't contain `=`, but a path can. Lines starting with `#` or `;` are comments; note that adding,
renaming, or removing a favorite from inside MyCommander rewrites the file and keeps only its
header comment. A line MyCommander can't read is reported once and skipped.

**Good to know:**

- A favorite on a drive that's unplugged, or a network share that's offline, still opens — the
  panel shows its usual "location can't be listed" message. Press `Backspace` or `Alt+←` to go
  back.
- A favorite that is itself a junction or symbolic link isn't opened, for the same safety reason
  `Enter` doesn't enter one in a listing. Add the folder it points to instead.
- While a panel is showing a ZIP archive or the directory tree, `Ctrl+D` goes to that view like
  every other key; use it from a normal file list.

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
| `enterFileAction` | What plain `Enter` does on a file: `view` (same as `F3`), `edit` (same as `F4`), `open` (same as `Shift+Enter`), or `execute` (run it with its Windows file association) | `view` |
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

Which programs view and open each kind of file live in a second file next to this one,
`file-actions.conf` — see [Choosing which program opens a file](#choosing-which-program-opens-a-file).

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

### Menu bar

| Key | Action |
|---|---|
| `Alt+F` | Open the File menu |
| `Alt+S` | Open the Selection menu |
| `Alt+C` | Open the Commands menu |
| `Alt+N` | Open the Network menu |
| `Alt+V` | Open the View menu |
| `Alt+E` | Open the Settings menu |
| `←` / `→` | Switch to the adjacent menu, while one is open |
| `↑` / `↓` | Move within the open menu |
| `Enter` | Choose the highlighted item |
| `Esc` | Close the menu without choosing anything |

### Navigation

| Key | Action |
|---|---|
| `Tab` | Switch active panel |
| `↑` / `↓` | Move cursor |
| `Page Up` / `Page Down` | Move a page at a time |
| `Home` / `End` | Jump to first / last entry, or move the command-line cursor if it has text |
| `Alt+←` / `Alt+→` | Go back / forward through location history |
| `Enter` | Open a folder, run the command line, or run the default file action |
| `Backspace` | Parent folder, or erase before the command-line cursor if it has text |
| `Alt+F1` / `Alt+F2` | Choose a drive for the left / right panel |
| `Ctrl+D` | Directory hotlist: favorite folders (`Enter` go, `Del` remove, `F2` rename) |

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
| `Ctrl+M` | Group rename (masks, counter, search/replace, case, preview) |
| `F5` | Copy |
| `Shift+F5` | Copy under a new name (same folder, or a full path) |
| `F6` | Move |
| `F7` | New folder |
| `Shift+F7` | New empty file |
| `F8` / `Delete` | Delete (Recycle Bin); `Delete` forward-deletes command-line text instead if it has any |
| `Shift+F8` | Delete permanently |
| `Ctrl+A` | Toggle a file attribute |

### Viewing & editing

| Key | Action |
|---|---|
| `F3` | View the current file (its default view program, or the built-in viewer) |
| `Shift+F3` | Choose from every program set up for the current file |
| `Shift+Enter` | Open the current file (its default open program, or the Windows default) |
| `F4` | Edit the current file externally |
| `Alt+Enter` | Show file properties |
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
| `Ctrl+T` | Show the active panel as a directory tree (see [Directory tree](#directory-tree)) |
| `Ctrl+F1` | Single-panel layout on/off (see [Single-panel layout](#single-panel-layout)) |
| `Alt+F5` / `Alt+F6` | Queue the selection as a background copy / move (see [Background jobs](#background-jobs)) |
| `Alt+F8` / `Alt+Shift+F8` | Queue the selection as a background delete (Recycle Bin / permanent) |
| `Alt+Q` | Show the background job queue |
| `Alt+F7` | Find files recursively — by name and/or by content (text or regular expression) |
| `Shift+F2` | Compare the two panels' folders and select the differences |
| `Ctrl+Shift+F2` | Synchronize the two panels' folders (preview, then confirm) |
| `Alt+F3` | Show the differences between two text files (see [Comparing two text files](#comparing-two-text-files)) |

### Command line

| Key | Action |
|---|---|
| `←` / `→` | Move the command-line cursor one character, if it has text |
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

The keys above are the defaults. You can change most of them — see
[Changing keyboard shortcuts](#changing-keyboard-shortcuts).

## Changing keyboard shortcuts

Choose **Settings → Edit key bindings**. The first time, this creates `keys.conf` (next to
`mycommander.ini`) listing every command with its default key, all commented out, and opens it in
your editor. Remove the `#` in front of a line, change its keys and save — the change applies at
once, no restart needed.

```
# command = key[, key...]
copy        = F5, Ctrl+C          # F5 keeps working, Ctrl+C copies too
find-files  = Ctrl+Shift+F        # Alt+F7 no longer opens Find files
shell       = none                # F9 does nothing now
```

- A command you list gets **exactly** the keys you write; one you don't list keeps its default.
  `none` leaves it without a key (the menu still has it).
- You can give a command several keys, separated by commas.
- Key names: `F1`…`F24`, `A`…`Z`, `0`…`9`, `Enter`, `Esc`, `Tab`, `Space`, `Backspace`, `Insert`,
  `Delete`, `Home`, `End`, `PgUp`, `PgDn`, `Up`, `Down`, `Left`, `Right`, `Num+`, `Num-`, `Num*`,
  `Num/`, `Num0`…`Num9`, with `Ctrl+`, `Alt+` and `Shift+` in front (in any order, any case).
- If you give a key to one command, the command that had it loses it. A default key you moved
  elsewhere does nothing afterwards.
- Everything that shows keys follows your choices: the `F1` list, the bottom hint bar and the
  shortcuts shown in the menus.
- Inside a ZIP archive or the folder tree your keys work too, and so do the default ones.

Some keys can't be changed, so MyCommander always stays usable: the arrow keys, `PgUp`/`PgDn`,
`Home`/`End`, `Enter`, `Esc`, `Backspace` and `Tab`; keys that type a character (letters, digits
and punctuation without `Ctrl` or `Alt` — and `Ctrl+Alt` + a letter, which is `AltGr` on many
keyboards); and `Alt+F/S/C/N/V/E`, which open the menus. Keys inside dialogs, the viewer and lists
like Find results stay as they are. **Quit** always keeps a key: if you leave it with none, `F10`
quits again.

If a line can't be used (a misspelt command, an unknown key, a key that's already taken), MyCommander
shows a short list of the problems when it loads the file and keeps every other line.

**Plugin shortcuts** can be changed too, as `plugin:<plugin-id>/<number> = key`. The plugin's
author documents its id and command numbers.

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

**`F3` or `Shift+Enter` opens the wrong program, or none.** Check, in this order:

1. **Is it in the right file?** File actions only work in `%APPDATA%\MyCommander\file-actions.conf`.
   Lines like `[.md]` or `view = …` in `mycommander.ini` (the file `Ctrl+F11` opens) are ignored,
   and MyCommander warns about them.
2. **Press `Shift+F3` on the file.** The list shows every program set up for it, with the one
   `F3`/`Shift+Enter` will run marked `*`. If your program isn't listed, the block's `[...]` line
   doesn't cover the file. Write `.txt`, not `txt`.
3. **Is another block above yours deciding?** The first matching block with a `view` (or `open`)
   line wins, so move your block higher.
4. **Is the program right?** The path must point to an existing `.exe`. If it can't be started,
   MyCommander says so, naming the program.

If a line in `file-actions.conf` has a mistake, MyCommander shows which line and why the next time
you press `F3`/`Shift+Enter`, and skips just that line.

**I want to check which version I have installed.** Run `MyCommander.exe --version` from a
terminal, or press `F1` inside the app — the very top of the shortcut reference shows the
installed version, alongside a short description and copyright information.

---

## Animated operation progress

Long ZIP open, copy, move and delete operations display an animated busy dialog.
The spinner updates four times per second even while a single file is being compressed
or read. A completed/total item counter and the current processing stage are shown when
available; counts restart when an operation changes to rebuilding the archive.
Confirmation dialogs temporarily suspend this display. Main-app file operations use
the same animation while retaining their speed information and Esc cancellation.

---

MyCommander is MIT-licensed, open-source software. Copyright (C) Attila Gallai 1985 - 2026.
