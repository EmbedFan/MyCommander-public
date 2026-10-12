# Implementation Plan — EXT-003 (Public plug-in API: viewers, archive handlers, file-system providers, commands)

Source: `AIPrompt/my-commander.md` §21:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| EXT-003 | A public plug-in API may later support viewers, archive handlers, file-system providers, and commands (FEA-0002/MC Plugin, Section 26; FEA-0007/ZIP Plugin, Section 27, below). | P3 | IN-MVP | Partial |

It was promoted to IN-MVP / Partial with FEA-0002 Phase 1 (1.3.0). Related requirements:

- EXT-002: a *common* provider interface, still LATER;
- PLUG-001…013: the plugin architecture;
- VEE-009…013: file actions for F3 / Shift+F3 (FEA-0009);
- KEY-005: rebindable keys.

## Implementation check (2026-10-08)

| EXT-003 names | Status | Evidence |
|---|---|---|
| **Commands** | ✅ Done | `AddMenuItem`/`RemoveMenuItem`/`SetMenuItemEnabled` (PLUG-004), `RegisterShortcut` (PLUG-008, user-rebindable through `keys.conf` since KEY-005), `OnMenuAction`/`OnShortcut`, `SimulateKey`. Used by ExamplePlugin, ZIP and FTP |
| **Archive handlers** | ✅ Done | `kMcPluginCapArchiveHandler` (`McPlugin_CanHandleFile` / `McPlugin_EnterArchive`). Enter on a claimed file hands the panel to the plugin. Used by ZIP (ARC-001…003) |
| **File-system providers** | ✅ Done (through panel content) | `kMcPluginCapPanel` (`RequestPanelContent`, `OnPanelDraw`/`Key`/`Click`/`Mouse`, PLUG-012), `McPlugin_OnReceiveFiles` (F5 from the other panel), `GetPanelPath`/`RefreshPanel`, progress, `PollCancel`. The FTP plugin (EXT-005) is a complete remote file-system provider built on exactly this, and ZIP is a virtual one. What is **not** there, deliberately, is a *common* provider interface that would let the host's own F5/F6/Alt+F7/Shift+F2 work generically across providers. That is **EXT-002** (LATER), not EXT-003 |
| **Viewers** | ❌ Missing | A plugin can only *call* the host's text viewer (`ShowFileViewer`, API 1.5). It cannot *be* a viewer: there is no capability for F3 / Shift+F3 to hand a file to a plugin, and no way for a plugin to present a file differently (hex, formatted, decoded, …) |

**So EXT-003's one real gap is viewer plugins.** This plan adds them, then marks EXT-003 Implemented. EXT-002
stays LATER and separate.

## Summary

A new capability, **`kMcPluginCapViewer`**, lets a plugin **render a file as text lines** that the host shows in
its own built-in viewer (VEE-003), with scrolling, search, `n`, hex mode and Esc all for free.

- **Claims:** the plugin says which files it can view, and whether it wants to be the **F3 default** for them or
  only **listed in Shift+F3**.
- **Order:** the user's own `file-actions.conf` choices always win, and users can pick a plugin viewer explicitly
  as `plugin:<plugin-id>` there.
- **Example:** the ExamplePlugin gains a **hex dump** viewer, offered in Shift+F3 for every file, as a working
  sample for plugin authors.

## Design

### D1 — SDK (API 1.9 → 1.10, additive)

```c
// McPluginAbi.h
kMcPluginCapViewer = 1 << 6,   // renders files as text for the host's viewer

typedef enum McViewClaim { kMcViewNo = 0, kMcViewOffer = 1, kMcViewDefault = 2 } McViewClaim;

// Cheap, name-only (like CanHandleFile): may this plugin view the file?
// kMcViewOffer = list it in Shift+F3; kMcViewDefault = also make it F3's default.
typedef McViewClaim (__stdcall* McPlugin_CanViewFileFn)(const wchar_t* fileName);

// The plugin's viewer name for Shift+F3, e.g. L"Hex dump".
typedef const wchar_t* (__stdcall* McPlugin_GetViewerNameFn)(void);

// Renders `filePath` by calling writer->AppendLine once per line (and optionally
// SetTitle). Returns true when it produced a view; false = declined/failed (the
// host then falls back to the built-in viewer and says so). May call PollCancel.
typedef bool (__stdcall* McPlugin_RenderViewFn)(const wchar_t* filePath, const McViewWriter* writer);

// McPluginTypes.h
typedef struct McViewWriter {
    uint32_t structSize;
    void* context;                                                    // host-owned, opaque
    void (__stdcall* AppendLine)(void* context, const wchar_t* text); // one line, no newline
    void (__stdcall* SetTitle)(void* context, const wchar_t* title);  // viewer title; default = file path
} McViewWriter;
```

- **Why text rendering, not a full-screen drawing API:**
  - the output stays consistent with every other screen;
  - search, scrolling, hex mode and resize work without each plugin re-implementing them;
  - the host keeps control of the console (no plugin-owned modal input loop);
  - the ABI is tiny. An interactive viewer API is listed as a later option (see Open questions).
- **Compatibility:** older plugins are unaffected (new optional exports, a new capability bit). A plugin declaring
  1.10 is refused by an older host, through the existing version check.

### D2 — Host: finding and running viewers (`PluginManager`)

- **`FindViewers(fileName)`** returns `{pluginId, viewerName, claim}` for every loaded `kMcPluginCapViewer`
  plugin whose `CanViewFile` claims the name. It is SEH-wrapped (PLUG-009) and in load order.
- **`RenderView(pluginId, filePath, lines, title)`** calls `McPlugin_RenderView` with a host-owned writer
  (SEH-wrapped).
- **Output limits:** at most **2 000 000 lines and 256 MB of text**, after which the host stops accepting lines
  and appends `… (output truncated)`. Lines are taken as given, and tabs are expanded by the viewer.
- **Viewing:** the host writes the lines to a temporary UTF-8 file with a BOM, opens the built-in viewer on it
  with **the plugin's title** (or `<file name> — <viewer name>`), and deletes the file afterwards.
- **A failed render:** if the plugin returns false or crashes, the host shows "<viewer name> could not show this
  file" and opens the built-in viewer instead, logging the reason (ERR-004).

### D3 — F3 and Shift+F3 resolution (`main.cpp`, VEE-009…012)

**F3**, in order:

1. the user's default `view` action from `file-actions.conf` (unchanged; it always wins);
2. the first viewer plugin claiming **`kMcViewDefault`** for the name;
3. the built-in viewer (unchanged).

**Shift+F3:** the chooser lists, after the file-action entries, every plugin viewer claiming the file
(`Hex dump (plugin)`), then the built-ins as today.

**`file-actions.conf`:**

- the program **`plugin:<plugin-id>`** selects a plugin viewer explicitly, e.g.
  `[*.bin] view* = Hex | plugin:com.mycommander.exampleplugin |`. It is valid only for `view` actions;
  `plugin:` on an `open` action gives a parse warning;
- File → Assign File to App offers the installed viewer plugins as choices;
- a `plugin:` entry whose plugin isn't loaded falls back to the built-in viewer with a one-line message.

**Inside plugin panels** (ZIP/FTP F3, which call the host's `ShowFileViewer`): the host now resolves step 2, a
`kMcViewDefault` viewer plugin, before the built-in viewer. It never resolves step 1's *external programs*,
because the file there is a temporary copy deleted right after viewing, and a detached editor would race that
deletion. A note in the developer guide explains this.

### D4 — Sample: a hex-dump viewer in ExamplePlugin

- `CanViewFile` → `kMcViewOffer` for every file, so it appears in Shift+F3 only and never takes over F3.
- `RenderView` writes `00000000  4D 5A 90 00 …  |MZ..|` lines, 16 bytes each, and polls `PollCancel` every
  64 KB.
- It is a developer-facing template (not installed by the installer, IS-0013 policy), and DEVELOPER_GUIDE's new
  "Viewers" section walks through it.

### D5 — What EXT-003 is *not* asking for (recorded so it isn't re-litigated)

- **A common file-system provider interface** (host F5 between two providers, Alt+F7 / compare / sync inside
  plugin panels): that is **EXT-002**, which stays LATER.
- **Plugin-supplied *editors*:** F4 is VEE-006 (external editor); a built-in editor is VEE-007, LATER.

## Files touched

| File | Change |
|---|---|
| `Source/MyCommanderPluginSdk/McPluginAbi.h`, `McPluginTypes.h`, `DEVELOPER_GUIDE.md` | Capability, exports, `McViewWriter`, API 1.10; a "Viewers" section |
| `Source/MyCommander/PluginManager.{h,cpp}` | Load the new exports, `FindViewers`, `RenderView` (SEH, limits), the viewer route for `HostShowFileViewer` |
| `Source/MyCommander/PluginManagerLogic.cpp` (pure) | `ChooseViewer(fileActionDefault, claims)`, the D3 order |
| `Source/MyCommander/FileActions.{h,cpp}` | `plugin:<id>` program for `view` actions (parse, validate, label) |
| `Source/MyCommander/Viewer.{h,cpp}` | `ViewerStart::title` (shown instead of the path) |
| `Source/MyCommander/main.cpp` | F3 / Shift+F3 / Assign File to App with viewer plugins; render to a temporary file; fallback message |
| `Source/ExamplePlugin/ExamplePlugin.cpp` | The hex-dump viewer |
| `Source/MyCommanderTests/PluginManagerTests.cpp`, `FileActionsTests.cpp` | Tests below |
| `README.md`, `USERGUIDE.md`, `CLAUDE.md`, `AIPrompt/my-commander.md`, `Version.h`, changelog | Docs; EXT-003 → Implemented (+ a PLUG-014 row for viewer plugins); history; 1.23.0 → 1.24.0 |

## Tests

**`PluginManagerTests.cpp`** (pure `ChooseViewer`):

- a file-action default beats a `kMcViewDefault` plugin, which beats the built-in viewer;
- `kMcViewOffer` never becomes the default;
- with two default claims, the first loaded wins;
- an empty claim list gives the built-in viewer;
- `IsPluginApiCompatible` accepts 1.10.

**`FileActionsTests.cpp`:**

- `plugin:com.x` parses for `view`, and on `open` gives a warning and is skipped;
- its label is shown in the chooser;
- a round trip through Assign File to App's line edits.

**Render limits:** a pure `ViewLineSink` (the host side of `McViewWriter`) is tested for the line and byte
caps and the truncation marker.

**Manual (ExamplePlugin built and deployed):**

- Shift+F3 on any file lists "Hex dump (plugin)", and choosing it shows the dump in the viewer, with search
  working;
- F3 is unchanged;
- `[*.bin] view* = Hex | plugin:com.mycommander.exampleplugin |` makes F3 use it;
- the plugin disabled via `disabledPluginIds` falls back with a message;
- inside a ZIP, F3 is still the built-in viewer (Hex is only an Offer claim).

## Phasing

1. **Phase 1:**
   - SDK types and exports, API 1.10;
   - `PluginManager` loading, `FindViewers`/`RenderView` with limits;
   - pure `ChooseViewer`, `ViewLineSink`, with tests.
2. **Phase 2:**
   - F3 / Shift+F3 / `plugin:` file actions / Assign File to App;
   - the `ShowFileViewer` route;
   - the viewer title.
3. **Phase 3:**
   - the ExamplePlugin hex-dump viewer;
   - DEVELOPER_GUIDE "Viewers";
   - docs, spec (EXT-003 Implemented, PLUG-014), version, changelog.

## Risks and notes

- **Huge outputs:** rendering a multi-GB file to text would be slow. The caps bound memory, and the plugin can
  poll `PollCancel`. The temporary file keeps the viewer's existing bounded-memory paging (VEE-004).
- **A plugin taking over F3** with `kMcViewDefault` for common types: the user's `file-actions.conf` always wins,
  and a plugin can be disabled (PLUG-010). The developer guide recommends `kMcViewOffer` unless the plugin is
  the obvious viewer for a format.
- **SEH isolation** covers render crashes (PLUG-009). A plugin that hangs in `RenderView` can only be guarded
  by its own `PollCancel` use; this is the same documented limit as every other plugin call.

## Open questions

1. **Viewer model:** text rendering into the host viewer (planned; consistent, small ABI), or an interactive
   full-screen drawing API for richer viewers (bigger; could follow later)?
2. **F3 default claims:** allow a plugin to become F3's default for files it claims (planned, with
   `file-actions.conf` still winning), or Shift+F3 / explicit `plugin:` entries only?
3. **Sample:** the hex-dump viewer in ExamplePlugin (planned, developer-facing, not installed), or a shipped,
   user-facing viewer plugin (e.g. hex or Markdown) bundled by the installer?

## Status

Implemented 2026-10-08, all three phases (version 1.24.0, plugin API 1.10), with the planned answers: text
rendering into the host viewer, `kMcViewDefault` allowed (file-actions always wins), and the hex dump in
ExamplePlugin. EXT-003 is Implemented and PLUG-014 is added.

Notes from implementing:

- **The default's F3 order is enforced by the pure `ChooseViewRoute`.** The Shift+F3 chooser's `*` marker
  follows the same order: with no file-action default, the `*` goes on the default-claiming plugin, if any,
  otherwise on the built-in viewer.
- **Nested calls:** `FindViewers` and `ShowPluginView` save and restore the "current plugin" because they can be
  reached from inside another plugin's `ShowFileViewer` call (ZIP/FTP F3).
- **The plugin side is verified** by loading `ExamplePlugin.dll` the way the host does and rendering a sample
  file (an ad-hoc check). A live F3 / Shift+F3 session in the running app is left for manual testing.
