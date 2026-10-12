# Feature Plan — FEA-0002 (MC Plugin)

Source: `AIPrompt/additional_feature_requests.md`

> Add plugin capability for the app (MC Plugin) — MC Plugin Requirements, 16 numbered sections
> (General Requirements; Identification and Metadata; Discovery and Loading; API and SDK;
> Lifecycle; Menu Integration; Help Integration; Left/Right Panel Integration; Dialog Support;
> Logging; UI Theme and Color Scheme; Keyboard Shortcuts; Communication with the Main
> Application; Communication Between Plugins; Error Handling and Isolation; Compatibility and
> Versioning). Full text preserved verbatim in the source file; not re-quoted here in full given
> its length — each Design section below cites its own source section number.

## Summary

This is, by a wide margin, the largest single feature requested in this project so far — it asks
for a general-purpose, DLL-based plugin architecture touching menu, help, both panels, dialogs,
logging, theming, keyboard shortcuts, and two directions of inter-component communication
(host↔plugin, plugin↔plugin), plus a versioned SDK a third party could build against. Read as one
implementation pass, it would be an enormous, high-risk change to a codebase whose entire
architecture today (`Panel`, `Dialog.h`, `MenuBar.h`, `Theme.h`) is built around a small, closed
set of concrete, non-virtual types with no extension points at all.

**Recommendation, stated up front rather than buried in Risk/scope notes:** build this in three
phases, each independently shippable and each *itself* the smallest useful slice of the section
it targets. Phase 1 (discovery/loading/metadata/versioning/menu/dialogs/logging/theme/shortcuts —
source §1–6, 9–13, 15–16) is a coherent, load-bearing unit: everything a plugin needs to add a
menu command that shows a dialog, logs, and reacts to the theme. Phase 2 (§7, Help Integration) is
small and additive once Phase 1 exists. Phase 3 (§8, Panel Integration, and §14, plugin↔plugin
communication) are each independently the size of a small feature on their own and depend on
Phase 1's plugin-identity/lifecycle machinery already existing. This plan designs all three so the
seams between them are real architectural boundaries, not an arbitrary cut — but implementation
should land them separately, each with its own build/test/changelog cycle, the same incremental
discipline every other multi-part fix in this project's history has followed (e.g. IS-0003's four
sub-features, each its own section).

Every section below is written against the actual current code (`MenuBar.h`, `Theme.h`,
`Console.h`, `Panel.h`, `Help.h`, `Log.h`, `Dialog.h`, all read in full before writing this), not
in the abstract — where the current architecture has no seam at all for a requested capability
(most acutely §8, panel content), that gap is named explicitly rather than hand-waved.

## Current state (what exists today, and why none of it is a plugin seam yet)

- `MenuBar.h`'s `TopMenu`/`MenuItem` list is a single `static const std::vector<TopMenu>` built by
  one function (`BuildMenus()`) — six fixed menus, items are one of `SimulatedKey` (replays an
  existing keyboard shortcut via `main.cpp`'s `SimulateKeyPress`), `ToggleColorMode` (one
  hardcoded action), or `Stub` (shows "not implemented"). Nothing here is insertable, removable,
  or extensible at runtime — it's baked at compile time.
- `Panel` (`Panel.h`) is a concrete class that *is* a filesystem browser — `Entries()` returns
  real `Entry` structs read from `std::filesystem`, `Refresh()`/`PumpRefresh()` read real
  directories. There is no `IPanelContent` abstraction, no virtual rendering hook, nothing a
  plugin could substitute its own content into. `main.cpp`'s `DrawPanel` reads a `const Panel&`
  directly. This is the single biggest gap between what exists and what §8 asks for.
- `Theme.h`'s live palette (`kAttrNormal` etc.) is a set of mutable globals, read directly by
  ~90 call sites across `main.cpp`/`Dialog.cpp`/`Help.cpp`/`Viewer.cpp`. There's no
  subscribe-to-changes mechanism — `ApplyThemePalette` just overwrites the globals once at
  startup (and once more on IS-0006's live config reload).
- `Dialog.h`'s `ShowChoiceDialog`/`ShowMessage`/`ShowTextPrompt`/`ShowProgress` already take only
  a `Console&` plus plain data (no `Panel`/app-state dependency) — these are naturally
  plugin-exposable almost as-is, just needing a C-ABI wrapper (see §9 below).
  the same for `Log.h`'s `Logger` (already a small class taking a level+message).
- `Help.h`'s `ShowShortcutReference` renders from a `static const` grouped-sections table
  (`Sections()` inside `Help.cpp`, not touched in this read but referenced by its own doc
  comment) — not Markdown, a hand-built category→shortcut table. §7 asks for Markdown-rendered
  plugin content alongside it, which is a new, small rendering capability, not a reuse of the
  existing table format.
- Nothing in the codebase loads a DLL at runtime today (`LoadLibraryW`/`GetProcAddress` appear
  nowhere) — every "external process" interaction (`Process.h`) spawns a *separate* process
  (`F4` edit, `F9` shell), never loads code into this one. Plugin loading is a genuinely new
  capability, not an extension of something already there.

## Design

### 1. ABI: why this has to be a C-style, versioned struct of function pointers

C++ class vtables are not a stable ABI across compilers or even across MSVC versions/runtime
library configurations — a plugin built with a different toolset than the host risks silent
corruption, not a clean failure. Given source §4's own "stable interface"/"well-defined" language
and §16's explicit versioning requirement, the SDK boundary must be plain C: fixed-layout structs
of `extern "C"` function pointers and POD data, the same approach every long-lived native plugin
ABI (VST, LADSPA, the original Windows plugin conventions) converged on for exactly this reason.
Every pointer/struct crossing the DLL boundary is a C type; C++ is free to be used *inside* a
plugin's own implementation, just never in what's exported.

```c
// McPluginAbi.h -- the one header both the host and every plugin compile
// against. No C++ types cross this boundary; every struct here is a
// fixed, append-only layout (new fields only ever added at the end,
// guarded by MC_PLUGIN_API_VERSION -- see #16 below).

#define MC_PLUGIN_API_VERSION_MAJOR 1   // breaking ABI change -> bump major, reject on mismatch
#define MC_PLUGIN_API_VERSION_MINOR 0   // additive-only change -> bump minor, host may still accept an older plugin

// The one symbol every MC Plugin DLL must export, by name, stdcall,
// undecorated (extern "C") -- everything else is reached through the
// struct it returns.
typedef struct McPluginInfo* (__stdcall *McPlugin_GetInfoFn)(void);
// Exported literally as: extern "C" __declspec(dllexport)
//   McPluginInfo* __stdcall McPlugin_GetInfo(void);
```

### 2. Plugin identification and metadata (§2)

```c
typedef struct McPluginMetadata {
    uint32_t   structSize;         // sizeof(McPluginMetadata) as the plugin compiled it -- lets
                                    // the host safely read only the fields it knows about even
                                    // against a newer-minor-version plugin (see #16)
    uint32_t   apiVersionMajor;
    uint32_t   apiVersionMinor;
    uint32_t   magic;              // MC_PLUGIN_MAGIC ('M'<<24|'C'<<16|'P'<<8|'L') -- the cheap,
                                    // fast first check that distinguishes an MC Plugin DLL from
                                    // an unrelated DLL that happens to export a same-named
                                    // symbol by coincidence, before any real parsing happens
    const wchar_t* id;             // stable, unique (reverse-DNS-style recommended, e.g.
                                    // "com.example.ftpplugin") -- the key everything else (menu
                                    // item ownership, log-line tagging, service registry) is
                                    // filed under
    const wchar_t* name;           // display name
    const wchar_t* description;
    const wchar_t* version;        // the plugin's own version string, free-form
    const wchar_t* author;
    uint32_t   capabilities;       // bitmask of McPluginCapability (below) -- declared up front
                                    // so the host knows which optional lifecycle callbacks it's
                                    // safe to call without a null check on every one
} McPluginMetadata;

typedef enum McPluginCapability {
    kMcPluginCapMenu       = 1 << 0,  // implements OnMenuAction / registers menu items
    kMcPluginCapPanel      = 1 << 1,  // implements the panel-content callbacks (#8)
    kMcPluginCapHelp       = 1 << 2,  // provides GetHelpMarkdown (#7)
    kMcPluginCapShortcuts  = 1 << 3,  // registers keyboard shortcuts (#12)
    kMcPluginCapServices   = 1 << 4,  // publishes a plugin↔plugin service (#14)
} McPluginCapability;
```

`McPlugin_GetInfo` must be callable **before** any other lifecycle step, with no side effects
(§3's "identified... before activating" and §16's "declare... version... before initialization")
— the host calls it immediately after `LoadLibraryW` succeeds, purely to read metadata, and calls
`FreeLibrary` again without ever initializing the plugin if the magic/version check fails. This
keeps "identify" and "activate" as two genuinely separate steps, matching §5's lifecycle list.

### 3. Discovery and loading (§3)

New `PluginManager` (`Source/MyCommander/PluginManager.h`/`.cpp`), pure enough to unit-test its
decision logic separately from the real `LoadLibraryW` calls (same "pure decision logic in one
function, real OS calls in a thin wrapper around it" split `Elevation.h`/`SetupLogic.h` already
established):

- **Discovery**: enumerate `*.dll` in a `Plugins\` subfolder next to `MyCommander.exe` (not
  `%APPDATA%` — plugins are installed alongside the app, config/session/log data is per-user;
  matches this project's existing `%APPDATA%\MyCommander\...` vs. install-directory split).
- **Identification**: `LoadLibraryW`, `GetProcAddress(L"McPlugin_GetInfo")` — a DLL missing that
  export, or whose `McPluginInfo::magic` doesn't match, is logged at `Warning` and immediately
  `FreeLibrary`'d, never touched again (§2's "distinguish MC Plugin DLLs from unrelated DLL
  files" — this is the whole mechanism).
- **Compatibility validation**: `apiVersionMajor` must equal the host's exactly; `apiVersionMinor`
  may be less than or equal to the host's (an older-minor plugin only used fields the host still
  supports; a newer-minor plugin declares a minor version the host doesn't understand yet, so it's
  rejected rather than guessed at) — pure function `IsPluginApiCompatible(hostMajor, hostMinor,
  pluginMajor, pluginMinor) -> bool`, directly unit-testable, no DLL needed.
- **Disabling**: a new `Config::disabledPluginIds` (`std::vector<std::wstring>`, following exactly
  IS-0006's own `ConfigKeyBlocks()` convention for a new setting, comma-separated ids) — checked
  *before* `LoadLibraryW` even runs for that DLL's id (identification still happens, so it's still
  listed as "present but disabled" rather than invisible, but no lifecycle step past identification
  ever runs) — satisfies §3's "mechanism for disabling problematic plugins" and §15's same
  requirement without needing any new UI: it's a config setting, consistent with every other
  app-behavior toggle in this project.
- All loading/identification/rejection is logged via the existing `Logger` (§3's "reported using
  the main application logging feature" — no new logging mechanism, straight reuse).

### 4. Plugin API and SDK (§4)

The SDK *is* `McPluginAbi.h` plus a small set of sibling headers (`McPluginHost.h` for the
struct the host hands the plugin at init — see #13 — and `McPluginTypes.h` for shared enums like
`McDialogButton`) — header-only, no separate SDK library to link, matching DEC-002's "no
third-party dependency" spirit extended to the plugin boundary itself: a plugin author needs
nothing but a C or C++ compiler and these headers. Ship an example skeleton plugin project
(`Source/ExamplePlugin/`, not part of `MyCommander.sln`'s own build — a separate, standalone
`.vcxproj` a plugin author can copy) implementing the smallest possible `kMcPluginCapMenu` plugin —
satisfies §4's "examples." "Documentation" is the Developer Guide, its own deliverable, see §17
below.

### 5. Lifecycle (§5)

```c
typedef enum McPluginState {
    kMcPluginDiscovered, kMcPluginValidated, kMcPluginLoaded, kMcPluginInitialized,
    kMcPluginActive, kMcPluginDeactivated, kMcPluginShutdown, kMcPluginUnloaded,
} McPluginState;
```

Exactly the nine states §5 lists, as a linear state machine `PluginManager` drives one plugin at a
time through in order; a failure at any step logs and jumps straight to `Deactivated`→`Shutdown`→
`Unloaded` for *that* plugin only (§1's "a plugin failure shall not prevent the main application or
other plugins from operating" — each plugin's state machine is independent). Two more exported
functions, both optional (checked via `capabilities`):

```c
typedef bool (__stdcall *McPlugin_InitFn)(const McPluginHost* host);   // Initialization -> Activation
typedef void (__stdcall *McPlugin_ShutdownFn)(void);                  // Deactivation -> Shutdown
```

`McPlugin_Shutdown` is the one place §5's "release its registered resources... menu items,
keyboard shortcuts, UI elements, event subscriptions, registered services" is *enforced*, not just
requested: `PluginManager` itself walks its own registries (menu items, shortcuts, services — all
tagged with the owning plugin's id per #2) and removes every entry tagged with that plugin's id
immediately after calling `McPlugin_Shutdown`, rather than trusting the plugin to have done it —
the host is the source of truth for "does this menu item still exist," a plugin's own bookkeeping
is not, so cleanup can't be skipped by a buggy plugin.

### 6. Menu integration (§6)

`MenuBar.h`'s `TopMenu`/`MenuItem` (already read in full) needs two changes, both additive:

- `MenuItemKind` gains a fourth value, `PluginCommand`, carrying a plugin id + command id instead
  of a `HintAction` — dispatched by calling that plugin's `OnMenuAction(commandId)` callback
  (part of its own exported interface struct, see #13) instead of `SimulateKeyPress`.
- `BuildMenus()`'s `static const std::vector<TopMenu>` becomes the **built-in** menu set only;
  `main.cpp` (or `PluginManager`) keeps a separate, mutable "plugin menu overlay" —
  insert-at/append/remove/enable-disable all operate on this overlay, never on the built-in
  vector, so a plugin can never corrupt or shadow a built-in item. Rendering merges the two: for
  each `TopMenu`, its built-in items plus any plugin items targeting that menu (insertion point
  specified by the plugin as "after built-in item mnemonic X" or "append"), in a stable order.
  This mirrors exactly how IS-0006's config self-healing and IS-0007's hint-bar row layout both
  already separate "the fixed, compiled-in table" from "what actually gets drawn," rather than
  mutating the fixed table in place.
- Enable/disable is a per-item flag on the overlay entry, checked at render time (a disabled item
  still exists, so `Remove` and `Disable` stay genuinely different operations, matching §6's own
  separate wording for them) and at mnemonic-dispatch time (a disabled item's mnemonic doesn't
  fire).

### 7. Help integration (§7) — Phase 2

Every `kMcPluginCapHelp` plugin exports one more function:

```c
typedef const wchar_t* (__stdcall *McPlugin_GetHelpMarkdownFn)(void);  // UTF-16, plugin-owned
                                                                        // lifetime (valid until
                                                                        // McPlugin_Shutdown)
```

`Help.cpp`'s existing `Sections()` table stays exactly as-is for built-in shortcuts; a new,
separate "Plugins" section is appended to the shortcut-reference screen's own scrollable content,
one sub-heading per loaded `kMcPluginCapHelp` plugin (its `name` from metadata — satisfies "clearly
identifiable"), rendering that plugin's Markdown through a small, deliberately minimal renderer
(headings, bullet lists, plain paragraphs, inline code spans — not a general CommonMark
implementation, which would be wildly disproportionate for a read-only help screen; anything more
exotic in a plugin's Markdown source renders as plain text, a documented, acceptable
degradation). "Same rendering mechanism as the main application help" is satisfied by this content
appearing *inside* `ShowShortcutReference`'s own scroll view, not a second, separate screen.

### 8. Left/right panel integration (§8) — Phase 3, the largest single gap

This is the one section with no existing seam at all (see Current state above). Minimal viable
design: introduce `IPanelContent`, a small abstract interface `Panel` gains an optional pointer
to; when set, `main.cpp`'s `DrawPanel` renders through it instead of `Panel`'s own
`Entries()`/cursor, and panel-targeted key/mouse events are offered to it first.

```cpp
// Panel.h addition -- pure virtual, no plugin/DLL dependency leaks into
// Panel.h itself; PluginManager owns the concrete adapter that forwards
// to a plugin's C exports.
class IPanelContent {
public:
    virtual ~IPanelContent() = default;
    virtual void Draw(Console& console, SHORT x, SHORT y, SHORT width, SHORT height, bool active) = 0;
    virtual void OnKey(WORD virtualKeyCode, DWORD controlKeyState) = 0;
    virtual void OnMouseClick(SHORT relativeX, SHORT relativeY, bool doubleClick) = 0;
};
```

A plugin requests panel content via its host-services struct (#13: `RequestPanelContent(side,
title)`); the host installs an `IPanelContent` adapter on that `Panel`, which forwards `Draw`/
`OnKey`/`OnMouseClick` to the plugin's own exported `OnPanelDraw`/`OnPanelKey`/`OnPanelClick`
C functions, translating `Console&` drawing into simple C primitives (`McPanelDrawText(x, y, text,
colorId)`, colors referenced by the theme's own named slots, not raw `WORD` attributes — satisfies
§11's "access to the application's standard colors" without leaking `Theme.h`'s Win32-specific
`WORD` type across the ABI). "Clear"/"Remove" uninstall the adapter, reverting that `Panel` to its
normal filesystem-browsing behavior. Explicitly **not** attempted in this design: a plugin
replacing panel *navigation* semantics (Back/Forward history, `NavigateTo`) — panel content plugins
own drawing and input only, the panel's own path/history machinery is untouched, since the request
describes "visual content" and "user interaction," not a full alternate `Panel` implementation.

### 9. Dialog support (§9)

Thin C wrappers around the *already Console-only, app-state-free* `Dialog.h` functions:

```c
typedef int  (__stdcall *McHost_ShowChoiceFn)(const wchar_t* title, const wchar_t** lines, int lineCount,
                                              const McDialogOption* options, int optionCount);
typedef void (__stdcall *McHost_ShowMessageFn)(const wchar_t* title, const wchar_t** lines, int lineCount);
typedef bool (__stdcall *McHost_ShowTextPromptFn)(const wchar_t* title, const wchar_t* initialText,
                                                   wchar_t* outBuffer, int outBufferSize);
```

One function per dialog *kind* named in §9 (configuration/input/info/warning/confirmation/error)
would be pure duplication — `ShowMessage`/`ShowChoiceDialog`/`ShowTextPrompt` are already generic
enough that "warning dialog" vs. "error dialog" vs. "info dialog" is just a different `title`/icon
convention, not different code; the plugin picks the title/wording, the host doesn't need to know
which "kind" it is. These functions live in the host-services struct (#13), called with the
plugin's own `redrawBackground` implicitly being "redraw the whole frame," since a plugin has no
narrower background to redraw.

### 10. Logging (§10)

One function, host-services struct:

```c
typedef void (__stdcall *McHost_LogFn)(McLogLevel level, const wchar_t* message);
```

The host prefixes every plugin log line with `[pluginId] ` before handing it to the existing
`Logger::Log` — satisfies "identify the plugin that generated them" with zero new logging
machinery; `McLogLevel`'s four values map 1:1 onto `LogLevel`'s `Error`/`Warning`/`Info`/`Debug`
(never `Off` — a plugin can't silence the host's own logging, only choose which of its own lines
to send, matching "plugins shall have access to the log levels supported" as read/write access to
severity choice, not control over the host's threshold).

### 11. UI theme and color scheme (§11)

Not raw `WORD` attributes (Win32-specific, and exposing `Theme.h`'s live mutable globals directly
would let a plugin corrupt the host's own theme state) — a small, named, read-only snapshot:

```c
typedef struct McThemeSnapshot {
    uint32_t structSize;
    uint32_t normalFg, normalBg, dirFg, dirBg, selectedFg, selectedBg, /* ...one pair per
                                                                          ThemePalette field */;
} McThemeSnapshot;
typedef void (__stdcall *McHost_GetThemeFn)(McThemeSnapshot* out);
```

Colors as plain RGB-ish small integers (the 16-color console palette indices Win32 already uses),
not opaque `WORD` bitmasks — a plugin author shouldn't need to know `FOREGROUND_INTENSITY` exists.
Theme-change notification (§11's last two bullets) is a plugin-provided optional callback,
`OnThemeChanged(const McThemeSnapshot*)`, called by `ApplyThemePalette` (`Theme.h`) itself right
after it updates the live globals — the one place a theme change already happens, so no new
notification plumbing beyond iterating `PluginManager`'s loaded-plugin list from there.

### 12. Keyboard shortcuts (§12)

```c
typedef void (__stdcall *McPlugin_OnShortcutFn)(uint32_t commandId);
typedef struct McShortcutSpec { WORD virtualKeyCode; bool ctrl, shift, alt; uint32_t commandId; } McShortcutSpec;
typedef bool (__stdcall *McHost_RegisterShortcutFn)(const McShortcutSpec* spec);   // false = conflict
typedef void (__stdcall *McHost_UnregisterShortcutFn)(uint32_t commandId);
```

Conflict detection: a new plugin-shortcut registry (id+modifiers → owning plugin id) checked
against *both* the built-in key-dispatch switch's own fixed key set (a small, hand-maintained
`constexpr` list of every VK+modifier combination `wmain`'s switch already handles — the one
place this plan asks for a literal, mechanical inventory of existing behavior, since there's no
existing single source of truth for "every key this app already binds") and every other
already-registered plugin shortcut; `RegisterShortcut` returns `false` on either collision
without registering anything (§12's "detect conflicts," read as "prevent," not just "report" —
silently letting two commands fight over one key press would be worse than a plugin author
finding out immediately via a `false` return). Dispatch: `main.cpp`'s key-dispatch switch's
existing `default:` case (currently just `appendToCommandLine`) gains one more check *before* that
fallback — look up the pressed key+modifiers in the plugin-shortcut registry, call
`OnShortcutFn(commandId)` if found, otherwise fall through to today's behavior unchanged. Built-in
shortcuts are still checked first (they're the `case` labels above `default`), so a plugin can
never shadow an existing app key — only ever fill a genuinely unused combination.

### 13. Communication with the main application (§13)

The `McPluginHost` struct, handed to every plugin once at `McPlugin_Init` (not re-fetched per
call — a stable pointer for the plugin's whole lifetime, freed only after `McPlugin_Shutdown`
returns):

```c
typedef struct McPluginHost {
    uint32_t structSize;
    // Dialogs (#9), logging (#10), theme (#11), shortcuts (#12) -- one
    // function pointer field per Mc Host_*Fn type defined in those sections.
    McHost_ShowChoiceFn      ShowChoice;
    McHost_ShowMessageFn     ShowMessage;
    McHost_ShowTextPromptFn  ShowTextPrompt;
    McHost_LogFn             Log;
    McHost_GetThemeFn        GetTheme;
    McHost_RegisterShortcutFn   RegisterShortcut;
    McHost_UnregisterShortcutFn UnregisterShortcut;
    // Menu (#6), panel content (#8), services (#14) -- registration/query
    // functions, each taking this plugin's own id (bound at Init, so the
    // plugin never has to pass it back itself and can never claim to *be*
    // a different plugin).
    McHost_AddMenuItemFn     AddMenuItem;
    McHost_RemoveMenuItemFn  RemoveMenuItem;
    McHost_SetMenuItemEnabledFn SetMenuItemEnabled;
    McHost_RequestPanelContentFn RequestPanelContent;
    McHost_ReleasePanelContentFn ReleasePanelContent;
    McHost_PublishServiceFn  PublishService;
    McHost_QueryServiceFn    QueryService;
    // Application state queries -- read-only, matching "query relevant
    // application state" without exposing Panel/Config C++ types directly.
    McHost_GetActivePanelPathFn GetActivePanelPath;
    McHost_GetSelectedNamesFn   GetSelectedNames;
} McPluginHost;
```

"Execute supported application commands" (§13) is satisfied by the same `SimulateKeyPress`
mechanism IS-0003's hint-bar clicks already use — a plugin asking the host to "run Copy" is
functionally identical to a plugin registering *its own* shortcut and having the host inject the
built-in `F5` key press on its behalf; no separate "command execution" API is needed beyond
exposing that one primitive as `McHost_SimulateKeyFn` alongside the others above.

### 14. Communication between plugins (§14) — Phase 3

A host-mediated service registry — `PluginManager` owns `std::unordered_map<std::wstring,
ServiceEntry>` (keyed by a plugin-chosen service name), `ServiceEntry{ownerPluginId, void*
serviceInterface, uint32_t interfaceVersion}`:

```c
typedef bool (__stdcall *McHost_PublishServiceFn)(const wchar_t* serviceName, void* interfacePtr,
                                                   uint32_t interfaceVersion);
typedef void* (__stdcall *McHost_QueryServiceFn)(const wchar_t* serviceName, uint32_t requestedVersion);
```

The `void* interfacePtr` is deliberately opaque to the host — two plugins agreeing on a shared
struct layout for a given `serviceName` is a contract between *them*, not something the host
parses or validates beyond "does this name exist, and does its published version satisfy what was
requested" (a plain integer `>=` check, no structural compatibility checking attempted, since the
host has no way to know what's actually inside an opaque `void*`). "Determine whether another
plugin or required capability is available" is `QueryService` returning `nullptr` — no separate
existence-check function needed. Every published service is automatically unpublished when its
owning plugin unloads (the same `PluginManager`-owns-cleanup pattern as #5's menu/shortcut
teardown), so a consumer plugin querying a service must always be prepared for `nullptr`, never
assume a once-found service stays available — documented in the SDK, not mechanically enforced
(there's no way to *force* a plugin to null-check its own cached pointer).

### 15. Error handling and isolation (§15)

Every call *into* plugin code (`McPlugin_GetInfo`/`Init`/`Shutdown`/`OnMenuAction`/
`OnShortcut`/panel callbacks) is wrapped in Windows SEH (`__try`/`__except
(EXCEPTION_EXECUTE_HANDLER)`) at the single choke point `PluginManager` already funnels every
plugin call through — a structured exception (access violation, divide-by-zero, a Windows
first-chance hardware exception) inside that call is caught, logged with the plugin's id and
exception code, and treated as that call having failed; **stated plainly, since the request's own
"shall not prevent the main application... from operating" needs an honest limit attached**: SEH
catches *structured* exceptions, not silent heap corruption, a stack overflow that's already
unwound past recovery, or a plugin spawning its own thread that then crashes — true fault isolation
for arbitrary third-party code requires running each plugin in its own process (out-of-process
COM or an RPC boundary), which is a fundamentally different, much larger architecture than "load a
DLL into this process," and is explicitly **not** what's being designed here (the request itself
says "MC Plugin DLLs," i.e. in-process). This trade-off should be confirmed with the user before
Phase 1 implementation begins, since it's the one place the literal requirement text ("shall not
prevent... from operating") and what's actually achievable with the requested DLL-based approach
diverge — see Risk/scope notes.

### 16. Compatibility and versioning (§16)

Already covered by #1's `MC_PLUGIN_API_VERSION_MAJOR`/`MINOR` and #3's `IsPluginApiCompatible` —
listed here only to confirm every one of §16's bullets maps onto something concrete above: "support
API versioning" → the two version constants; "declare... version... compatible with" →
`McPluginMetadata::apiVersionMajor/Minor`; "verify... before initialization" →
`IsPluginApiCompatible` gates the Validated→Loaded transition in #5's state machine, strictly
before `McPlugin_Init` is ever called; "incompatible plugins shall not be activated" → a failed
check jumps straight to Unloaded, never reaching Loaded/Initialized/Active; "clearly reported" →
logged at `Warning` with the plugin's id, declared version, and the host's own version, in one line.

### 17. Developer guide for the MC Plugin SDK (addition beyond the source's 16 sections)

Not asked for by any of the 16 numbered requirements verbatim, but load-bearing for §4's "simple
and easy-to-use API and SDK" and "documentation... for plugin developers" to mean anything in
practice — a header a developer has to reverse-engineer by reading `PluginManager.cpp` isn't
"easy to use." One new document, own deliverable, own file, separate from the terse example-plugin
README the earlier draft of §4 sketched (that README is now folded into this guide instead of
existing alongside it, so there's one authoritative doc, not two overlapping ones):

**`Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md`** — plain Markdown (consistent with every other
doc in this repo — `README.md`, `USERGUIDE.md`, `CLAUDE.md` — no new doc toolchain), living beside
the SDK headers it describes rather than under `AIPrompt/` (which is this project's own internal
planning archive, not something a third-party plugin author would ever be pointed at). Contents,
one chapter per concern, each grounded in the concrete types/functions this plan already defined
above (not reworded — the guide *is* the readable narrative form of §1–16's structs and function
pointers, cross-referenced by section number so a change to the design here has an obvious,
single place in the guide that needs updating too):

1. **Getting started** — toolchain requirements (a C or C++17-capable compiler able to produce a
   `x64` Windows DLL; MSVC is what the host itself is built with, but the ABI being plain C means
   any compiler producing a compatible calling convention works), the minimum DLL surface (export
   `McPlugin_GetInfo`, nothing else, to be a *valid but inert* plugin), and a "five-minute" walk
   from an empty `.vcxproj` to a plugin the host discovers and logs as loaded but inactive.
2. **The ABI contract** (§1, §16) — why C, not C++, crosses the boundary; the
   `structSize`/major/minor versioning discipline and exactly what changes are safe
   (append-only, minor bump) versus breaking (anything else, major bump); what the host does with
   an incompatible plugin, so an author isn't guessing why their DLL was silently skipped.
3. **Metadata reference** (§2) — every `McPluginMetadata` field, required vs. optional, the magic
   constant, and worked-through capability-bit examples.
4. **Lifecycle** (§5) — the nine-state diagram in prose, which two callbacks are optional and how
   `capabilities` controls whether the host ever calls them, and the concrete resource-cleanup
   contract from §5/#5 (the host removes a plugin's menu items/shortcuts/services itself after
   `Shutdown` returns — a plugin doesn't have to, and can't prevent it by forgetting to).
5. **One chapter per capability** — Menu (§6), Help Markdown (§7), Panel content (§8, marked
   clearly as a Phase 3 capability not available until that phase ships), Dialogs (§9), Logging
   (§10), Theme (§11), Shortcuts (§12) — each with the relevant struct/function signatures already
   defined above plus one short, runnable code snippet.
6. **Talking to the host and other plugins** (§13, §14) — the full `McPluginHost` field list with
   a one-line description per function pointer; the service-registry pattern (§14) with a
   publish/query worked example between two hypothetical plugins, including the "a queried service
   pointer can become stale the moment its owner unloads — always be prepared for a later call to
   fail" caveat spelled out in plain language, not just the structural note in §14 above.
7. **Error handling and what isolation actually means** (§15) — states plainly, for a plugin
   author's benefit as much as the end user's, that SEH at the host's call boundary catches
   structured exceptions but is not a substitute for the plugin's own correctness; recommends
   defensive patterns (validate host pointers before use, avoid unbounded recursion/allocation
   inside a callback the host is waiting on).
8. **Worked example** — a fully annotated walk through `Source/ExamplePlugin/`, tying every file
   in that project back to the chapter that explains it.
9. **Troubleshooting** — a short table of "my plugin doesn't load / doesn't show up / its menu
   item doesn't fire" against the specific log line the host would have written for each cause
   (missing export, magic mismatch, version mismatch, disabled via config, shortcut conflict
   rejected at registration) — so a plugin author's first stop is this table, not asking the
   host's own maintainers to read their DLL's disassembly.

Ships with **Phase 1** (not deferred to the end) — a plugin author needs chapters 1–4, 5's
menu/dialog/logging/theme/shortcut sub-chapters, 6's host-communication chapter, and 7 the moment
Phase 1 lands, since that's the exact surface Phase 1 exposes; chapters 5's Help/Panel
sub-chapters are added when Phases 2/3 respectively ship, not written speculatively ahead of the
code they'd be describing. No automated test applies to a Markdown document; verification is a
manual completeness pass confirming every exported SDK symbol at the time of each phase's release
has a corresponding guide chapter, mirroring how this project already keeps `Help.cpp`'s shortcut
table and `README.md`'s keybindings table in sync by manual cross-check (`CLAUDE.md`'s own stated
convention) rather than a generated-from-one-source mechanism.

## Files touched (once implemented — none touched by this plan document itself)

- New: `Source/MyCommanderPluginSdk/McPluginAbi.h`, `McPluginHost.h`, `McPluginTypes.h` — the
  SDK headers, header-only, no `.cpp`.
- New: `Source/MyCommander/PluginManager.h`/`.cpp` — discovery, loading, lifecycle, the menu/
  shortcut/service registries, SEH-wrapped call dispatch.
- New: `Source/ExamplePlugin/` — a standalone example `.vcxproj`, not part of `MyCommander.sln`.
- New: `Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md` — the SDK developer guide (§17), shipped
  incrementally alongside each phase as described there.
- Modified: `Panel.h`/`Panel.cpp` (`IPanelContent`, Phase 3), `MenuBar.h` (`PluginCommand` item
  kind + the overlay merge, Phase 1), `Theme.h` (`ApplyThemePalette` gains the
  plugin-notification call, Phase 1), `Help.cpp` (Plugins section + Markdown mini-renderer, Phase
  2), `main.cpp` (loads `PluginManager` at startup after `Config`, wires the key-dispatch
  fallback, calls `DrawPanel` through `IPanelContent` when set), `Config.h`/`Config.cpp`
  (`disabledPluginIds`, following `ConfigKeyBlocks()`'s existing per-key convention).
- New test files: `PluginManagerTests.cpp` (pure logic: magic/version validation,
  `IsPluginApiCompatible`, menu-overlay merge ordering, shortcut-conflict detection, service
  registry publish/query/auto-unpublish-on-unload) — no real `LoadLibraryW` calls in automated
  tests, matching `Elevation.h`/`MyCommanderSetup`'s own established precedent for real-OS-
  side-effect code; a hand-built stub DLL fixture could cover the actual loading path if wanted
  later, not designed here.

## Test plan

Every piece of this design that's pure decision logic (not a real DLL/Win32 call) is unit-testable
the same way `Elevation.h`'s `BuildElevatedHelperParameters` and `SetupLogic.h`'s installer-path
logic already are: `IsPluginApiCompatible` (major mismatch rejected, minor-newer rejected,
minor-older-or-equal accepted), the menu overlay's insert/append/remove/enable-disable merge
ordering against a fixed built-in `TopMenu` list, shortcut-conflict detection against both a
built-in-key inventory and other plugins' registrations, and the service registry's
publish/query/version-check/auto-unpublish-on-simulated-unload behavior. `PluginManager`'s actual
`LoadLibraryW`/`GetProcAddress`/SEH-wrapped-call code, like `Elevation.h`'s real `RunElevated`,
gets no automated test — real DLL loading and SEH recovery from an actually-crashing callee aren't
practically exercisable in an automated suite without a purpose-built crashing test DLL, which is
out of scope for this plan (a reasonable follow-up, not designed here). Manual verification against
the example plugin (§4) is the practical substitute, the same standing limitation this whole
project's `main.cpp`-adjacent, real-OS-side-effect code has always had.

## Risk / scope notes

- **This needs to be built in phases, not one pass** — restated from the Summary because it's the
  single most important scope decision in this document. Attempting all three phases as one
  change would be an enormous, hard-to-review, hard-to-roll-back-piece-by-piece unit, exactly the
  kind of change this project's own established practice (one fix/feature at a time, its own
  build+test+changelog cycle) exists to avoid.
- **In-process isolation has a real, honest ceiling** (§15, detailed above) — SEH catches
  structured exceptions at the call boundary, not every possible way third-party code can take
  the host down. Worth explicit user sign-off before Phase 1 starts, since "a plugin failure shall
  not prevent the main application... from operating" is stated as an absolute in the request but
  can only be delivered as "best-effort, with a named and documented limit" by an in-process DLL
  architecture.
- **§8 (panel content) is architecturally the largest gap from today's code** — `Panel` has no
  extension point today; `IPanelContent` is a genuinely new abstraction layered on top of it, not
  a small addition. Recommended as Phase 3 specifically because Phases 1–2 are independently
  useful without it, and because getting the `IPanelContent`/`Panel` boundary right benefits from
  Phase 1's plugin-lifecycle machinery already existing and already being exercised by real
  plugins.
- **Markdown rendering (§7) is deliberately minimal**, not a general CommonMark implementation —
  proportional to "render inside a read-only help screen," not a general document viewer.
- **A new requirement-ID namespace is needed** — this plan doesn't invent requirement IDs itself
  (that's the spec's job, updated when Phase 1 actually lands), but flags that `CLAUDE.md`'s
  existing namespace list (`CON-`, `UI-`, ..., `MENU-`, `PROP-`) will need a new prefix (e.g.
  `PLUG-`) registered alongside it at implementation time, the same way `MENU-`/`PROP-` were
  presumably added for FEA-0005/FEA-0003.
- Out of scope entirely, not addressed by this plan: FEA-0001 (an actual FTP plugin) and any other
  concrete plugin — this plan is the platform only, matching how FEA-0001 is filed as its own,
  separate, later feature request in the same source document.

## Status

**Phase 1: implemented** (2026-09-30), per the design above, with no deviations. Before starting,
confirmed with the user via `AskUserQuestion`: (1) start with Phase 1 only, not all three phases
at once; (2) accept SEH-based best-effort plugin isolation (§15) rather than redesigning around
true process isolation. Both confirmed.

**Deployment fix (2026-09-30, user-reported):** `ExamplePlugin.vcxproj`'s build had never actually
put `ExamplePlugin.dll` anywhere `PluginManager::DiscoverAndLoad` would find it — the standalone
project built into its own `Source\ExamplePlugin\Build\...`, not `Build\x64\$(Configuration)\
Plugins\` next to the real `MyCommander.exe` the solution produces, so every actual run of the app
loaded zero plugins despite every earlier build/`dumpbin` check passing. Fixed with a
`PostBuildEvent` that copies the DLL into place after every build; verified by launching
`Build\x64\Debug\MyCommander.exe` for real and confirming `mycommander.log` shows the plugin
discovered, identified, initialized, and its menu item/shortcut registered — the first genuine
in-app (non-`dumpbin`) confirmation that plugin loading works end to end. Clicking the menu item /
pressing its shortcut interactively is still unverified (no interactive-console capture available
in this environment).

- §1–4 (general requirements, identification/metadata, discovery/loading, API/SDK): `McPluginAbi.h`,
  `PluginManager::DiscoverAndLoad`, and the SDK headers/Developer Guide/example plugin all landed
  exactly as designed.
- §5 (lifecycle): the nine-state model, host-enforced menu/shortcut cleanup on unload, and SEH
  wrapping at every call boundary all landed exactly as designed.
- §6 (menu integration): `MergePluginMenuItems`/the `PluginCommand` `MenuItemKind`/`gMergedMenus`
  landed exactly as designed — Phase 1 ships insertion as append-only (`kMcMenuPlacementAfter`'s
  exact index placement is accepted but not yet honored, silently falling back to append, the
  same graceful fallback the design already specified for an unmatched `afterItemMnemonic`); a
  precise insertion-point refinement is a small, separable follow-up if ever wanted, not treated
  as a Phase 1 gap.
- §9–13 (dialogs, logging, theme, shortcuts, host communication): all landed exactly as designed,
  including the `McThemeSnapshot` fg/bg packing and `SimulateKey`/`GetActivePanelPath`/
  `GetSelectedNames`.
- §15–16 (error handling/isolation, versioning): landed exactly as designed.
- §17 (Developer Guide): shipped with Phase 1, extended for Phase 2 (a new "Help content" section
  under §5, a `kMcPluginCapHelp` row in the capabilities table), then Phase 3 ("Panel content" and
  "Inter-plugin services" sections under §5, plus `kMcPluginCapPanel`/`kMcPluginCapServices`
  capabilities-table rows) — the guide's intro now says it covers the complete architecture, all
  three phases, rather than listing which phases are and aren't covered.
- §7 (Help), §8 (Panel content), §14 (inter-plugin services): all three **implemented** — see the
  Phase 2 and Phase 3 status below.

**Phase 2: implemented** (2026-09-30), per the §7 design above, with no deviations. New
`kMcPluginCapHelp` capability bit and `McPlugin_GetHelpMarkdownFn` typedef (`McPluginAbi.h`);
`PluginManager::CollectHelpContent()` fetches (SEH-wrapped, fresh every call) every loaded
Help-capable plugin's Markdown. New, deliberately minimal `MarkdownRenderer.h/.cpp` (headings,
bullet lists, plain paragraphs, inline code spans stripped rather than colored — matching the
design's own "deliberately minimal" framing exactly), pure and unit-tested
(`MarkdownRendererTests.cpp`, 13 cases). `Help.cpp`'s `ShowShortcutReference` gained an optional
`pluginContent` parameter (default empty — the screen is byte-for-byte unchanged from Phase 1
whenever no Help-capable plugin is loaded) and appends a new "Plugins" section, one sub-heading
per plugin (its own `name`) followed by its rendered Markdown, after the built-in categories —
satisfying "same rendering mechanism as the main application help" by living inside
`ShowShortcutReference`'s own scroll view rather than a second screen, exactly as designed.
`PLUG-011` promoted to `IN-MVP`/Implemented in the spec (§26); no Postponement Register entry
needed (a promotion, not a demotion). Full test suite: 413 cases, 11,756 checks, 0 failed (13
new). Full interactive in-app verification (a Help-capable plugin's content actually appearing on
the F1 screen) remains outstanding, the same standing no-interactive-console-capture limitation
noted for Phase 1.

**Phase 3: implemented** (2026-09-30), per the §8/§14 design above, with a few deliberate,
documented simplifications called out below. `PLUG-012`/`013` promoted to `IN-MVP`/Implemented in
the spec (§26); no Postponement Register entry needed (a promotion, not a demotion). FEA-0002 is
now fully delivered across all three phases.

- **§8 (Panel content):** `IPanelContent` landed exactly as designed in `Panel.h`
  (`Draw`/`OnKey`/`OnMouseClick`, pure virtual, no plugin/DLL dependency in `Panel.h` itself);
  `RequestPanelContent(side, title)`/`ReleasePanelContent(side)` and a `McPanelDrawText`-shaped
  primitive (`McHost_PanelDrawTextFn`) landed as designed. Three deliberate simplifications beyond
  the original design text: (1) the panel's header line shows the plugin's own `title` in place of
  the real path, with the volume line left blank, rather than the design's more open-ended framing
  — a concrete, minimal choice for "content/input only" without inventing a second header layout;
  (2) colors are a small, four-role named palette (`McPanelColor`: Normal/Highlight/Section/
  Warning) rather than exposing every one of `McThemeSnapshot`'s ~13 named slots — a panel-content
  plugin is drawing its own content, not reproducing the built-in listing's exact palette, so the
  smaller set was judged sufficient and easier to document; (3) key routing forwards *every* key
  except `Tab`/`F10` to the plugin while its content is installed (including the panel's own
  Left/Right history keys) rather than a layered "offer to content first, fall back to built-in"
  arrangement — `IPanelContent::OnKey` returns `void` in the design's own interface, giving no
  "handled" signal to fall back on, so a full takeover of that one panel's body-level input is the
  more literal reading of "renders through it instead of Panel's own Entries()/cursor," not a
  smaller deviation from it. Mouse support is a plain left-press/double-click forward (via the
  existing, tested `HitTestPanel`) — drag and wheel are not forwarded to plugin content, since the
  built-in drag-select/wheel-scroll code paths have no plugin-content equivalent to translate into
  and the request's own wording ("draw and handle user interaction") doesn't specifically ask for
  either.
- **§14 (Inter-plugin services):** `PublishService`/`QueryService` and the host-mediated
  `std::map<std::wstring, ServiceEntry>` registry landed exactly as designed — opaque
  `void* interfacePtr`, plain integer version `>=` check, automatic unpublish at the owning
  plugin's unload.
- No new automated test coverage this phase — every new `PluginManager` method here is real
  Win32/Console/SEH-dependent code (drawing into a live `Console&`, dispatching into a plugin's
  DLL export), matching `Elevation.h`'s own `RunElevated` precedent already established for Phase
  1/2's equivalent code; there was no new *pure* decision logic to extract into
  `PluginManagerLogic.cpp` this phase the way §6/§12's menu-merge/shortcut-conflict logic was.
  Full test suite: 413 cases, 11,756 checks, 0 failed (unchanged from Phase 2). Full interactive
  in-app verification (a panel-content plugin actually drawing into a live panel, a service query
  actually resolving between two loaded plugins) remains outstanding, the same standing
  no-interactive-console-capture limitation noted for Phase 1/2.

See `3_Reports/ChangeLogs/changelog_2026-09-30.md`'s FEA-0002 entries for the full file list and
test counts.
