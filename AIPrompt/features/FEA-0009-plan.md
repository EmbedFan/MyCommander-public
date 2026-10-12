# Feature Plan — FEA-0009 (File Extension Actions)

Source: `AIPrompt/additional_feature_requests.md`

> Implement support for configurable actions associated with file extensions.
> - Allow registering different actions for specific file extensions (`.txt`, `.pdf`, `.zip`, `.jpg`, …).
> - Support at least these action types: **View** (display or preview the file) and **Open** (open
>   the file using a configured external or internal application).
> - Allow multiple programs/actions to be registered for the same file extension (View, Open).
> - Allow one registered action to be marked as the **default** for each action type.
> - Store: file extension, action type, program/executable, command-line arguments, optional
>   description/name.
> - Support placeholders in command-line arguments, especially the selected file path.
> - If no custom action is registered, allow falling back to the operating system's default file
>   association where applicable.
> - The registration mechanism should be easy to extend with additional action types later.

## Summary

This implements the long-parked **`VEE-009`** ("File associations shall be configurable by
extension or pattern", P2, LATER, Not Started).

### What exists today

| Key | Behavior | Code |
|---|---|---|
| F3 | Always the built-in text viewer (DEC-003). | `DoView`, `main.cpp:1420` |
| F4 | Always `%EDITOR%`, falling back to `notepad.exe`. | `DoEdit` + `ResolveEditorCommand`, `main.cpp:245` |
| Enter on a file | Follows `Config::enterFileAction` (`view`/`edit`/`execute`, default `view`). `execute` is the only route to the OS association. | `ActivateCursorEntry`; `DoExecute` uses `ShellExecuteExW "open"` |
| Enter on a `.zip` | Goes to a plugin first. | `PluginManager::TryEnterAsArchive` |
| File → "Assign File to App" | A dead stub since FEA-0005. | `MenuBar.h:97` |

There is no per-extension anything. FEA-0009 adds:
- a per-extension **action table**;
- two action types (**View**, **Open**) with a registry that is easy to extend;
- argument placeholders;
- an OS-association fallback;
- a way to pick a non-default action.

It's split into two phases. **Phase 1** satisfies every listed requirement. **Phase 2** adds in-app
management on the "Assign File to App" menu item.

## Design decisions

### D1 — Storage: a separate `file-actions.conf`, not new keys in `config.ini` (new **DEC-011**)

The action table is a *list of records* (several per extension, each with five fields). The main
config file (DEC-007) is a flat `key = value` preference list, and its machinery doesn't fit lists:
- `AppendMissingConfigKeys` and `UpdateConfigValue` (IS-0006, FEA-0005) assume one line per key;
- the IS-0006 live-reload diff assumes one line per key.

Squeezing records into it (`fileAction.txt.view.1 = …`) would be unreadable to hand-edit.

Instead: a sibling file `%APPDATA%\MyCommander\file-actions.conf`.
- It's still DEC-007's hand-rolled, dependency-free, comment-friendly text style.
- It's grouped into blocks by extension.
- It's created on first run with a fully commented sample (the CFG-001 "discoverable" precedent of
  `EnsureConfigFileExistsAt`).
- A missing or empty file means "no custom actions": the original F3/Enter behavior is unchanged
  (CFG-003).

Format, one action per line:

```ini
# MyCommander file actions (FEA-0009). Edit and save; changes apply immediately.
#
# [<patterns>]              one or more extensions (.txt) or wildcard patterns (*.tar.gz, readme*),
#                           space-separated. [*] matches every file (lowest-effort catch-all).
# <type>[*] = <description> | <program> | <arguments>
#   type      : view | open           (more types can be added later; unknown types are ignored
#                                       with a warning, never fatal)
#   *         : marks the default action of that type for these patterns
#   program   : an executable path or bare name (PATH lookup), or one of:
#                 internal:viewer   the built-in text viewer (F3's original behavior)
#                 system:           the Windows file association (ShellExecute "open")
#   arguments : placeholders  %f full path   %n file name   %b name without extension
#                             %e extension (no dot)   %d directory   %% a literal %
#               If no file placeholder (%f/%n/%b) appears, "%f" is appended automatically.
#               Quote placeholders yourself where a path may contain spaces: "%f".

[.txt .log .md .ini]
view* = Built-in viewer        | internal:viewer |
view  = Notepad++ (read-only)  | C:\Program Files\Notepad++\notepad++.exe | -ro "%f"
open* = Notepad                | notepad.exe     | "%f"

[.pdf]
open* = Windows default        | system:         |
```

- **Why `|` as the separator:** Windows forbids `|` in file names, so it can never be part of a real
  program path. Each field is trimmed.
- **Line numbers:** each action keeps its source line number. Phase 2's editor uses it to change
  exactly one line without rewriting (and losing) the user's comments.

### D2 — Resolution rules ("which action runs")

For a file name `F` and an action type `T`:

1. **Collect** every action of type `T` from every block where some pattern matches `F`.
   - An extension pattern matches `F`'s extension, case-insensitively.
   - Any other pattern goes through `PathUtil.h`'s existing `WildcardMatch` against the whole file
     name. A compound extension like `*.tar.gz` therefore works.
   - Actions keep file order.
2. **Pick the default:** the first collected action marked `*`. If none is marked, the first
   collected action.
   - "First match wins" in file order is predictable, and mirrors how users already read such files.
     A specific block placed above `[*]` naturally takes priority.
3. **Fallback when nothing is collected** (the requirement's "fall back to the OS association
   where applicable"):
   - **View** → the built-in viewer. This keeps F3 exactly as today, which is also what makes a
     missing file a no-op.
   - **Open** → `system:` (the Windows association).

Several `*` markers for the same type in one block is reported as a warning (CFG-005 style). The
first one wins.

### D3 — Keys

| Key | Action |
|---|---|
| **F3** | Default **View** action for the cursor file (falls back to the built-in viewer). |
| **Shift+Enter** | Default **Open** action (falls back to the Windows association). This is new. Today Shift+Enter just behaves like Enter, so the cost is small. With text in the command line, Shift+Enter keeps running it, like plain Enter, so nothing typed is ever lost. |
| **Shift+F3** | **"Choose action…"**: a list of *every* registered View and Open action for the cursor file, plus the two built-ins (Built-in viewer, Windows default). This is how a non-default action is reached ("multiple programs per extension"). |
| **Enter** | Unchanged: `enterFileAction` (`view`/`edit`/`execute`). The plugin's `.zip` handling keeps priority. `enterFileAction`'s `view` now means "the default View action", so a user who registers a View program gets it on Enter too. A new value **`open`** is added, meaning "the default Open action". Opting in stays explicit, as with VEE-008. |
| **F4** | Unchanged. Edit is the obvious next action type (see D5). |

**The chooser** is a new keyboard and mouse dialog: a vertical, scrollable list, Up/Down and Enter,
Esc cancels. It's built in `Dialog.cpp` as `ShowListChoice`, because `ShowChoiceDialog`'s one-line
hotkey bar doesn't scale to an unknown number of entries. It's generic, not FEA-0009-specific:
FEA-0005's other stubs (e.g. "Compare contents") will need the same shape.

### D4 — Running an action (VEE-008 / SEC-002 / SEC-004)

- **`internal:viewer`** → `ShowTextFileViewer` (DEC-003).
- **`system:`** → `ShellExecuteExW` with the verb `open`. This is `DoExecute`'s existing code,
  factored into a `RunShellAssociation(path)` helper both share.
- **Anything else** runs the program with the expanded arguments.
  - **How it runs:** through a new `RunDetachedCommandLine(exe, rawArguments, workDir)` in
    `Process.{h,cpp}`. The working directory is the file's own directory.
  - **Why a new function:** `RunDetached` quotes *each* argument itself (SEC-004). User-written
    arguments are a template the user already quoted (`-ro "%f"`), so they're passed verbatim after
    placeholder expansion.
  - **Why expansion is safe:** a Windows path can never contain `"`, so a placeholder inside the
    user's own quotes can't break out of them.
  - **Wait behavior:** detached, like F4's default since IS-0002. Viewer and opener programs are
    overwhelmingly GUI programs.
  - **Console programs:** a console-subsystem program shares this console, the same documented
    limitation as IS-0002's non-blocking launch. Per-action "wait" is an open question.
- **Explicit-action rule:** nothing ever runs without an explicit keypress (F3, Shift+Enter, the
  chooser, or Enter with an explicitly configured `enterFileAction`). Registering a program only
  changes *which* program an explicit key runs. **No elevation** is ever requested (SEC-002).
- **Failure:** shows a message naming the action's description and program plus the Win32 error,
  and is logged at Warning (ERR-004). Only the program path and the error are logged, never the
  arguments (ERR-005's no-content rule).

### D5 — "Easy to extend with more action types"

- Action types are one `enum class FileActionType { View, Open }` plus one table of
  `{type, keyword, fallback}` rows in `FileActions.cpp`.
- **Parser:** unknown type keywords produce a warning and are skipped, never fatal (CFG-004/005).
  A file written for a newer version therefore still loads.
- **Adding `edit`** would mean:
  - one enum value and one table row;
  - F4 asking `DefaultAction(Edit)` with `%EDITOR%` as its fallback.

  It's deliberately not in scope (the request names View and Open), but it's a ~10-line follow-up
  and the plan keeps it that cheap.

## Phase 1 — Action table, resolution, keys, fallbacks

### New pure module `Source/MyCommander/FileActions.{h,cpp}`

It has no `Console`/`Dialog` dependency, following the CLAUDE.md pure-module precedent.

```cpp
enum class FileActionType { View, Open };

struct FileAction {
    FileActionType type;
    bool isDefault = false;
    std::wstring description;
    std::wstring program;     // path / bare name, or "internal:viewer" / "system:"
    std::wstring arguments;   // template with %f %n %b %e %d %%
    int sourceLine = 0;       // 1-based line in the file (Phase 2 edits it in place)
};

struct FileActionBlock {
    std::vector<std::wstring> patterns;   // ".txt", "*.tar.gz", "*"
    std::vector<FileAction> actions;
    int headerLine = 0;
};

struct FileActionTable { std::vector<FileActionBlock> blocks; };

struct ParseFileActionsResult { FileActionTable table; std::vector<std::wstring> warnings; };

ParseFileActionsResult ParseFileActions(const std::wstring& text);          // pure
ParseFileActionsResult LoadFileActionsFrom(const std::filesystem::path& p); // missing file = empty, no warning
std::filesystem::path FileActionsFilePath();                                // %APPDATA%\MyCommander\file-actions.conf
void EnsureFileActionsFileExistsAt(const std::filesystem::path& p);         // commented sample, never overwrites

std::vector<FileAction> FindFileActions(const FileActionTable&, const std::wstring& fileName,
                                        FileActionType);                   // D2 step 1, file order
std::optional<FileAction> DefaultFileAction(const FileActionTable&, const std::wstring& fileName,
                                            FileActionType);               // D2 step 2 (nullopt -> D2 step 3)
std::wstring ExpandFileActionArguments(const std::wstring& argumentTemplate,
                                       const std::filesystem::path& file);  // placeholders + auto "%f"
const wchar_t* FileActionTypeKeyword(FileActionType);                      // "view" / "open"
```

**Parser rules:**
- `#`/`;` start a comment (DEC-007); blank lines are ignored.
- A line before any `[…]` header is a warning.
- A bad line has fewer than 3 `|` fields, an empty program, or an unknown type. It gives a warning
  naming the line number and content, and is skipped.
- An unknown placeholder `%x` is kept literally and warned about once.

### `Process.{h,cpp}`

Add `RunDetachedCommandLine(exe, rawArguments, workingDirectory)`.
- It's the same as `RunDetached`, but the arguments are appended verbatim after the quoted program
  path.
- `RunDetached`'s own per-argument quoting is untouched.

### `main.cpp`

- **Startup:**
  - call `EnsureFileActionsFileExistsAt` and `LoadFileActionsFrom` next to the existing Config load;
  - send warnings to the Logger;
  - show warnings once at startup like Config's CFG-005 warnings.
- **Live reload (IS-0006 parity):**
  - `ConfigWatch` already watches `%APPDATA%\MyCommander\` for writes, the same directory.
  - On a change notification, reload `file-actions.conf` as well as the config.
  - A parse failure keeps the previous table and logs.
- **`RunFileAction(action, path)`:** the D4 dispatcher.
- **`DoViewWithAction`:** F3 uses the default View action, falling back to `DoView`.
- **`DoOpenWithAction`:** Shift+Enter, with an empty command line only.
- **`DoChooseAction`:** Shift+F3, through `ShowListChoice`.
- **`ActivateCursorEntry`:**
  - `View` routes through the default View action;
  - the new `Open` value routes through `DoOpenWithAction`;
  - `TryEnterAsArchive` keeps priority, unchanged.

### `Config.{h,cpp}`

- `EnterFileAction` gains `Open` (keyword `open`).
- Its documentation block in the generated file and in `AppendMissingConfigKeys` lists the new value.

### `Dialog.{h,cpp}`

Add `ShowListChoice(console, title, items, redraw) -> int` (index, or -1).
- It's a scrollable vertical list sized by `LayOutBox`, widths through `TextWidth.h` (UI-011).
- Up/Down/PgUp/PgDn/Home/End move; Enter chooses; Esc cancels.
- A mouse click picks a row (UI-012 parity: every mouse path also has a keyboard path, KEY-001).
- The cursor/scroll arithmetic is a pure `ApplyListChoiceKey` in `DialogInput.cpp` (TST-005
  precedent).

### UI text

- `HintBar.h` File context: "Shift+F3 Actions". Shift+Enter Open is listed only in Help/README,
  because the hint bar is already crowded on narrow terminals.
- `Help.cpp` and `README.md` keybinding tables are updated together (CLAUDE.md sync rule): F3's
  description changes; Shift+F3 and Shift+Enter are added.
- `README.md` gets a short "File actions" section with the format and an example.

## Phase 2 — In-app management ("Assign File to App")

Replace the `MenuBar.h:97` stub with a real item. It opens a dialog for the **cursor file's
extension**:

1. **List** the actions registered for that extension (via `ShowListChoice`), each shown as
   `[view*] Notepad++ — C:\…\notepad++.exe -ro "%f"`. The list also offers **Add…** and **Edit
   file-actions.conf** (opens it in `%EDITOR%`, like Ctrl+F11 does for the config).
2. **Choosing an action** gives *Set as default / Remove / Cancel* (`ShowChoiceDialog`).
3. **Add…** opens a `ShowFormDialog` (FEA-0007's form) with these fields:
   - Type (Choice: view/open);
   - Description (Text);
   - Program (Text);
   - Arguments (Text, pre-filled `"%f"`);
   - Default (Checkbox).

   It validates that the program isn't empty (existence isn't checked: it may be a PATH name).

Edits are **line-level text edits** through pure functions in `FileActions.cpp`. They're unit-tested
on strings, so the user's comments and layout are never rewritten:
- `AddFileActionLine(text, extension, action)`: appends to the first block whose patterns include
  exactly `extension`, or appends a new `[.ext]` block at the end;
- `RemoveFileActionLine(text, sourceLine)`;
- `SetDefaultFileActionLine(text, sourceLine)`: adds `*` to that line and removes it from same-type
  lines of the same block.

The file is written atomically (temp file + `MoveFileExW`, the CommandHistory precedent). The
live-reload path then picks it up like a manual edit.

## Tests

New `Source/MyCommanderTests/FileActionsTests.cpp`, added to `MyCommanderTests.vcxproj` together
with `FileActions.cpp`:

- **Parse:**
  - the sample file parses with 0 warnings;
  - blocks, patterns, types, the default marker, trimmed fields and line numbers are correct;
  - comments and blank lines are skipped;
  - bad lines (missing fields, empty program, unknown type, action before any block) give one
    warning each and are skipped;
  - several `*` for one type gives a warning and the first wins;
  - a missing file gives an empty table with no warnings (CFG-003).
- **Matching:**
  - `.TXT` vs `.txt` (case-insensitive);
  - `*.tar.gz` (compound pattern);
  - `readme*` (name pattern);
  - `[*]` catch-all ordering below a specific block;
  - no extension at all;
  - a dotfile like `.gitignore`.
- **Default:**
  - the marked action wins;
  - with none marked, the first one wins;
  - per type, independently;
  - `nullopt` when nothing matches.
- **Expansion:**
  - every placeholder (`%%` → `%`);
  - auto-appended `"%f"` when no file placeholder is present;
  - not appended when `%n` is present;
  - unknown `%x` is kept;
  - Unicode and space-containing paths.
- **Phase 2 line edits:**
  - add to an existing block;
  - add to a new block;
  - remove;
  - set default (moves the `*` and leaves other types alone);
  - every comment line is preserved byte-for-byte.
- **`ApplyListChoiceKey`:** clamping, paging and wrap/no-wrap at the ends (in
  `DialogInputTests.cpp`).

`RunDetachedCommandLine` and `ShellExecuteExW` are real process side effects. They're verified by
hand, matching `RunElevated`'s untestable-OS-side-effect precedent.

## Spec, docs, version

- **`AIPrompt/my-commander.md`:**
  - **`VEE-009`:** Disposition LATER → IN-MVP, Implemented, wording kept;
  - **new rows** (Implemented once done):
    - `VEE-010`: View/Open action types, several per extension, one default per type;
    - `VEE-011`: argument placeholders;
    - `VEE-012`: OS-association fallback for Open, built-in viewer fallback for View;
    - `VEE-013`: Phase 2's in-app management;
  - **`MENU-`:** "Assign File to App" is no longer a stub;
  - **new `DEC-011`** (file-actions storage format, D1), DECIDED;
  - a version-history note.
- **`CLAUDE.md`:**
  - a new `FileActions.{h,cpp}` source-layout bullet;
  - list it among the pure-module/test precedents;
  - mention `ShowListChoice` in the `Dialog` bullet.
- **`Version.h`:** MINOR bump, 1.10 → 1.11, because this is a new feature.
- **Changelog** entry for the day.

## Risk / scope notes

- **Behavior with no `file-actions.conf` changes is identical to today.**
  - F3 falls back to the built-in viewer.
  - Enter follows `enterFileAction` as before.
  - Shift+Enter falls back to the Windows association, which is new but explicit.

  The first-run sample's only active default is the built-in viewer for text files, i.e. today's
  behavior. Everything else in it is commented out.
- **Main risk: a user-configured program that hangs or is console-based.** It's detached, so
  MyCommander stays responsive. A console program interleaving with our screen is a documented
  limitation (D4).
- **`.zip` interaction:** Enter keeps opening archives in the ZIP plugin. Shift+Enter and Shift+F3
  give access to a registered external archiver, so users get both.
- **Out of scope:**
  - multi-selection actions (`%s` = all selected paths);
  - per-action "wait for exit";
  - an Edit action type (F4);
  - importing associations from the Windows registry;
  - per-action icons or hotkeys.

  Each fits the D5 extension shape.

## Open questions

1. **Keys.** The plan uses: F3 for the default View, **Shift+Enter** for the default Open, and
   **Shift+F3** for the action chooser. Alternatives are Alt+F3 for the chooser, or a "View with…"
   and "Open with…" pair on the File menu. Are these keys OK?
2. **Enter.** Should `enterFileAction = view` (the default) also honor a registered View program
   (planned: yes), or always use the built-in viewer, so only F3 is affected?
3. **Phase 2 now or later?** Phase 1 alone meets every listed requirement, with editing done in the
   text file. Phase 2 adds the "Assign File to App" dialog. The plan includes both; Phase 2 can be
   deferred.
4. **Edit action type.** Should F4/Edit be included now, since it's cheap (D5), or left out as not
   requested?

## Status

Implemented 2026-10-05 (version 1.11.0), Phases 1 and 2. The open questions took the plan's
defaults: Shift+Enter / Shift+F3 as proposed; Enter with `enterFileAction = view` honors a registered
View program; Phase 2 included; no Edit type. Deviations from the plan:

- **Reload:** instead of hooking IS-0006's `ConfigWatch` (which only runs while Ctrl+F11's editor is
  open), `CurrentFileActions` re-stats the file before each action is resolved and reloads when its
  timestamp or size changed — a save applies on the next keypress however it was made, with no watch
  handle to manage.
- **Open via `system:`** doesn't wait for the launched application (unlike `enterFileAction = execute`'s
  `DoExecute`, which still does).
- **File encoding:** read and written as UTF-8 (ANSI fallback on invalid UTF-8), unlike
  `mycommander.ini`'s plain `wifstream`, so non-ASCII program paths round-trip.
- **Adding never edits a shared block** (found while documenting the Markdown example): the
  first-run sample's `[.txt .log .md .ini .conf]` block meant "add a default `.md` viewer" also
  changed `.txt`/`.log`/`.ini`/`.conf`. `AddFileActionLine` now appends only to a block dedicated
  to exactly that one pattern, else inserts a new `[pattern]` block just before the first shared
  block listing it (so its default wins). The manager labels shared-block actions
  `[shared with …]`, and its "Default" uses `SetDefaultFileActionForPattern`, which also unmarks a
  dedicated block's same-type default that would otherwise keep winning. Shift+F3 no longer lists
  a built-in twice when the file registers it itself.
- **Precedence changed after user testing (2026-10-05):** D2's "first `*` anywhere wins" let the
  first-run sample's active `[.txt .log .md .ini .conf] view* = Built-in viewer` block beat a
  user's own, unmarked `.txt`/`.md` blocks. That also contradicted the file's own header ("the first
  matching block wins"). Now the first matching block that has an action of the needed type decides,
  and `*` only picks within it. The sample is comments-only. `MigrateLegacyFileActionsText` removes
  exactly that generated block from an existing file, at startup and before each resolve.
  `mycommander.ini` now warns, naming `file-actions.conf`, about file-action lines typed into it
  (the user had put theirs there via Ctrl+F11). The manager's "Default" on a shared entry copies it
  into the extension's own block rather than editing the shared one.
- **Assign File to App** keys the block by the lowercased extension (or the whole name for an
  extensionless file / dotfile) and only lists/edits blocks naming exactly that pattern; actions
  inherited from wildcard blocks are shown by Shift+F3 but managed by hand in the file.
