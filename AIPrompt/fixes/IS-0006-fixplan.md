# Fix Plan — IS-0006

Source: `AIPrompt/issue_fix_requests.md`

> Issue with configuration saving mechanism.
> On softwarw start read the configuration and check all available config values are saved.
> The missing wan required to add, and saved the config immediatelly.
> Add a hoot-key, to open the config file with default text editor. Monitor the file, and when
> the user is saved/closed this update the app states/behaviour as it requred by the config
> values.

## Summary

Three related pieces, all inside `Config`/`main.cpp`:

1. **Self-healing config file.** At startup, after loading `mycommander.ini`, detect any
   currently-known setting whose key is *absent* from the file (typically: an older file,
   written before a newer setting existed) and append it — with its default value and the same
   documentation comment the fully-commented default file already carries for that key — so the
   file on disk stays complete and self-documenting, not just tolerantly defaulted in memory.
2. **A hot-key (`F11`) to open the config file** in the external editor, reusing the same
   editor-resolution/launch machinery `F4` (`DoEdit`) already has.
3. **Live reload.** While that editor session is open, watch the file the same way IS-0002
   already watches an edited panel file; once the user saves or closes it, reload `Config` from
   disk and apply every setting it drives to the already-running app — not just leave the new
   values sitting unused until the next restart.

## Part 1 — self-healing config file

### Current behavior

`Config.cpp`'s `LoadConfigFrom` already tolerates a missing key perfectly well (the in-memory
`Config` field simply keeps its default) — CFG-003's whole point. What it does *not* do is tell
the *file* about that default: an old `mycommander.ini` from before, say, UI-010's `colorTheme`
setting was added will happily keep loading forever without ever gaining a `colorTheme` line,
even though `EnsureConfigFileExistsAt` (only ever invoked when the file doesn't exist *at all*)
would have written one for a brand-new install.

### Design

Restructure the fully-commented default text `EnsureConfigFileExistsAt` currently holds as one
large string literal into a small, ordered table — one entry per known key, holding exactly the
comment-plus-`key = default`-line block that setting already gets in that literal today:

```cpp
struct ConfigKeyBlock {
    std::wstring key;   // canonical lowercase key, matching LoadConfigFrom's own comparisons
    std::wstring text;  // the comment(s) + "key = default\n" block for this setting
};
const std::vector<ConfigKeyBlock>& ConfigKeyBlocks();  // one entry per LoadConfigFrom-recognized key, in the
                                                       // file's existing documented order
```

`EnsureConfigFileExistsAt` becomes: write the fixed header comment, then join every block in
order — same output shape as today, just assembled from one source of truth instead of a second,
independently-maintained literal (no test depends on exact byte-for-byte formatting, only on
values round-tripping through `LoadConfigFrom`, confirmed by reading `ConfigTests.cpp`).

`LoadConfigResult` gains a new field:

```cpp
struct LoadConfigResult {
    Config config;
    std::vector<std::wstring> warnings;
    std::set<std::wstring> presentKeys;  // IS-0006: which recognized keys the file actually had
};
```

`LoadConfigFrom`'s existing `if (key == L"...") ... else if (...) ... else { continue; }` chain
needs exactly one small addition: a `bool recognized = true;` set to `false` only in that final
`else` (unrecognized key), and `if (recognized) result.presentKeys.insert(key);` once the chain
finishes for that line — regardless of whether the *value* parsed successfully (a key with a
typo'd value is still present, just also reported via the existing `warnings` path; that's a
different, already-handled problem from a key being absent altogether).

A new function, next to `EnsureConfigFileExistsAt`:

```cpp
// IS-0006: appends every ConfigKeyBlocks() entry not in `presentKeys` to the existing file at
// `path`, each with its own default value and documentation comment -- keeps an older config
// file self-healing/self-documenting rather than silently relying on an in-memory default the
// file itself never mentions. No-op if nothing's missing, or the file can't be opened for
// append. Returns the keys that were appended (empty if none).
std::vector<std::wstring> AppendMissingConfigKeys(const std::filesystem::path& path,
                                                   const std::set<std::wstring>& presentKeys);
```

Wired into `wmain` right after `LoadConfig()` (which, by then, has already run
`EnsureConfigFileExists()` first — so the file is guaranteed to exist, either pre-existing or
freshly written with everything by that call):

```cpp
EnsureConfigFileExists();
auto loadedConfig = LoadConfig();
Config config = loadedConfig.config;
...
Logger logger(config.logLevel);
auto addedKeys = AppendMissingConfigKeys(ConfigFilePath(), loadedConfig.presentKeys);
if (!addedKeys.empty()) {
    logger.Log(LogLevel::Info, L"Config: added " + std::to_wstring(addedKeys.size()) +
                                L" missing setting(s) to " + ConfigFilePath().wstring());
}
```

**Deliberately no popup dialog** for this (unlike CFG-005's parse-warning dialog, which *does*
interrupt startup with a message): a missing key isn't a mistake the user made, it's an entirely
normal consequence of upgrading past whatever version introduced the setting — logging it (only
visible if `logLevel` is already enabled) is proportionate; a dialog on every such upgrade would
be a nag, not a warning.

### Test plan (Part 1)

`ConfigTests.cpp` gets new cases: `LoadConfigFrom` on a file with only some keys present
correctly populates `presentKeys` (and doesn't include keys that were only *attempted*, e.g. an
unrecognized key, or excludes nothing that legitimately parsed even if its *value* triggered a
warning); `AppendMissingConfigKeys` on a file missing N keys appends exactly those N, leaves
every existing line/value byte-for-byte untouched (confirmed by re-`LoadConfigFrom`-ing and
checking values, and by checking the original lines are still present in the raw file text), and
is a no-op (returns empty, file untouched) when nothing is missing. `EnsureConfigFileExistsAt`'s
existing tests (`_CreatesFileWhenMissing`, `_DoesNotOverwriteAnExistingFile`) are re-run
unmodified to confirm the `ConfigKeyBlocks()`-based rewrite doesn't change its observable
behavior.

## Part 2 — `F11`: open the config file

`F11` is unclaimed (checked against the full key-dispatch switch) and continues this app's own
established function-key vocabulary (`F1` Help ... `F10` Quit) rather than introducing a
`Ctrl+letter` binding that would look inconsistent next to it — the next available function key
in that sequence.

```cpp
void DoEditConfig(Console& console, Panel& left, Panel& right, bool leftActive, Logger& logger,
                  ConfigWatch& watch);
```

Mirrors `DoEdit`'s editor-resolution and launch logic (`ResolveEditorCommand`, `RunDetached`,
the same `kStrCannotStartEditorTitle` failure dialog) but targets `ConfigFilePath()` instead of
the active panel's cursor file, and **always launches detached**, regardless of
`Config::waitForEditorToClose` — that setting is specifically about `F4`'s own edit-a-panel-file
trade-off (a console-subsystem `%EDITOR%` sharing this window); blocking the whole app until the
config editor closes would defeat Part 3's entire point of a *live* reload while you keep working.
`EnsureConfigFileExists()` has already run unconditionally before the main loop starts, so the
file is always guaranteed to exist by the time `F11` could ever be pressed — no on-demand
creation needed here.

Wired into `wmain`'s key-dispatch switch as `case VK_F11: DoEditConfig(...); break;`, not gated
on `commandLine.empty()` (matching every other F-key, none of which are gated that way).

**Deliberately not added to the bottom hint bar's per-context segments** (unlike `F1`/`F10`,
which appear in all seven `HintContext`s): this is a rare, power-user action, not something that
belongs alongside the panel-relevant hints shown for every keystroke. It **is** added to the
`F1` shortcut reference (`Help.cpp`'s `Sections()`, KEY-004) and README's keybindings table, so
it's still documented and discoverable, just not competing for hint-bar space. (If a hint-bar
entry is wanted after all, that's a one-line follow-up, not a redesign — flagging the choice here
rather than silently deciding it's final.)

## Part 3 — live reload on save/close

### `ConfigWatch`

A new, single (not a vector — there's only ever one config file) watch struct in `main.cpp`,
deliberately **not** reusing IS-0002's `PendingEdit`/`PollPendingEdits`: a `PendingEdit` change
means "refresh a panel's *directory listing*," entirely different from what a config-file change
means ("reload `Config` and re-apply every setting it drives to already-running state") — reusing
the same struct/poller for two unrelated reactions would need a branch inside `PollPendingEdits`
distinguishing "this one's the config file" from every real call site, which is worse than two
small, purpose-specific pieces (the same reasoning that already kept IS-0004's `SelectDragState`
separate from `DragState` despite their structural similarity).

```cpp
struct ConfigWatch {
    bool active = false;
    HANDLE editorProcess = nullptr;
    HANDLE changeNotification = nullptr;
};
```

`DoEditConfig` populates it exactly the way `DoEdit` populates a `PendingEdit`
(`FindFirstChangeNotificationW` on the config file's parent directory,
`FILE_NOTIFY_CHANGE_LAST_WRITE | FILE_NOTIFY_CHANGE_SIZE`, logged and abandoned on failure rather
than blocking the edit that already started) — if a *previous* `ConfigWatch` is still `active`
(e.g. `F11` pressed again before the first editor window closed), its handles are closed first
rather than leaked, matching `PollPendingEdits`' own per-entry cleanup.

### `PollConfigWatch`

Called once per main-loop iteration, the same ~16 ms cadence `PollPendingEdits` already uses:

```cpp
void PollConfigWatch(ConfigWatch& watch, Config& config, Console& console, Panel& left, Panel& right, Logger& logger);
```

On a signaled `changeNotification` (a save) or `editorProcess` (the editor exited — one final
reload in case a save just before exit hasn't been picked up), it re-runs `LoadConfig()` and
applies the result via a new `ApplyConfigLive` helper, described below. **Does not** re-run Part
1's missing-key backfill on this path — that is deliberately startup-only (see Scope note below).
If the reload produced any CFG-005 parse warnings, the same `ShowMessage(console, ...,
kStrConfigurationWarningTitle, ...)` dialog startup already shows for load warnings is shown
here too, so a typo made while live-editing is surfaced immediately, not just silently defaulted.

### `ApplyConfigLive`

Every `Config` field, and how it's currently *only* read once at startup (found by tracing each
field's call sites through `main.cpp`):

| Field | Where it's read | Live already? |
|---|---|---|
| `confirmRecycleBinDelete`/`confirmPermanentDelete` | `DoDelete(..., config, ...)`, by const-ref, every call | Yes — reassigning `config` is enough |
| `enterFileAction` | `ActivateCursorEntry(..., config, ...)`, by const-ref, every call | Yes |
| `persistCommandHistory` | read once at startup (load) and once at exit (save), both against the live `config` variable | Yes |
| `waitForEditorToClose` | `DoEdit(..., config, ...)`, by const-ref, every call | Yes |
| `groupDirectoriesFirst` | consumed **once**, at `Panel` construction | **No** — `Panel` already has `SetGroupDirectoriesFirst(bool)` |
| `useBasicSymbols` | consumed **once**, at `Console` construction | **No** — `Console` already has `SetUseBasicSymbols(bool)` |
| `visibleColumns`/`typeColumnWidth`/`mtimeColumnWidth`/`sizeColumnWidth` | copied **once** into `main.cpp`'s module-level `gVisibleColumns`/`gTypeColumnWidth`/`gMtimeColumnWidth`/`gSizeColumnWidth` (UI-005's documented "set once at startup" globals) | **No** — reassign the globals directly |
| `colorTheme` | drives **one** `ApplyThemePalette(...)` call at startup (UI-010) | **No** — re-call it |
| `logLevel` | constructs `Logger` **once**; `Logger` has no setter today | **No** — add `Logger::SetLevel(LogLevel)` |

```cpp
void ApplyConfigLive(const Config& newConfig, Console& console, Panel& left, Panel& right, Logger& logger) {
    console.SetUseBasicSymbols(newConfig.useBasicSymbols);
    left.SetGroupDirectoriesFirst(newConfig.groupDirectoriesFirst);
    right.SetGroupDirectoriesFirst(newConfig.groupDirectoriesFirst);
    gVisibleColumns = newConfig.visibleColumns;
    gTypeColumnWidth = newConfig.typeColumnWidth;
    gMtimeColumnWidth = newConfig.mtimeColumnWidth;
    gSizeColumnWidth = newConfig.sizeColumnWidth;
    ApplyThemePalette(newConfig.colorTheme == ColorTheme::HighContrast ? kHighContrastThemePalette
                                                                        : kDefaultThemePalette);
    logger.SetLevel(newConfig.logLevel);
}
```

`PollConfigWatch` calls this, then assigns `config = reloaded.config;` so every by-const-ref call
site (the "Yes" rows above) also sees the new values on its very next invocation.

`Log.h`'s `Logger` gains one new method:

```cpp
void SetLevel(LogLevel level) { level_ = level; }
```

### Test plan (Part 3)

`ApplyConfigLive`/`PollConfigWatch`/`DoEditConfig` themselves get no automated coverage,
consistent with `main.cpp`'s established limit (no test exercises `DoEdit`/`PollPendingEdits`
either, for the same reason). `LogTests.cpp` gets one new case for `Logger::SetLevel` (construct
`Off`, confirm nothing's written, `SetLevel(Info)`, confirm a subsequent `Log(Info, ...)` now
writes) — the one genuinely new, pure piece of behavior this part adds outside `main.cpp`.
`Panel::SetGroupDirectoriesFirst`/`Console::SetUseBasicSymbols` are pre-existing, already-used
setters, not new surface to test.

## Files touched

- `Source/MyCommander/Config.h`/`Config.cpp` — `ConfigKeyBlock`/`ConfigKeyBlocks()`,
  `LoadConfigResult::presentKeys`, `AppendMissingConfigKeys`; `EnsureConfigFileExistsAt`
  rewritten in terms of `ConfigKeyBlocks()`.
- `Source/MyCommander/Log.h` — `Logger::SetLevel`.
- `Source/MyCommander/main.cpp` — `ConfigWatch`, `DoEditConfig`, `PollConfigWatch`,
  `ApplyConfigLive`; `VK_F11` added to the key-dispatch switch; `PollConfigWatch(...)` added
  next to the existing `PollPendingEdits(...)` call in the main loop; the
  `AppendMissingConfigKeys` call added right after `Logger` is constructed; a
  `ConfigWatch configWatch;` declared in `wmain` alongside `drag`/`selectDrag`/`pendingEdits`.
- `Source/MyCommander/Help.cpp` — one new row in `Sections()`'s "Other" (or a new "Configuration"
  group) for `F11`.
- `README.md` — one new keybindings-table row for `F11`, and a short mention in the
  Configuration section that `F11` opens the file and changes apply live on save.
- `AIPrompt/my-commander.md` — version-history paragraph; no `Implemented`/Disposition change
  expected (extends CFG-001/002/003/004/005, all already Implemented, the same treatment every
  fix in this series has gotten for an already-Implemented parent requirement).
- Tests: `Source/MyCommanderTests/ConfigTests.cpp` (Part 1), `Source/MyCommanderTests/LogTests.cpp`
  (Part 3's `SetLevel`).

## Risk / scope notes

- The config-file watch (like `PendingEdit`'s own) watches the *directory*
  (`%APPDATA%\MyCommander`), not the specific file — Win32's
  `FindFirstChangeNotificationW` has no finer granularity, the same imprecision `PendingEdit`
  already accepts. That directory also holds the log file, session file, and command-history
  file, so an unrelated write there (most likely: `mycommander.log` itself, if `logLevel` is on)
  can trigger a spurious reload+re-apply. Harmless (idempotent — re-applying identical values
  costs a little CPU, nothing more) and only possible during the bounded window an `F11` session
  is actually open, so not engineered around further.
- Part 1's backfill is deliberately startup-only, not re-run on every live reload (Part 3) — so
  deleting a key from the file while editing it live doesn't get silently fought/re-added by the
  app between keystrokes. If a "backfill on every reload too" behavior is actually wanted, that's
  a one-line change (call `AppendMissingConfigKeys` from `PollConfigWatch` too) — flagged as a
  deliberate choice, not an oversight, in case it's not what's wanted.
- `F11` is a judgment call, not something the issue specified — open to changing it if a
  different binding is preferred (see Part 2's own note on hint-bar visibility too).
- No requirement ID's `Implemented`/Disposition changes; this extends CFG-001–005 (config file
  format/discoverability/tolerance), already Implemented.

## Status

Implemented, per the design above, with no deviations:

- **Part 1** (self-healing): `ConfigKeyBlocks()`, `LoadConfigResult::presentKeys`, and
  `AppendMissingConfigKeys` landed exactly as designed; `EnsureConfigFileExistsAt` now builds its
  output by joining the same table. Wired into `wmain` right after `Logger` construction, exactly
  as planned, with an Info-level log line (no popup).
- **Part 2** (`F11`): `DoEditConfig` landed exactly as designed — always detached, targeting
  `ConfigFilePath()`. Added to `Help.cpp`'s shortcut reference and README's keybindings table;
  deliberately not added to the hint bar, as scoped.
- **Part 3** (live reload): `ConfigWatch`, `PollConfigWatch`, and `ApplyConfigLive` landed exactly
  as designed, including `Logger::SetLevel` (`Log.h`) and the reused startup-warning-dialog path
  for reload-time parse warnings. The missing-key backfill is confirmed startup-only, not
  re-triggered by a live reload.
- See `3_Reports/ChangeLogs/changelog_2026-09-25.md`'s IS-0006 entry for the full file list and
  test counts.

**Post-implementation corrections (2026-09-25, reported by the user testing the shipped
feature):**

- Plain `F11` never reached the app on the user's terminal host — most hosts (Windows Terminal
  included) reserve it as a window-fullscreen toggle, the same class of host interception IS-0004
  hit with Shift-held mouse clicks. Switched to `Ctrl+F11` everywhere: `main.cpp`'s `case
  VK_F11:` now gates on `ctrlHeld`, and every doc comment/`Help.cpp` row/README mention was
  updated to match.
- Every `ConfigKeyBlocks()` comment block now ends with an explicit `# Values: ...` line (values
  and default), not just the three enum-valued settings that already spelled it out — including
  giving `mtimeColumnWidth`/`sizeColumnWidth` their own one-line addition where they previously
  had no comment of their own at all. README's sample config block was updated to match.
  Comment-only; no parsing/behavior change.
