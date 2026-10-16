#pragma once

// FEA-0002 (MC Plugin), Phase 1: the core ABI every MC Plugin DLL and the
// MyCommander host both compile against. Deliberately plain C, not C++ --
// C++ vtables are not a stable ABI across compilers or even across
// different MSVC runtime-library configurations, so nothing that crosses
// this boundary is a C++ class or template; every type here is POD, every
// function a plain `extern "C"` pointer. A plugin's own *implementation*
// is free to be C++ internally -- only what's exported has to stay C.
//
// See Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md for the full narrative
// walkthrough this header is the reference material for.

#include <stdint.h>
#include <wchar.h>
#ifndef __cplusplus
#include <stdbool.h>  // C++ has `bool` built in; a C plugin needs this for the typedefs below
#endif

#ifdef __cplusplus
extern "C" {
#endif

// Bump MAJOR for any change that isn't purely additive (removing/renaming
// a field, changing a function pointer's signature) -- the host rejects a
// plugin whose declared major version doesn't match its own exactly. Bump
// MINOR for an additive-only change (a new field appended to the end of a
// struct, a new optional export) -- the host accepts a plugin whose minor
// version is less than or equal to its own (an older-minor plugin only
// ever used fields the host still supports); a plugin declaring a *newer*
// minor than the host knows about is rejected, not guessed at.
#define MC_PLUGIN_API_VERSION_MAJOR 1
#define MC_PLUGIN_API_VERSION_MINOR 13  // 1.13: NavigatePanel (FEA-0010); 1.9: kMcFormFieldPassword (FTP-010); 1.10: viewer plugins (EXT-003); 1.11: WaitWhilePaused (FOP-015); 1.12: file-system providers (EXT-002)

// The cheap, fast first check that distinguishes an MC Plugin DLL from an
// unrelated DLL that happens to export a same-named symbol by coincidence,
// before any real parsing happens. 'M' 'C' 'P' 'L' packed big-endian into
// one uint32_t.
#define MC_PLUGIN_MAGIC 0x4D43504Cu

// One bit per optional capability a plugin declares up front in its own
// McPluginMetadata::capabilities -- lets the host know which optional
// lifecycle callbacks it's safe to look up and call without a per-call
// null check on every one. A capability bit set with no matching export
// present is a load-time validation failure (logged, plugin not
// activated), not a per-call null-pointer risk.
typedef enum McPluginCapability {
    kMcPluginCapMenu = 1 << 0,       // registers menu items, implements OnMenuAction
    kMcPluginCapShortcuts = 1 << 1,  // registers keyboard shortcuts, implements OnShortcut
    kMcPluginCapHelp = 1 << 2,       // provides help content, implements McPlugin_GetHelpMarkdown (Phase 2, §7)
    kMcPluginCapPanel = 1 << 3,      // can supply panel content, implements OnPanelDraw/OnPanelKey/OnPanelClick (Phase 3, §8)
    kMcPluginCapServices = 1 << 4,   // publishes/queries inter-plugin services via McPluginHost (Phase 3, §14) --
                                     // this bit is informational only (PublishService/QueryService need no
                                     // capability check to call, matching every other McPluginHost function), a
                                     // host logs and ignores any capability bit it doesn't recognize, rather than
                                     // rejecting the whole plugin, so a plugin built against a later SDK still
                                     // loads its earlier-Phase-compatible capabilities correctly against an
                                     // older host.
    kMcPluginCapArchiveHandler = 1 << 5,  // claims files as navigable virtual directories
    kMcPluginCapViewer = 1 << 6,          // API 1.10 (EXT-003): renders files as text for the host's viewer
    kMcPluginCapFileSystem = 1 << 7,      // API 1.12 (EXT-002): opens McFsProvider tables in host panels
                                          // (McPluginHost::OpenProvider, McFileSystem.h)
} McPluginCapability;

// Phase 3, §8: which of the two main panels a panel-content call concerns.
// Declared here (not McPluginTypes.h) since McPlugin_OnPanelDraw/Key/
// ClickFn below need it by value, and McPluginAbi.h deliberately has no
// dependency on McPluginTypes.h (McPanelColor, used only by
// McHost_PanelDrawTextFn, lives there instead since only McPluginHost.h --
// which already includes both headers -- needs it).
typedef enum McPanelSide {
    kMcPanelLeft,
    kMcPanelRight,
} McPanelSide;

// The nine-state lifecycle every plugin passes through, in order --
// exposed here mainly for documentation/logging clarity (the host drives
// this state machine internally; a plugin never sets its own state).
typedef enum McPluginState {
    kMcPluginDiscovered,
    kMcPluginValidated,
    kMcPluginLoaded,
    kMcPluginInitialized,
    kMcPluginActive,
    kMcPluginDeactivated,
    kMcPluginShutdown,
    kMcPluginUnloaded,
} McPluginState;

// §2: every MC Plugin DLL's identity and capabilities, returned by the one
// required export (McPlugin_GetInfo, below). `structSize` lets the host
// safely read only the fields it was compiled to know about even against
// a plugin built with a newer minor SDK version that appended fields past
// the end of what this host understands.
//
// Every `const wchar_t*` field's string must remain valid for the whole
// time the plugin DLL stays loaded (i.e. a string literal, or a buffer the
// plugin itself owns and never frees before McPlugin_Shutdown returns) --
// the host never copies these strings out immediately, only when it
// actually needs them (logging, the menu bar, disabled-plugin-id
// comparison).
typedef struct McPluginMetadata {
    uint32_t structSize;
    uint32_t apiVersionMajor;
    uint32_t apiVersionMinor;
    uint32_t magic;  // must equal MC_PLUGIN_MAGIC
    const wchar_t* id;           // stable, unique -- reverse-DNS-style recommended,
                                  // e.g. L"com.example.ftpplugin"
    const wchar_t* name;         // display name
    const wchar_t* description;
    const wchar_t* version;      // the plugin's own version string, free-form
    const wchar_t* author;
    uint32_t capabilities;       // bitmask of McPluginCapability
} McPluginMetadata;

// The one symbol every MC Plugin DLL must export, by exactly this name,
// undecorated (`extern "C"`), returning a pointer to a `static const`
// McPluginMetadata the plugin owns for its whole lifetime. Must have no
// side effects and must be safe to call before McPlugin_Init -- the host
// calls this purely to identify and validate the plugin, before deciding
// whether to activate it at all (§3's "identified... before activating").
//
//   extern "C" __declspec(dllexport)
//   const McPluginMetadata* __stdcall McPlugin_GetInfo(void);
typedef const McPluginMetadata*(__stdcall* McPlugin_GetInfoFn)(void);

// Forward-declared here, fully defined in McPluginHost.h (kept in a
// separate header since it depends on the dialog/log/theme/shortcut/menu
// types defined in McPluginTypes.h) -- McPluginAbi.h only needs the type
// name to declare McPlugin_InitFn's signature.
typedef struct McPluginHost McPluginHost;

// Optional. Initialization -> Activation (§5). Called once, after
// McPlugin_GetInfo has already validated successfully. `host` is a stable
// pointer valid for the plugin's entire remaining lifetime (not re-handed
// per call) -- the plugin should keep it, not assume it will be passed
// again. Returning false aborts activation for this plugin only (logged by
// the host as an initialization failure, §15); other already-active
// plugins are unaffected.
//
//   extern "C" __declspec(dllexport)
//   bool __stdcall McPlugin_Init(const McPluginHost* host);
typedef bool(__stdcall* McPlugin_InitFn)(const McPluginHost* host);

// Optional (but required in practice for any plugin holding resources).
// Deactivation -> Shutdown (§5). The host has already removed every menu
// item/shortcut/service this plugin registered *before* calling this --
// a plugin does not need to (and cannot) prevent that cleanup by skipping
// it here. This callback exists for the plugin's *own* resources (memory,
// handles, threads) the host has no way to know about.
//
//   extern "C" __declspec(dllexport)
//   void __stdcall McPlugin_Shutdown(void);
typedef void(__stdcall* McPlugin_ShutdownFn)(void);

// Required if kMcPluginCapMenu is set in capabilities. Called when the
// user activates a menu item this plugin registered via
// McPluginHost::AddMenuItem, with the same commandId the plugin chose at
// registration time.
typedef void(__stdcall* McPlugin_OnMenuActionFn)(uint32_t commandId);

// Required if kMcPluginCapShortcuts is set. Called when the user presses a
// key combination this plugin registered via McPluginHost::RegisterShortcut,
// with the same commandId the plugin chose at registration time.
typedef void(__stdcall* McPlugin_OnShortcutFn)(uint32_t commandId);

// §11: fully optional, no capability bit -- any loaded plugin may export
// this (looked up once at load time; simply not called again if absent).
// Called every time the live theme changes (Theme.h's ApplyThemePalette),
// including the very first time it's applied after this plugin finished
// initializing, with a fresh snapshot of the new theme -- a plugin that
// only ever needs the *current* theme, never change notifications, can
// ignore this and just call McPluginHost::GetTheme on demand instead.
typedef struct McThemeSnapshot McThemeSnapshot;  // fully defined in McPluginTypes.h
typedef void(__stdcall* McPlugin_OnThemeChangedFn)(const McThemeSnapshot* theme);

// Required if kMcPluginCapHelp is set (Phase 2, §7). Returns this plugin's
// own help content as Markdown source, rendered by the host through a
// small, deliberately minimal renderer (headings, bullet lists, plain
// paragraphs, inline code spans -- not a general CommonMark implementation;
// anything more exotic degrades to a plain paragraph line) as a new
// "Plugins" sub-section of the host's own F1 shortcut-reference screen, one
// sub-heading per loaded kMcPluginCapHelp plugin using its own `name` from
// McPluginMetadata. The returned string is UTF-16 and must remain valid
// until McPlugin_Shutdown returns (plugin-owned lifetime, same convention
// as McPluginMetadata's own string fields) -- the host calls this fresh
// every time the F1 screen opens rather than caching the result.
typedef const wchar_t*(__stdcall* McPlugin_GetHelpMarkdownFn)(void);

// Required if kMcPluginCapPanel is set (Phase 3, §8). Called only for a
// `side` this plugin currently owns (i.e. after a McHost_RequestPanelContentFn
// call for that side succeeded, and before McHost_ReleasePanelContentFn or
// this plugin's own McPlugin_Shutdown) -- panel navigation itself (the
// panel's real filesystem path/history) is untouched; these three exports
// own only that one panel's drawing and input while installed.
//
// McPlugin_OnPanelDraw is called once per redraw with the exact content
// area's width/height (in console cells) and whether this panel is
// currently the active one; the plugin draws by calling back into
// McPluginHost::PanelDrawText (with the same `side`) any number of times
// during this call, at coordinates relative to that same area's own
// top-left corner (0,0) -- there is no other way for a plugin to draw here,
// Console itself is never exposed across the ABI.
typedef void(__stdcall* McPlugin_OnPanelDrawFn)(McPanelSide side, uint16_t width, uint16_t height, bool active);
// A key pressed while this panel is active and owns content -- every key
// except Tab (switch active panel), F1 (help), and F10 (quit), which remain the host's
// own cross-panel chrome regardless of installed content.
typedef void(__stdcall* McPlugin_OnPanelKeyFn)(McPanelSide side, uint16_t virtualKeyCode, uint32_t controlKeyState);
// A left-click or double-click landing inside this panel's content area,
// `relativeX`/`relativeY` in the same coordinate space McPlugin_OnPanelDraw
// draws in.
typedef void(__stdcall* McPlugin_OnPanelClickFn)(McPanelSide side, int16_t relativeX, int16_t relativeY,
                                                  bool doubleClick);
// Optional richer input: Win32 button state, event flags, and modifiers.
typedef void(__stdcall* McPlugin_OnPanelMouseFn)(McPanelSide side, int16_t relativeX, int16_t relativeY,
                                                uint32_t buttons, uint32_t eventFlags, uint32_t controlKeyState);

// Optional (API 1.8). Called only for a `side` this plugin currently owns
// panel content on (same precondition as OnPanelDraw/OnPanelKey/OnPanelClick)
// when the host's own F5 Copy is pressed while that side is the
// *destination* and the *other* panel (`sourceSide`) is active -- without
// this export, the host falls back to its own ordinary real-filesystem copy
// into that panel's real underlying directory (Panel content replaces only
// what's drawn/how keys are handled while installed, not the panel's own
// real path underneath, so a plain real-file copy there was otherwise silent
// and easy to mistake for "copying into the content failed"). Returning
// true tells the host this plugin handled the copy itself (regardless of
// whether anything was actually added -- e.g. nothing selected is still
// "handled", just a no-op); the host then skips its own real-filesystem
// copy entirely for this keypress. Returning false (or not exporting this
// at all) leaves the host's own fallback copy unchanged -- so an older
// plugin, or one that doesn't implement this, keeps today's behavior.
typedef bool(__stdcall* McPlugin_OnReceiveFilesFn)(McPanelSide side, McPanelSide sourceSide);

// Required if kMcPluginCapArchiveHandler is set. CanHandleFile must be a
// cheap, name-only predicate; EnterArchive takes ownership of the panel by
// calling RequestPanelContent and returns whether it actually succeeded.
typedef bool(__stdcall* McPlugin_CanHandleFileFn)(const wchar_t* fileName);
typedef bool(__stdcall* McPlugin_EnterArchiveFn)(McPanelSide side, const wchar_t* filePath);

// API 1.10 (EXT-003): viewer plugins. Required if kMcPluginCapViewer is set.
// A viewer renders a file as plain text lines; the host shows them in its own
// built-in viewer (scrolling, search, hex mode, resize all come for free).
//
// McPlugin_CanViewFile must be a cheap, name-only predicate, like
// CanHandleFile. kMcViewOffer lists the viewer in Shift+F3 (the action
// chooser); kMcViewDefault also makes it F3's default for that file -- unless
// the user's own file-actions.conf names a view action, which always wins.
// Prefer kMcViewOffer unless your plugin is the obvious viewer for a format.
typedef enum McViewClaim {
    kMcViewNo = 0,
    kMcViewOffer = 1,
    kMcViewDefault = 2,
} McViewClaim;
typedef McViewClaim(__stdcall* McPlugin_CanViewFileFn)(const wchar_t* fileName);

// The viewer's short name for the chooser, e.g. L"Hex dump".
typedef const wchar_t*(__stdcall* McPlugin_GetViewerNameFn)(void);

// The host-owned sink a renderer writes into. `context` is opaque; pass it
// back unchanged. AppendLine takes one line without its newline; the host
// caps the total output (lines and bytes) and marks a truncated view.
// SetTitle optionally replaces the viewer's header (default: the file path).
typedef struct McViewWriter {
    uint32_t structSize;
    void* context;
    void(__stdcall* AppendLine)(void* context, const wchar_t* text);
    void(__stdcall* SetTitle)(void* context, const wchar_t* title);
} McViewWriter;

// Renders `filePath` through `writer`. Return true when a view was produced;
// false = declined or failed (the host then opens its built-in viewer and
// says so). A long render should call McPluginHost::PollCancel now and then.
typedef bool(__stdcall* McPlugin_RenderViewFn)(const wchar_t* filePath, const McViewWriter* writer);

#ifdef __cplusplus
}  // extern "C"
#endif
