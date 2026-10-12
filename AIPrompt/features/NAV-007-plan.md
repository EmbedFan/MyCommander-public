# Implementation Plan — NAV-007 (Named favorite locations)

Source: `AIPrompt/my-commander.md`, `NAV-007`:

> The user shall be able to create named favorite locations.

- Priority P2, currently `LATER` / `Not Started`.
- It's also listed under §29 "Recommended Postponement Candidates" ("Favorites — History and direct
  path entry cover the initial need").

## Summary

Add a **directory hotlist**: a named, ordered list of folders the user can jump to from any panel in
two keystrokes. It follows Total Commander's `Ctrl+D` "Directory hotlist", which this app is modeled
on.

- **Open it:** `Ctrl+D` (or **Commands → Directory Hotlist**) shows the list in a dialog.
- **Go:** `Enter` takes the active panel to the chosen folder.
- **Manage it in the same dialog:**
  - **+ Add current folder** saves where the active panel is now, under a name you type (pre-filled
    with the folder's own name);
  - `Delete` removes an entry (asks first);
  - `F2` renames one.
- **Storage:** the list lives in its own small, hand-editable file. It is saved immediately on every
  change, so it survives an abnormal exit too.

### What exists to build on

| Piece | Where | Reused for |
|---|---|---|
| `ShowListChoice` + pure `ApplyListChoiceKey` | `Dialog.{h,cpp}`, `DialogInput.cpp` (FEA-0009) | The hotlist dialog itself (extended with extra keys, below) |
| `ShowTextPrompt` (real cursor, IS-0015/IS-0016) | `Dialog.cpp` | Typing or editing a favorite's name |
| `Panel::NavigateTo(dir)` | `Panel.{h,cpp}` (SRC-002/NAV-004) | Jumping. It already handles an unreachable location by showing LIST-006's "location inaccessible" banner instead of failing. |
| Atomic write (temp file + `MoveFileExW`) | `Session.cpp`, `CommandHistory.cpp`, `FileActions.cpp` | Saving the list |
| UTF-8 file read/write | `FileActions.cpp` (`ReadFileActionsText`/`SaveFileActionsText`) | Same need: folder paths with accents, e.g. `C:\Dokumentumok\Árvíztűrő` |
| `PathsEquivalent` / lexical normalization | `PathUtil.h` | Spotting "this folder is already a favorite" |
| `BuiltinShortcutInventory` | `PluginManagerLogic.cpp` | Reserving `Ctrl+D` so a plugin can't claim it |

`Ctrl+D` is free today. No built-in key and no shipped plugin (ZIP, Example) uses it.

## Design

### D1 — Storage: `%APPDATA%\MyCommander\favorites.conf`

It's a separate file, like `history.txt`, `session.ini` and `file-actions.conf`, not keys in
`mycommander.ini`. The data is an *ordered list of user records*, which the config file's one-line-per-key
machinery (`AppendMissingConfigKeys`, `UpdateConfigValue`, live reload) doesn't fit; this is the same
reasoning as DEC-011. The format is DEC-007-style plain text:

```ini
# MyCommander directory hotlist (NAV-007). Ctrl+D opens it.
# One favorite per line:  name = path      (order = list order; # or ; starts a comment)
Projects  = C:\munka\2026
Downloads = C:\Users\attil\Downloads
Share     = \\server\public\team
Árvíztűrő = D:\Dokumentumok\Árvíztűrő
```

- **Splitting a line:** at the **first** `=`. A name may not contain `=`; a path may, since `=` is a
  legal file name character. Both sides are trimmed. A line without `=`, or with an empty name or
  path, produces a CFG-005-style warning (line number and content) and is skipped, never fatal.
- **Encoding:** UTF-8, read with or without a BOM, falling back to ANSI. This is the same reader as
  `file-actions.conf`. Factor `FileActions.cpp`'s two helpers into a tiny shared header,
  `Utf8TextFile.h` (`ReadUtf8TextFile` / `WriteUtf8TextFileAtomic`), and make `FileActions.cpp` use
  it too. That way the encoding rules can't drift.
- **Not part of ERR-006's concern:** a favorite is a destination the user picks explicitly. Nothing
  ever navigates on its own at startup, so there is no replay risk. That's why, unlike `session.ini`,
  it's **written on every change**, not only at a clean exit.
- **Missing or empty file** means an empty list (CFG-003). The file is created on the first *Add*;
  there's no startup sample.

### D2 — The hotlist dialog (`Ctrl+D`)

Title: `Directory hotlist`. Rows, aligned via `TextWidth.h` (UI-011):

```
  Projects     C:\munka\2026
  Downloads    C:\Users\attil\Downloads
  Share        \\server\public\team
  + Add current folder (C:\munka\2026\MyCommander)
```

| Key | Action |
|---|---|
| `↑`/`↓`/`PgUp`/`PgDn`/`Home`/`End`, mouse wheel | Move (existing `ApplyListChoiceKey`) |
| `Enter` / click | On a favorite: the active panel goes there (`NavigateTo`), the dialog closes. On **+ Add current folder**: prompt for a name, then add it. |
| `Delete` | Remove the favorite under the cursor, after a Yes/No (FOP-008 spirit, even though only a list entry is lost) |
| `F2` | Rename the favorite under the cursor (`ShowTextPrompt`, pre-filled) |
| `Esc` | Close |

- After a Delete or F2, the dialog stays open with the cursor kept, so several edits can be made in
  one visit.
- **Adding:**
  - The name prompt is pre-filled with the folder's leaf name, or the drive / share root itself for
    `C:\` or `\\server\share`.
  - If the folder is **already** a favorite (lexically equal, case-insensitive, ignoring a trailing
    `\`), it's not duplicated; the cursor moves to the existing entry and a message says so.
  - New favorites are appended at the end; order is the file's order.
  - An empty name is rejected, and so is a name containing `=`.
- **Unreachable favorites** (unplugged USB drive, offline share) are not probed when the list is
  shown. Probing a network path can block for many seconds. Choosing one just navigates, and
  `Panel` shows its existing LIST-006 inaccessible banner, exactly like typing that path. The user
  can `Backspace`/`Alt+←` back as usual.
- **A favorite that is itself a junction or symlink** isn't entered. `Panel::NavigateTo`'s NAV-009
  guard refuses reparse points and shows its usual "cannot enter" status message, the same as
  pressing Enter on it in a listing. The plan keeps that safety rule rather than special-casing
  favorites. Favoriting the *real* target folder works.
- **Panel content:** if the active panel shows content (a ZIP archive or the tree), `Ctrl+D` goes to
  that content like every other key (FEA-0002 §8), so the hotlist isn't available there. That's
  consistent with drive selection and other navigation. The other panel and Tab still work.

### D3 — `ShowListChoice` gains optional "extra keys"

`Delete`/`F2` need the list dialog to return *which key* closed it, not just an index. Add an
overload; existing callers are unchanged:

```cpp
struct ListChoiceResult { int index = -1; WORD key = 0; };  // key: VK_RETURN for Enter/click, else the extra key
ListChoiceResult ShowListChoice(Console&, const std::wstring& title, const std::vector<std::wstring>& items,
                                const std::function<void()>& redraw, int initialIndex,
                                const std::vector<WORD>& extraKeys);
```

- An extra key closes the dialog with the cursor's index and that key.
- The hint line lists them, e.g. `Up/Down choose, [Enter] go, [Del] remove, [F2] rename, [Esc] close`.
- The existing `int ShowListChoice(...)` becomes a thin wrapper.
- `ApplyListChoiceKey` (pure, tested) is unchanged.

### D4 — Pure module `Favorites.{h,cpp}`

It has no `Console`/`Dialog` dependency (the CLAUDE.md precedent):

```cpp
struct Favorite { std::wstring name; std::filesystem::path path; };
struct ParseFavoritesResult { std::vector<Favorite> favorites; std::vector<std::wstring> warnings; };

ParseFavoritesResult ParseFavorites(const std::wstring& text);
std::wstring FormatFavorites(const std::vector<Favorite>&);   // header comment + aligned "name = path" lines
std::filesystem::path FavoritesFilePath();                    // %APPDATA%\MyCommander\favorites.conf
ParseFavoritesResult LoadFavoritesFrom(const std::filesystem::path&);
bool SaveFavoritesTo(const std::filesystem::path&, const std::vector<Favorite>&);

int  FindFavoriteByPath(const std::vector<Favorite>&, const std::filesystem::path&);  // -1 if none; lexical,
                                                                                       // case-insensitive, trailing '\' ignored
std::wstring DefaultFavoriteName(const std::filesystem::path& folder);  // leaf name, or the root itself
bool IsValidFavoriteName(const std::wstring&);                          // non-empty after trim, no '='
```

- **Rewriting the file on save:** `FormatFavorites` rewrites the whole file, keeping one standard
  header comment. Comments the user typed in by hand are **not** preserved. This is deliberately
  simpler than FEA-0009's line-level edits, because a hotlist has no structure worth commenting
  between entries.
- **Hand-editing still works** for reordering or bulk changes: open the file, edit, save. The next
  `Ctrl+D` re-reads it, because the list is loaded fresh each time the dialog opens. It's tiny.

### D5 — Wiring (`main.cpp`)

- `case 'D':` with `ctrlHeld` calls `DoDirectoryHotlist(console, left, right, leftActive, logger)`;
  without Ctrl, the existing `appendToCommandLine` fallback, like `'F'`/`'H'`/`'R'`/`'T'`.
- **`DoDirectoryHotlist`:**
  - load (warnings go to the log, and are shown once per change of the file, like
    `CurrentFileActions`);
  - build rows;
  - loop on `ShowListChoice(..., {VK_DELETE, VK_F2})`;
  - act;
  - on any change, `SaveFavoritesTo`; on a failed save, show a message and log a warning;
  - on go: `target.NavigateTo(path)`.
- **Menu:** Commands gains `real(L"Directory Hotlist", L'H', 'D', /*ctrl=*/true)` (MENU-002: the
  same action as the key). `H` is free among Commands' mnemonics (F, D, C, Y, V, J).
- **`BuiltinShortcutInventory`:** add `{'D', ctrl}` so a plugin registering `Ctrl+D` is reported as
  a conflict (PLUG-/FEA-0002 §12).
- **Hint bar:** not added. Rows are already crowded (IS-0007), and the key is listed in F1 help,
  README and USERGUIDE, the same treatment as `Ctrl+T`.

## Phases

1. **Phase 1 — pure logic + tests:**
   - `Utf8TextFile.h` (extracted, with `FileActions.cpp` switched to it; its tests must stay green);
   - `Favorites.{h,cpp}`;
   - `FavoritesTests.cpp`;
   - both vcxproj files updated.
2. **Phase 2 — UI:**
   - the `ShowListChoice` extra-keys overload;
   - `DoDirectoryHotlist`, `Ctrl+D`, the Commands menu item, the shortcut inventory.
3. **Phase 3 — docs:**
   - Help (new "Directory hotlist" rows), README, USERGUIDE (a section with an example file);
   - spec, version, changelog.

## Tests

New `Source/MyCommanderTests/FavoritesTests.cpp`:

- **Parse:**
  - names and paths trimmed;
  - comments and blank lines skipped;
  - order kept;
  - the first `=` splits, so `Odd = C:\a=b\c` gives path `C:\a=b\c`;
  - Unicode names and paths;
  - UNC and drive-root paths;
  - bad lines (no `=`, empty name, empty path) give one warning each, naming the line, and are
    skipped.
- **Format:** a parse → format → parse round trip is identical; the header comment is present.
- **Load/Save:**
  - a missing file is an empty list with no warning;
  - save then load is identical, including Unicode (UTF-8 bytes on disk);
  - the parent folder is created when missing;
  - all of it inside `TempDir` (TST-004).
- **`FindFavoriteByPath`:** `C:\Munka` vs `c:\munka\`, matching; a different folder, -1; a UNC path.
- **`DefaultFavoriteName`:**
  - `C:\munka\2026` gives `2026`;
  - `C:\` gives `C:\`;
  - `\\server\share` gives `\\server\share`.
- **`IsValidFavoriteName`:** empty or spaces are invalid; `a=b` is invalid; `Projekt Árvíztűrő` is
  valid.
- **Regression:** existing `FileActionsTests` stay green after the `Utf8TextFile.h` extraction.
- **`MenuBarTests`:** Commands contains "Directory Hotlist", a SimulatedKey `Ctrl+D`.
- **`PluginManagerTests`:** if there's an inventory test, `Ctrl+D` is reported as built-in.

Manual check:
- `Ctrl+D` on an empty list shows only **+ Add current folder**;
- add two folders;
- `Enter` goes there;
- `F2` renames, `Delete` removes (with a confirmation);
- the file looks right in an editor;
- a favorite on an unplugged drive shows the inaccessible banner;
- the same works from **Commands → Directory Hotlist**.

## Spec, docs, version

- **`AIPrompt/my-commander.md`:**
  - `NAV-007`: Disposition `LATER` → `IN-MVP`, Implemented;
  - optionally sharpen its wording: "…named favorite locations (directory hotlist, `Ctrl+D`), stored
    per user";
  - §29's "Favorites" candidate row gets a note "(implemented in 1.16.0)". The ID is never removed
    (CLAUDE.md rule);
  - a version-history note.
- **`CLAUDE.md`:**
  - a source-layout bullet for `Favorites.{h,cpp}` and `Utf8TextFile.h`;
  - add `Favorites` to the pure-module precedent list;
  - mention the `ShowListChoice` extra-keys overload in the `Dialog` bullet.
- **`Version.h`:** MINOR bump, 1.15 → 1.16 (new feature).
- **Changelog** entry.

## Risk / scope notes

- **Low risk:** it's purely additive. A new key, a new menu item and a new file; nothing existing
  changes behavior. The `ShowListChoice` overload keeps the old signature.
- **The `Utf8TextFile.h` extraction** touches `FileActions.cpp`. It's mechanical, and its full test
  suite guards it.
- **Out of scope** (natural follow-ups):
  - showing favorites inside the `Alt+F1`/`Alt+F2` drive picker;
  - per-favorite hotkeys (`Ctrl+Alt+1..9`);
  - submenus or groups (TC has them);
  - reordering from inside the dialog (edit the file instead);
  - "add the other panel's folder";
  - favorites inside a ZIP archive (a plugin path isn't a filesystem path).

## Open questions

1. **Key:** `Ctrl+D`, as in Total Commander, or something else?
2. **Hand-written comments:** rewriting the whole file on each in-app change drops comments you
   typed between entries (the header is kept). OK, or should in-app edits be line-level like
   `file-actions.conf`, which is more code?
3. **Drive picker:** also list favorites under the drives in `Alt+F1`/`Alt+F2` now, or leave that
   for later (planned: later)?

## Status

Implemented 2026-10-07, all three phases (version 1.16.0), with the plan's defaults for the open
questions: `Ctrl+D`; whole-file rewrite on in-app changes (header kept); drive picker later. Small
differences from the plan text:

- The extra-keys dialog is named `ShowListChoiceWithKeys` (not an overload of `ShowListChoice`), to
  avoid overload ambiguity with `ShowListChoice`'s default argument. It takes the hint line as a
  parameter.
- A quoted path (`Name = "C:\Program Files"`) is accepted and unquoted when read.
- Load warnings are shown once per distinct set per session (and always logged), rather than on
  every `Ctrl+D`.
