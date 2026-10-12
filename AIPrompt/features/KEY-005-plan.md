# Implementation Plan — KEY-005 (Shortcut rebinding through configuration)

Source: `AIPrompt/my-commander.md` §15:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| KEY-005 | Shortcut rebinding may be supported later through configuration. | P2 | LATER | Not Started |

Neighbours that constrain the design:

- KEY-001: everything is keyboard-reachable;
- KEY-002/003: commander-style defaults stay the defaults;
- KEY-004: the F1 reference must show the real keys;
- KEY-006: invalid combinations fail safely with feedback;
- DEC-007: a hand-rolled `key = value` text format, no third-party parser;
- PLUG-*: plugin shortcuts must not shadow built-in ones.

§29 lists it as "Shortcut customization — a stable default key map is enough for MVP".

## Implementation check (2026-10-08)

Nothing is rebindable. Keys are hard-wired in four places that must agree:

| Where | What | Consequence for rebinding |
|---|---|---|
| `main.cpp` `wmain`, the ~600-line key switch (~line 4610–5010) | `switch (wVirtualKeyCode)` with `ctrlHeld`/`shiftHeld`/`altHeld` checks inside each case. Context rules: Backspace/Delete/Enter/Left/Right/Home/End/Esc mean command-line editing when it has text; tree/plugin panels take keys first; the menu takes keys while open | The switch is the single source of behavior. Rewriting it is the riskiest part, so the plan **doesn't rewrite it** (D3) |
| `MenuBar.h` `SimulatedKey` items, `HintBar.h` segments (`HintAction{vk, ctrl, shift, alt}`) | A menu choice or hint click **injects the default key press** (`SimulateKeyPress`, IS-0003) | If Copy moved off F5, a menu/hint that injects F5 would run whatever F5 is now. Injected keys must bypass the remapping |
| `Help.cpp` | Hand-written `{key text, description}` rows | Must show the current key for rebindable commands |
| `PluginManagerLogic.cpp` `BuiltinShortcutInventory` | Static list of built-in chords that plugins may not take | Must follow the active bindings |

Reusable pieces:

- `Config`'s file watcher (`ConfigWatch`/`PollConfigWatch`) reloads `mycommander.ini` live on save;
- `FileActions`/`Favorites` show the pattern for a separate UTF-8 `%APPDATA%\MyCommander\*.conf` file with
  warnings for bad lines (`Utf8TextFile.h`);
- `Ctrl+F11` opens the config in the editor.

## Summary

A new file **`%APPDATA%\MyCommander\keys.conf`** rebinds the main screen's **commands**:

```
# command = key[, key...]      ("none" unbinds; commands not listed keep their default)
copy        = F5, Ctrl+C
move        = F6
find-files  = Alt+F7, Ctrl+Shift+F
quit        = F10
```

It takes effect on save, with no restart. The F1 reference, the bottom hint bar and the plugin-conflict check
all show and use the **current** keys. The menu bar and hint clicks keep working whatever the keys are.

Rebinding covers **commands**: function keys, Ctrl/Alt/Shift combinations, Insert and numpad selection. These
stay **fixed**:

- cursor movement;
- command-line editing (typing, Backspace/Delete/Enter/Left/Right/Home/End/Esc while it has text);
- Tab;
- the menu's `Alt+letter` keys;
- the inside of dialogs, the viewer, the tree, the search/compare/sync lists and plugin panels.

## Design

### D1 — Commands and chords (a new pure module `KeyMap.{h,cpp}`)

```cpp
struct KeyChord { WORD vk = 0; bool ctrl = false, shift = false, alt = false; };   // == / hashing
std::optional<KeyChord> ParseKeyChord(std::wstring_view);   // "Ctrl+Shift+F2", "Alt+Enter", "Num+", "Del", "F5"
std::wstring FormatKeyChord(const KeyChord&);               // canonical spelling, used by help/hints/messages

enum class CommandId { Help, Rename, Copy, CopyAs, QueueCopy, Move, QueueMove, ... };   // ~50
struct CommandInfo { CommandId id; const wchar_t* name;          // config name, e.g. L"copy"
                     const wchar_t* label;                       // e.g. L"Copy" (help/hints)
                     std::vector<KeyChord> defaults; };          // today's keys, e.g. delete = F8, Del
const std::vector<CommandInfo>& Commands();                      // the one table

class KeyMap {
public:
    static KeyMap Defaults();
    // Applies keys.conf text over the defaults; problems go to `warnings` (never throws, never empty map).
    static KeyMap Load(std::wstring_view text, std::vector<std::wstring>& warnings);
    std::optional<CommandId> CommandFor(const KeyChord&) const;   // the user's chord -> command
    const std::vector<KeyChord>& KeysFor(CommandId) const;        // current chords (may be empty)
    bool IsFreed(const KeyChord&) const;   // a default chord whose command was moved away and nothing took it
};
std::wstring DefaultKeysFileText();      // fully-commented file listing every command with its default
```

- **Command names** are stable, lower-case and hyphenated (`copy`, `queue-copy`, `select-all`, `find-files`,
  `compare-folders`, …). The table is the only place that lists them.
- **Multiple keys per command** are allowed; `delete` defaults to `F8, Del`.
- **The fixed keys** live in one `IsFixedChord(chord)` predicate in `KeyMap.cpp`, so the reserved set is
  testable.

### D2 — `keys.conf` rules and validation (KEY-006)

The file is read with `Utf8TextFile.h`. `#` and `;` start comments.

- **A command that appears replaces all of its default keys;** `= none` unbinds it.
- **Rejected with a warning,** keeping that command's previous binding:
  - an unknown command name;
  - an unparseable key;
  - a **fixed** key (plain arrows, Enter, Esc, Tab, Backspace);
  - a **printable key** with no Ctrl or Alt (plain letters, digits and punctuation, also with Shift). These would
    steal typing from the command line;
  - **`Alt+F/S/C/N/V/E`**: these open the menus.
- **A key claimed by two lines:** the first line keeps it and the second gets a warning.
- **A key the user binds that is another command's default:** the user's binding wins, the other command loses
  just that key, and a note is shown, so moving a key never silently creates two meanings.
- **`quit` can't be left with no key.** If it is, `F10` is restored, with a warning, so the app can always be
  closed by keyboard (KEY-001).
- **Feedback:** after loading, or after a reload on save, the warnings are shown once in a message box and logged
  (ERR-004). A file with errors still applies every valid line.

### D3 — Dispatch without rewriting the switch: a translation layer

The existing switch keeps working on **default** keys. A small step at the top of the main loop translates the
pressed chord before the switch sees it:

1. **Injected events pass through unchanged.** `SimulateKeyPress` (menu items, hint clicks) pushes the chord it
   injects onto a small FIFO. When the main loop reads a key-down that matches the FIFO's front, it pops it and
   takes the event as already canonical.
2. **A chord the user bound** → `CommandFor(chord)` → rewrite the event's key and modifiers to that command's
   **first default chord**. The switch then runs the right command.
3. **A freed default chord** (`F5` after `copy = Ctrl+C`, with nothing else on F5) → treated as unbound. Plugin
   shortcuts still get it (`DispatchShortcut`), otherwise nothing happens. It is never the old command.
4. **Anything else** (fixed keys, typing, plugin keys) → unchanged.

The translation runs **after** the menu-open and plugin/tree-content routing that already come first, and only
for the main panel context. Inside a plugin panel, tree or dialog, keys are never remapped.

**Why translate rather than rewrite:** the switch's context rules (command line has text, tree on the other side,
plugin panel active, …) stay exactly as tested today. The change is about 40 lines instead of 600, and a missing
`keys.conf` is byte-for-byte today's behavior.

### D4 — Everything that shows a key follows the map

- **The F1 help (`Help.cpp`):** rows for rebindable commands carry a `CommandId`, and their key column is built
  from `KeysFor(id)` (e.g. `F5, Ctrl+C`, or `(unbound)`). Fixed-key rows stay hand-written. README's table
  documents the **defaults** and says they can be changed.
- **The hint bar (`HintBar.h`):** segments reference a `CommandId` and take their key text from the map
  (`F5 Copy` becomes `Ctrl+C Copy`). An unbound command's segment is dropped. A click injects the command's
  canonical chord via the bypass FIFO. `LayOutHintBar`'s width logic and the IS-0007 tests at 60 columns are
  re-run with the default map.
- **Menu items (`MenuBar.h`)** keep `SimulatedKey` with the **default** chord, which the FIFO makes safe. They
  don't display key text, so there is no change in what they show.
- **Plugins:** `BuiltinShortcutInventory()` becomes the fixed keys plus every chord bound in the **active**
  map. If a reload binds a key a plugin had already registered, the user's binding wins and the plugin
  shortcut is shadowed, with a warning naming the plugin.

### D5 — Loading, editing, reloading

- **At startup:** `keys.conf` is read after `Config` and before plugins load, so plugin conflicts are checked
  against the final map. A missing file means the defaults; it is **not** auto-created, because the defaults are
  self-describing in F1.
- **Settings → "Edit key bindings"** opens `keys.conf` in the configured editor, the same as `Ctrl+F11`. If the
  file doesn't exist yet, it is first created from `DefaultKeysFileText()`: every command, its default commented
  out, and the rules above.
- **Reload on save** uses the same watcher pattern as `ConfigWatch` (timestamp/size). The new map, warnings and
  plugin conflict check are applied at the next loop iteration.
- **`--version`** and the rest of startup are unaffected; a broken `keys.conf` can never stop the app.

## Command list (defaults = today's keys)

| Name | Default | Name | Default |
|---|---|---|---|
| `help` | F1 | `quit` | F10 |
| `rename` | F2 | `group-rename` | Ctrl+M |
| `drive-left` / `drive-right` | Alt+F1 / Alt+F2 | `compare-folders` / `sync-folders` | Shift+F2 / Ctrl+Shift+F2 |
| `view` / `choose-action` | F3 / Shift+F3 | `edit` | F4 |
| `sort-name/-ext/-size/-time` | Ctrl+F3 / F4 / F5 / F6 | `open` | Shift+Enter |
| `copy` / `copy-as` / `queue-copy` | F5 / Shift+F5 / Alt+F5 | `move` / `queue-move` | F6 / Alt+F6 |
| `mkdir` / `new-file` | F7 / Shift+F7 | `find-files` | Alt+F7 |
| `delete` / `delete-permanent` | F8, Del / Shift+F8, Shift+Del | `queue-delete` / `queue-delete-permanent` | Alt+F8 / Alt+Shift+F8 |
| `shell` | F9 | `edit-settings` | Ctrl+F11 |
| `filter` / `toggle-hidden` | Ctrl+F / Ctrl+H | `refresh` / `tree` | Ctrl+R / Ctrl+T |
| `attributes` / `properties` | Ctrl+A / Alt+Enter | `hotlist` / `job-queue` | Ctrl+D / Alt+Q |
| `toggle-select` | Insert | `select-mask` / `deselect-mask` | Num+ / Num- |
| `select-all` / `deselect-all` / `invert-selection` | Ctrl+Num+ / Ctrl+Num- / Num* | `select-down` / `select-up` | Shift+Down / Shift+Up |
| `history-back` / `history-forward` | Alt+Left / Alt+Right | `insert-name` / `insert-path` | Ctrl+Enter / Ctrl+Shift+Enter |
| `command-history-up` / `-down` | Ctrl+Up / Ctrl+Down | | |

The final list is taken from the switch while implementing. A test asserts that every chord the switch handles
for a command is that command's default.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/KeyMap.{h,cpp}` (new, pure) | `KeyChord` parse/format, the command table, `KeyMap::Defaults/Load/CommandFor/KeysFor/IsFreed`, `IsFixedChord`, `DefaultKeysFileText` |
| `Source/MyCommander/main.cpp` | Load/watch `keys.conf`; the translation step and the `SimulateKeyPress` bypass FIFO; warnings; Settings → Edit key bindings |
| `Source/MyCommander/HintBar.h`, `Help.{h,cpp}`, `MenuBar.h`, `PluginManagerLogic.cpp`, `PluginManager.cpp` | Key text from the map; the inventory from the map; the reload conflict warning |
| `Source/MyCommander/Strings.h` | Texts |
| `Source/MyCommanderTests/KeyMapTests.cpp` (new), `HintBarTests.cpp`, `MenuBarTests.cpp`, `PluginManagerTests.cpp` | Tests below |
| Both `.vcxproj` (+ filters) | New files |
| `README.md`, `USERGUIDE.md`, `CLAUDE.md`, `AIPrompt/my-commander.md`, `Version.h`, changelog | A "Changing keyboard shortcuts" section; KEY-005 LATER → IN-MVP / Implemented; §29 note; history; 1.21.1 → 1.22.0 |

## Tests

**`KeyMapTests.cpp`** (pure):

- **Parse/format round trip** for every default chord. Accepts `ctrl+shift+f2`, `Ctrl + F5`, `Num+`, `Del`/`Delete`,
  `Enter`/`Return`; rejects `Ctrl+`, `Foo+F5`, `F25`.
- **Defaults:**
  - every command has at least one default except where intended;
  - no chord is the default of two commands;
  - no default is a fixed key;
  - the defaults equal today's `BuiltinShortcutInventory` command chords (guards against drift).
- **Load:**
  - an override replaces the defaults;
  - `none` unbinds;
  - multiple keys;
  - an unlisted command keeps its default;
  - a freed default is `IsFreed` and `CommandFor` returns nothing for it.
- **Validation, each with a warning and the previous binding kept:**
  - an unknown command;
  - a bad key;
  - a fixed key;
  - plain or Shift+ printable keys;
  - `Alt+F/S/C/N/V/E`;
  - a duplicate claim (the first line wins);
  - taking another command's default (the user wins, the other loses that key, a note);
  - `quit = none` (F10 restored).
- **`DefaultKeysFileText()`** parses back to exactly the defaults, with no warnings.

**`HintBarTests.cpp`:**

- labels follow the map;
- an unbound command's segment is dropped;
- the IS-0007 60-column layout still places every default segment.

**`PluginManagerTests.cpp`:** the inventory contains a user-bound chord and no longer contains a freed default.

**Manual:**

1. `copy = Ctrl+C`: F5 does nothing (or reaches a plugin), Ctrl+C copies, F1 and the hint bar show `Ctrl+C`,
   and File/Commands menu items still work.
2. Swap `copy = F6` / `move = F5`: each key does the other's job, and the menu items still run the right command.
3. A `keys.conf` with errors: one message lists them, the valid lines apply, and the app is never stuck without
   `quit`.
4. Edit and save while running: it takes effect without a restart.
5. Typing in the command line is unaffected by any binding.

## Phasing

1. **Phase 1:** `KeyMap` (chords, command table, load and validation, default file text), with tests. No
   behavior change.
2. **Phase 2:** the translation step and the injected-key bypass; loading and watching `keys.conf`; warnings;
   Settings → Edit key bindings.
3. **Phase 3:**
   - F1 help, hint bar and plugin inventory from the map;
   - docs, spec, version, changelog.

## Risks and notes

- **Injected-event matching:** the FIFO matches by chord, in order. If the user presses the very same chord at
  the same instant as a menu click injects it, both are canonical, which is harmless (the same command). The FIFO
  is cleared if it ever grows stale (more than about 1 s old), so a lost injection can't swallow a later real
  keypress.
- **Freed keys are silent.** That is deliberate, so a moved command's old key can't do something surprising. The
  F1 help shows the new key.
- **Keyboard layouts:** chords are virtual keys, like today. `Ctrl+Num+` is the numpad key; a Hungarian
  `Shift+3` `+` isn't a chord (typing goes to the command line, as now).
- **Out of scope:**
  - rebinding inside dialogs, the viewer, the tree, the search/compare/sync lists and plugin panels;
  - key sequences or chords with two non-modifier keys;
  - an in-app key-binding editor (the file plus F1 is enough for P2);
  - per-panel bindings.

## Open questions

1. **Scope:** main-screen commands only, with cursor movement and command-line editing fixed (planned)? Or also
   cursor keys and the viewer/dialog keys?
2. **Where:** a separate `keys.conf` (planned: keeps `mycommander.ini` short; same pattern as `favorites.conf` /
   `file-actions.conf`), or a `[keys]` section in `mycommander.ini`?
3. **A freed default key:** silent (planned), or a status-line hint like "F5 is not bound — Copy is now Ctrl+C"?
4. **The menu bar:** show each item's current key next to its label, like Windows menus? (Planned: no; the F1
   help shows them.)

## Status

Implemented 2026-10-08, all three phases (version 1.22.0).

User decisions on the open questions:

1. **Scope:** the main screen, panels and plugins; standard dialogs are not user-settable.
   - Tree and plugin panels receive the same translated keys. Inside them a freed default key is passed through
     (not dropped), so e.g. the tree's `Num+` expand keeps working.
   - Plugin shortcuts are rebindable as `plugin:<id>/<commandId>`. The host applies the user's key at
     `RegisterShortcut` and re-applies it on reload. There is no SDK change; `DEVELOPER_GUIDE.md` documents it.
   - The viewer and the search/compare/sync lists stay fixed with the dialogs.
2. **A separate `keys.conf`.**
3. **Freed default keys are silent.**
4. **Menus show each item's current key,** right-aligned (`MenuItem::keyText`, filled from the map each loop).

Changes made while implementing:

- **A command's own default keys are never rewritten.** `Delete` stays `Delete`, not `F8`, because the switch
  already handles every default.
- **`Ctrl+Alt`+printable keys are rejected,** like plain printable keys, because they are AltGr characters on many
  layouts.
- **Command-line editing keys are exempt from translation while the command line has text:** Backspace, Delete,
  Home, End, Esc, Enter, and Left/Right without Alt.
- **Plugin shortcuts are dispatched before the main switch,** not from its `default:` branch. Some cases ignore
  modifiers (`Shift+F9` opens the shell), so a plugin bound to such a key in `keys.conf` would otherwise never fire.
- **`BuiltinShortcutInventory()` now also lists every command default.** Some, like `Shift+F3`, `Shift+Enter` and
  `Alt+Left`, were missing before. `BuiltinShortcutInventory(KeyMap)` follows the user's map.
- **Settings → "Edit key bindings"** (`K`) is a new `MenuItemKind::EditKeyBindings`. It creates the file from
  `DefaultKeysFileText()` on first use. Reloading is a timestamp/size poll at most twice a second, so no watch
  handle is needed.
- **The too-small-terminal screen** also honours a rebound `quit` key.

Tests: `KeyMapTests.cpp` (17 cases). 721 cases, 0 failed (Debug).
