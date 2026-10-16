# Fix Plan — IS-0009

Source: `AIPrompt/issue_fix_requests.md`

> Subject Menu:
>     Add to the File menu the "Exit" command to close the application.
> Subject Plugin:
>     In the help add a selector line in front of each plugin help text
> App:
>     This is a commandline app. I would be happy, if the current terminal title would show the
>     My Commander - <current version>

## Summary

Three independent, small asks filed together under one issue — each is architecturally separate
(no dependency between them) and written up as its own part below:

1. **File menu "Exit" item** — the File menu (`MenuBar.h`) has no item that quits the app today;
   `F10` (and `Esc`) already do, via `main.cpp`'s own key-dispatch switch. Add a `SimulatedKey`
   item that replays `F10`, the same mechanism every other "already has a key" menu item already
   uses (MENU-002).
2. **A selector line in front of each plugin's help text** (F1 screen, FEA-0002 §7) — today, a
   loaded plugin's own name (the line introducing its block in the new "Plugins" section) is
   rendered with the *exact same* style (`kAttrSection`) as any ordinary Markdown `#` heading
   *inside* that plugin's own content. Scrolling through multiple plugins' help text, there is no
   visual way to tell "a new plugin's block starts here" apart from "this is just a heading within
   the plugin I'm already reading" — this is read as the gap the issue's "selector line" is asking
   to fix. Add one new, distinctly-styled row immediately before each plugin's name/content block.
3. **Terminal window title** — `MyCommander.exe` never calls `SetConsoleTitleW`; the window/tab
   keeps whatever title the terminal host already had (empty, "Command Prompt", the previous
   working directory, etc., depending on the host). Set it once at startup to `MyCommander -
   <version>`, reusing the exact same `MC_VERSION_STR` the startup log line and `--version` already
   print, so there's a single source of truth for "what does the current version look like as
   text."

## Part 1 — File menu "Exit" command

### Current behavior

`Source/MyCommander/MenuBar.h`'s `BuildMenus()` File menu (~line 86) has eight items (`Modify
Attribs`, `Pack Files`, `Unpack Package`, `Compare contents`, `Assign File to App`, `Used Disk
Area`, `Group Rename`, `Generate Check Code`) — none of them quit the app. Quitting is reachable
only via a bare `F10` or `Esc` keypress, handled directly in `main.cpp`'s key-dispatch switch
(`case VK_ESCAPE: case VK_F10: running = false; break;`, appears twice more for the drive-picker/
help-screen's own local loops, but the main loop's own copy is what's relevant here).

### Design

Add one more item to the File menu list, using the existing `real(...)` lambda (`MenuItemKind::
SimulatedKey`) exactly like every other "this already has a key" item in the file — no new
`MenuItemKind`, no new dispatch logic:

```cpp
{L"File", L'F',
 {
     real(L"Modify Attribs", L'M', L'A', /*ctrl=*/true),
     stub(L"Pack Files", L'P'),
     stub(L"Unpack Package", L'U'),
     stub(L"Compare contents", L'C'),
     stub(L"Assign File to App", L'A'),
     stub(L"Used Disk Area", L'D'),
     stub(L"Group Rename", L'G'),
     stub(L"Generate Check Code", L'E'),
     real(L"Exit", L'X', VK_F10),
 }},
```

- **Mnemonic `X`**: every other letter a plausible "Exit" mnemonic could use (`E`) is already
  taken by "Generate Check Code" within this same menu (mnemonics are scoped per-menu, so this
  collision matters here even though it wouldn't across different menus). `X` is free within File,
  and happens to match Total Commander's own convention for this exact item (`File > Exit`,
  `Alt+X`) — a nice, unforced alignment with the app this project is modeled on, not a coincidence
  worth second-guessing.
- **Placed last**, the conventional position for a menu's own "Exit"/"Quit" item (and matches
  Total Commander's own File-menu ordering) — not alphabetical or grouped with the other items
  above it, which are a mix of real/stub and not otherwise ordered by any rule this plan should
  disturb.
- **Reuses `F10` via `SimulateKeyPress`**, not a new `running = false` call site — satisfies
  MENU-002 ("every menu item that already has an existing keyboard shortcut shall invoke that
  exact same action, not a second, independent implementation") exactly the way every other
  `real(...)` item already does. No change to `main.cpp`'s key-dispatch switch at all.

### Files touched

- `Source/MyCommander/MenuBar.h` — one new `real(...)` entry in the File menu's item list.
- `Source/MyCommanderTests/MenuBarTests.cpp` — if any existing test asserts the File menu's exact
  item count/contents, it needs updating for the new ninth item; otherwise no new test is strictly
  required (a `SimulatedKey` item's own dispatch is already covered generically by whatever
  existing test exercises `real(...)`/`MenuItemKind::SimulatedKey` item behavior, if any — this
  item adds no new *kind* of behavior to test).
- `README.md` — the menu-bar/File-menu documentation (if it enumerates items) gains "Exit".
- `AIPrompt/my-commander.md` — a version-history paragraph; `MENU-001`/`MENU-002` stay
  `Implemented` (this extends the menu's own content, not either requirement's existence).

### Test plan

Manual verification: `Alt+F` opens File, `X` (or clicking `[X]Exit`) quits the app the same way
`F10` already does. No automated coverage beyond whatever `MenuBarTests.cpp` already has for
`BuildMenus()`'s own structure (pure, Console-free, so a count/content assertion there is cheap
and worth having if the file doesn't already do this generically).

## Part 2 — A selector line in front of each plugin's help text

### Current behavior

`Source/MyCommander/Help.cpp`'s `AppendPluginRows` (~line 231, FEA-0002 Phase 2) builds, per
loaded `kMcPluginCapHelp` plugin: a blank separator row (between plugins, not before the first),
then a `DisplayRow` with `pluginKind = MarkdownLineKind::Heading` holding the plugin's own
`name`, then one row per line of that plugin's own parsed Markdown. The paint loop (~line 243)
renders *every* `MarkdownLineKind::Heading` row — whether it's this plugin-name row or an
ordinary `# Heading` line from inside the plugin's own Markdown source — identically, via
`kAttrSection`:

```cpp
case MarkdownLineKind::Heading:
    console.PutText(0, y, PadOrTrim(r.pluginText, width), kAttrSection);
    break;
```

So a plugin whose own help text happens to use a `#` heading (the Developer Guide's own worked
example does: `"# Commands"`) produces two visually identical rows in a row — the plugin's name,
then its own first heading — with nothing to tell them apart, and nothing to mark where the
*next* plugin's block begins either, beyond a single blank line (easy to miss while scrolling).

### Design

Insert one new, distinctly-styled row immediately before each plugin's name row — a full-width
rule (`'='` repeated to the screen's own width, plain ASCII, no `ACC-006` basic-symbol-mode
dependency to worry about, unlike a Unicode box-drawing character) rendered with `kAttrHeader`
(the same strong highlight this very screen's own title row at the top already uses, and
distinct from both `kAttrSection` — used for built-in categories and in-plugin headings alike —
and the plain-text `kAttrNormal` everything else on this screen uses). This is the "selector
line": a single, unambiguous row whose sole purpose is marking "a plugin's own help text starts
here," never reused for anything else on this screen.

```cpp
// DisplayRow gains one more discriminant, alongside sectionTitle/item/isPluginRow:
bool isPluginSelector = false;  // a full-width rule marking "a new plugin's help starts here"
```

`AppendPluginRows` emits it right before each plugin's existing name row:

```cpp
for (const auto& content : pluginContent) {
    if (!firstPlugin) rows.push_back(DisplayRow{});  // existing blank separator, unchanged
    firstPlugin = false;

    DisplayRow selectorRow;
    selectorRow.isPluginSelector = true;
    rows.push_back(std::move(selectorRow));

    DisplayRow nameRow;                                 // unchanged from today
    nameRow.isPluginRow = true;
    nameRow.pluginKind = MarkdownLineKind::Heading;
    nameRow.pluginText = content.name;
    rows.push_back(std::move(nameRow));
    // ... existing per-line loop, unchanged ...
}
```

Paint loop gains one more branch, checked before the existing `isPluginRow` branch (order matters
only in that both must be mutually exclusive, which they are — a `DisplayRow` is never both):

```cpp
if (r.isPluginSelector) {
    console.PutText(0, y, std::wstring(static_cast<size_t>(width), L'='), kAttrHeader);
}
```

- **Why a full rule, not just restyling the existing name row**: the issue asks for a line "in
  front of" each plugin's help text, read literally as a new row preceding it, not a restyle of a
  row that's already there. A full-width rule is also more scannable while scrolling quickly than
  a differently-colored but still single-word-width line would be — it reads as a clear break even
  out of the corner of the eye.
- **`kAttrHeader`** (an existing alias for `kAttrHeaderActive`, already used by this very screen's
  own title bar — see `ShowShortcutReference`'s `paint()`, the very first `PutText` call) is
  chosen specifically because it's not used *anywhere else* in this function's own body content,
  so this row can never be confused with a built-in section title (`kAttrSection`), a shortcut key
  column (`kAttrKey`), or plain text (`kAttrNormal`).
- **No change to `MarkdownLineKind`** or `MarkdownRenderer.h`/`.cpp` — the selector line is a
  `Help.cpp`-local, plugin-boundary marker, not a Markdown construct a plugin author writes into
  their own help text; it's inserted by the host, once per plugin, regardless of what that
  plugin's Markdown actually contains.
- **Only appears when at least one plugin is loaded with `kMcPluginCapHelp`** (the surrounding
  `AppendPluginRows` early-return for an empty `pluginContent` is unchanged) — the screen still
  looks exactly as it did before FEA-0002 Phase 2 whenever no such plugin exists, same as today.

### Files touched

- `Source/MyCommander/Help.cpp` — `DisplayRow`'s new `isPluginSelector` field, `AppendPluginRows`'
  new row emission, `paint()`'s new rendering branch.
- `Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md` — the "Help content" section's worked example
  already shows what a plugin's own Markdown looks like; a one-line mention that the host adds its
  own separator row above each plugin's block (so a plugin author isn't surprised to see an extra
  `====` line they didn't write) keeps the guide accurate.
- `AIPrompt/my-commander.md` — a version-history paragraph; `PLUG-011` stays `Implemented` (this
  refines its own rendering, not its existence).

### Test plan

`AppendPluginRows`/`DisplayRow` are `Help.cpp`-local (anonymous namespace), not in a separately
testable pure module — consistent with `BuildRows`/`Sections` never having had direct unit
coverage either (the whole file's rendering has always been exercised manually, per every prior
`KEY-004`-related fix plan's own test-plan section). Manual verification once implemented: load
two plugins that both declare `kMcPluginCapHelp` (the existing `ExamplePlugin` plus a second,
throwaway test plugin, or temporarily duplicate `ExamplePlugin`'s own export under a different
`id`), press `F1`, and confirm a `====`-style rule appears immediately before *each* plugin's own
name, visually distinct from any `#`-heading inside that plugin's own Markdown content.

## Part 3 — Terminal window title

### Current behavior

Nothing in the codebase calls `SetConsoleTitleW` (confirmed via a full-repo search) — the
console/terminal keeps whatever title it already had before launching `MyCommander.exe`, which
varies by host (Windows Terminal, legacy `conhost`, an IDE's integrated terminal, …) and is
never something this app controls today.

### Design

Add a thin wrapper to `Console` (keeping the one Win32-call-isolation rule `CLAUDE.md` already
states — "`Console` class is the model — don't scatter direct Win32 calls through `Panel` or
`main.cpp`"):

```cpp
// Console.h
void SetTitle(const std::wstring& title);

// Console.cpp
void Console::SetTitle(const std::wstring& title) {
    SetConsoleTitleW(title.c_str());
}
```

Called once in `wmain`, immediately after `Console console;` is constructed (`main.cpp`, ~line
2533-2534, right alongside the existing `console.SetUseBasicSymbols(config.useBasicSymbols);`
call), reusing the exact same version-string construction the startup log line two lines above it
already uses — one source of truth for "MyCommander" + the version string's exact formatting,
not a second, independently-worded copy:

```cpp
Console console;
{
    wchar_t title[64];
    swprintf_s(title, L"MyCommander - %S", MC_VERSION_STR);
    console.SetTitle(title);
}
console.SetUseBasicSymbols(config.useBasicSymbols);
```

- **`MC_VERSION_STR`** (`Version.h`) is the same macro the startup log line (`"MyCommander %S
  starting"`) and `--version` (`"MyCommander %S\n"`) already print — it expands to the full
  four-part `MAJOR.MINOR.PATCH.BUILD` string (e.g. `1.5.0.75`), so the title reads e.g.
  `MyCommander - 1.5.0.75`. Deliberately not a trimmed `MAJOR.MINOR.PATCH`-only string: inventing
  a second version-formatting convention purely for the title bar would be new, unrequested scope,
  and the existing one is already exactly what every other place in this app that prints a version
  string uses.
- **No config toggle.** The issue reads as a plain ask ("I would be happy if..."), not a
  conditional one — and a title bar with no information in it (today's behavior) has no
  established user expectation to preserve, unlike e.g. `confirmRecycleBinDelete`'s own
  default-on-but-overridable convention for a *destructive* behavior. If a future request asks to
  make this configurable or to restore the terminal's own original title on exit, that's new scope
  for its own issue, not assumed here.
- **Set once at startup, never updated afterward** — the issue doesn't ask for the title to track
  the active panel's path or any other live state (unlike, say, a shell prompt), just to identify
  the running app and its version. No live-update plumbing, no interaction with `Theme.h`'s
  `RegisterThemeChangeListener`-style live-update pattern.
- **`SetConsoleTitleW` has no documented failure mode worth handling** (unlike every other Win32
  call this codebase wraps with an error path) — a title that silently fails to change (e.g. some
  unusual hosting environment that ignores it) degrades to today's exact behavior, not a crash or
  a blocked startup; no return-value check is added, matching e.g. how `Console`'s own
  `WriteConsoleOutputW` calls elsewhere already don't individually branch on failure either.

### Files touched

- `Source/MyCommander/Console.h`/`Console.cpp` — new `SetTitle` method.
- `Source/MyCommander/main.cpp` — one new call site in `wmain`, right after `Console console;`.
- `README.md` — a one-line mention under wherever startup behavior is already documented (if
  anywhere), noting the window/tab title now shows `MyCommander - <version>`.
- `AIPrompt/my-commander.md` — this is new, small, user-facing behavior with no existing
  requirement ID to attach to; add one short line to the version-history paragraph. Whether it
  warrants its own new requirement ID (likely under `UI-` or `ARC-`) or stays an undocumented
  cosmetic nicety is a judgment call left to whoever implements this — flagged here rather than
  decided, since this plan's job is the fix design, not spec bookkeeping policy.

### Test plan

`Console::SetTitle` is a one-line wrapper around a Win32 call with no return value to assert on
and no observable side effect `MyCommanderTests.exe` could check headlessly (the test binary has
no interactive console title to inspect, and even if it did, this is exactly the kind of
real-OS-side-effect code this codebase already doesn't unit-test, per `Elevation.h`'s own
`RunElevated` precedent). Manual verification: launch `MyCommander.exe` from a real terminal and
confirm the window/tab title reads `MyCommander - <version>` matching whatever `--version` prints
for that same build.

## Files touched (implementation phase, not yet made — combined across all three parts)

- `Source/MyCommander/MenuBar.h` — Part 1.
- `Source/MyCommanderTests/MenuBarTests.cpp` — Part 1, if applicable.
- `Source/MyCommander/Help.cpp` — Part 2.
- `Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md` — Part 2.
- `Source/MyCommander/Console.h`/`Console.cpp` — Part 3.
- `Source/MyCommander/main.cpp` — Part 3.
- `README.md` — Parts 1 and 3.
- `AIPrompt/my-commander.md` — all three parts (version-history paragraph(s); no existing
  requirement's `Disposition`/`Implemented` field is expected to change, since all three extend
  already-`Implemented` requirements/features rather than completing a still-open one — Part 3
  being the one exception, which may or may not get a brand-new ID per the note in its own Files
  touched section above).

## Risk / scope notes

- All three parts are small, additive, and independent — safe to implement and verify in any
  order, or by three separate changes, without any part depending on another.
- **Part 1** carries essentially no risk: it's the same `real(...)`/`SimulateKeyPress` pattern
  already used by a dozen other menu items, reusing `F10`'s already-correct, already-tested quit
  behavior rather than introducing a second way to quit.
- **Part 2** is scoped to `Help.cpp`'s own plugin-rendering code only — no ABI change, no
  `PluginManager`/SDK header change, no effect on a plugin author's own Markdown source or how
  it's parsed (`MarkdownRenderer.h` is untouched). A plugin author sees an extra row they didn't
  write appear above their own content; this plan's Developer Guide update exists specifically so
  that isn't a surprise.
- **Part 3** is the only part touching `Console`, a class every other feature in this app depends
  on — but the change is additive (one new method, one new call site) and does not alter
  `Console`'s existing buffer/input/mode-handling behavior in any way; risk is effectively
  confined to "does `SetConsoleTitleW` do anything unexpected in some hosting environment," which
  has no failure mode worse than "the title doesn't change" (see Part 3's own design notes above).
- No part of this plan touches `Theme.h`, `PluginManager.{h,cpp}`, the plugin ABI, `FileOps`, or
  any destructive-operation code path — the lowest-risk category of change this project makes.

## Status

**Implemented** (2026-10-01), per the design above, with no deviations:

- **Part 1 (File menu Exit):** landed exactly as designed — `real(L"Exit", L'X', VK_F10)` appended
  as the File menu's ninth item. `MenuBarTests.cpp`'s `BuildMenus_FileMenu_HasEveryRequestedItem`
  updated for the new item; a new `BuildMenus_FileMenu_ExitIsSimulatedKeyF10` case added asserting
  its kind/key.
- **Part 2 (plugin selector line):** landed exactly as designed — `DisplayRow::isPluginSelector`,
  `AppendPluginRows`' new row emission, and `paint()`'s new `kAttrHeader`-styled full-width `'='`
  rule, checked first in the row-rendering `if`/`else if` chain. Developer Guide's "Help content"
  section updated to mention the host-inserted rule.
- **Part 3 (terminal title):** landed exactly as designed — `Console::SetTitle` wraps
  `SetConsoleTitleW`; `wmain` calls it once right after `Console console;` is constructed, reusing
  `MC_VERSION_STR` the same way the startup log line does. New `UI-015` added to the spec
  (`IN-MVP`/Implemented) since this behavior had no existing requirement to extend.
- Verified via `MyCommander.sln` Debug|x64 and Release|x64 builds (0 warnings/errors, including the
  nested Release installer-payload rebuild) and `MyCommanderTests.exe` on both configurations (414
  test cases, 11,762 checks, 0 failed, up from 413/11,756 — the 1 new `MenuBarTests.cpp` case).
- As anticipated in Part 3's own test-plan note, this environment's console app has no traditional
  HWND (confirmed again here: `tasklist /v`'s own "Window Title" column reports a generic
  `OleMainThreadWndName`, not the console's own title, when run under this session's terminal
  host) — `SetConsoleTitleW`'s actual visual effect could not be screenshotted/confirmed
  automatically; the user was asked to verify it in a real terminal window/tab. Part 2's full F1
  screen render is likewise unverified pixel-for-pixel (same standing limitation every `Help.cpp`
  fix plan this session has noted), though the change is a small, mechanically verified addition to
  an already-tested rendering branch.

See `3_Reports/ChangeLogs/changelog_2026-10-01.md`'s IS-0009 entry for the full file list.
