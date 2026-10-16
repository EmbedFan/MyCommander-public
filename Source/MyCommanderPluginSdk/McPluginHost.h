#pragma once

// FEA-0002, Phase 1: McPluginHost -- the function-pointer struct the host
// hands every plugin once, at McPlugin_Init, and which stays valid for
// that plugin's entire remaining lifetime. Every function pointer here is
// already bound to the calling plugin's own identity by the host (a
// plugin never passes its own id back in) -- a plugin cannot claim to *be*
// a different plugin by forging an id argument, because there is no id
// argument to forge.
//
// Phase 1 covers dialogs (§9), logging (§10), theme (§11), shortcuts
// (§12), menu integration (§6), the SimulateKey command-execution
// primitive (§13), and read-only application-state queries (§13). Panel
// content (§8) and the inter-plugin service registry (§14) are Phase 3 --
// deliberately not declared here yet; those additions, when they land,
// append new fields to the end of this struct under a minor version bump
// (MC_PLUGIN_API_VERSION_MINOR), never reorder or remove what's here.

#include "McFileSystem.h"
#include "McPluginAbi.h"
#include "McPluginTypes.h"

#ifdef __cplusplus
extern "C" {
#endif

// #9 Dialogs -- thin wrappers around Dialog.h's own Console-only,
// app-state-free functions. `lines`/`options` are plain C arrays, length
// given explicitly (no null-termination convention, to keep an empty list
// unambiguous). Returns the chosen option's index, or -1 if the user
// pressed Esc -- identical contract to Dialog.h's own ShowChoiceDialog.
typedef int (__stdcall* McHost_ShowChoiceFn)(const wchar_t* title, const wchar_t* const* lines, int lineCount,
                                              const McDialogOption* options, int optionCount);
// Dismissed by any keypress or click, like Dialog.h's own ShowMessage --
// no return value, nothing to choose.
typedef void (__stdcall* McHost_ShowMessageFn)(const wchar_t* title, const wchar_t* const* lines, int lineCount);
// Returns false if the user pressed Esc (outBuffer left untouched);
// true and the edited text copied into outBuffer (truncated to
// outBufferSize - 1 characters plus a null terminator, never overrun)
// otherwise.
typedef bool (__stdcall* McHost_ShowTextPromptFn)(const wchar_t* title, const wchar_t* initialText,
                                                   wchar_t* outBuffer, int outBufferSize);

// #10 Logging -- the host prefixes every line with this plugin's own id
// before handing it to the real Logger, so a log file reader can always
// tell which plugin produced which line without the plugin having to
// remember to do that itself.
typedef void (__stdcall* McHost_LogFn)(McLogLevel level, const wchar_t* message);

// #11 Theme -- `out` must point at a caller-owned McThemeSnapshot; the
// host fills it in and returns. To be notified of *changes* (not just to
// read the current theme once), a plugin registers an optional callback
// at Init time via its own exported symbol convention documented in
// DEVELOPER_GUIDE.md's Theme chapter, called by the host every time
// ApplyThemePalette (Theme.h) runs again after startup.
typedef void (__stdcall* McHost_GetThemeFn)(McThemeSnapshot* out);

// #12 Shortcuts -- returns false (nothing registered) on a conflict
// against either a built-in shortcut or one already held by another
// plugin. UnregisterShortcut is available for a plugin that wants to
// release a shortcut before its own Shutdown (Shutdown itself always
// releases every remaining one automatically, per §5).
typedef bool (__stdcall* McHost_RegisterShortcutFn)(const McShortcutSpec* spec);
typedef void (__stdcall* McHost_UnregisterShortcutFn)(uint32_t commandId);

// #6 Menu integration. `topMenuMnemonic` selects which of the six built-in
// top menus (File/Selection/Commands/Network/View/Settings) receives the
// new item, matched the same case-insensitive way MenuBar.h's own
// FindTopMenuByMnemonic already works; an unrecognized mnemonic is a
// no-op (returns false, nothing added) rather than silently choosing an
// arbitrary menu. `itemMnemonic` need only be unique within the target
// menu (MenuBar.h's own existing per-menu mnemonic scoping). Returns the
// commandId's own success/failure, not a handle -- RemoveMenuItem and
// SetMenuItemEnabled both take the same commandId back.
typedef bool (__stdcall* McHost_AddMenuItemFn)(wchar_t topMenuMnemonic, const wchar_t* label, wchar_t itemMnemonic,
                                                uint32_t commandId, McMenuPlacement placement,
                                                wchar_t afterItemMnemonic);
typedef void (__stdcall* McHost_RemoveMenuItemFn)(uint32_t commandId);
typedef void (__stdcall* McHost_SetMenuItemEnabledFn)(uint32_t commandId, bool enabled);

// #13 Talking to the host. SimulateKey replays a key press through
// exactly the same synthetic-input mechanism IS-0003's hint-bar clicks and
// FEA-0005's SimulatedKey menu items already use -- "execute a supported
// application command" is, concretely, "press the key that already does
// it." GetActivePanelPath/GetSelectedNames are read-only application-state
// queries; GetSelectedNames follows Panel::SelectionOrCursor's own
// convention (the selection, or just the cursor entry if nothing is
// selected) since that's the convention every built-in file operation
// already follows and a plugin command should feel consistent with them.
// Both write into a caller-owned buffer/array the same truncate-safely
// convention as ShowTextPrompt above; GetSelectedNames returns the number
// of names available (which may exceed outCount if the buffer was too
// small -- the plugin can re-call with a bigger buffer if it cares).
typedef void (__stdcall* McHost_SimulateKeyFn)(uint16_t virtualKeyCode, bool ctrl, bool shift, bool alt);
typedef void (__stdcall* McHost_GetActivePanelPathFn)(wchar_t* outBuffer, int outBufferSize);
typedef int (__stdcall* McHost_GetSelectedNamesFn)(wchar_t** outNames, int outCount, int maxNameLength);

// #8 Panel content (Phase 3). Requires kMcPluginCapPanel and all three of
// McPlugin_OnPanelDraw/OnPanelKey/OnPanelClick exported -- RequestPanelContent
// returns false without installing anything if that capability/export set
// is missing, or if `side` is already owned by another plugin (only one
// plugin's content may occupy a given side at a time). `title` replaces
// that panel's own path in its header line for as long as content is
// installed -- the real Panel path/history underneath is untouched (§8's
// own "content/input only", not a full alternate Panel implementation),
// just not shown while content covers it. ReleasePanelContent reverts that
// panel to its normal filesystem-browsing display; every side a plugin
// still owns is released automatically at Shutdown regardless, the same
// host-owns-cleanup convention as menu items and shortcuts (§5).
typedef bool (__stdcall* McHost_RequestPanelContentFn)(McPanelSide side, const wchar_t* title);
typedef void (__stdcall* McHost_ReleasePanelContentFn)(McPanelSide side);
// Callable only from inside this plugin's own McPlugin_OnPanelDraw for the
// same `side` (a call at any other time is a silent no-op -- there is no
// live drawing surface to target outside that one call). `x`/`y` are
// relative to the content area's own top-left corner, as handed to
// OnPanelDraw; text landing outside the area's width/height is clipped,
// never an error.
typedef void (__stdcall* McHost_PanelDrawTextFn)(McPanelSide side, int16_t x, int16_t y, const wchar_t* text,
                                                  McPanelColor color);

// #14 Inter-plugin services (Phase 3). `interfacePtr` is opaque to the
// host -- two plugins agreeing on what it actually points to is a contract
// between them, not something the host parses; the host only tracks
// whether a name exists and at what version. PublishService returns false
// (nothing published) if `serviceName` is already published by another
// still-loaded plugin. QueryService returns nullptr if no plugin has
// published that name, or if the published `interfaceVersion` is lower
// than `requestedVersion` -- a querying plugin must always be prepared for
// nullptr, including from a service that answered non-null a moment ago
// (its owner may have unloaded since), since every service a plugin
// published is automatically unpublished at that plugin's own Shutdown, the
// same host-owns-cleanup convention as menu items/shortcuts/panel content.
typedef bool (__stdcall* McHost_PublishServiceFn)(const wchar_t* serviceName, void* interfacePtr,
                                                   uint32_t interfaceVersion);
typedef void* (__stdcall* McHost_QueryServiceFn)(const wchar_t* serviceName, uint32_t requestedVersion);

// FEA-0007, §1/§3: a generic multi-field options dialog (ShowForm) plus
// side-scoped panel queries/refresh -- none of this is ZIP-specific, any
// future plugin needing more than ShowChoice/ShowTextPrompt's single
// concern, or needing to address a panel other than "whichever is active,"
// can use these.
//
// ShowForm returns false if the user pressed Esc (fields left however the
// user had them edited up to that point -- the caller should not apply
// them); true if confirmed (Enter), with every field's value updated in
// place in the same array the caller passed in.
typedef bool (__stdcall* McHost_ShowFormFn)(const wchar_t* title, McFormField* fields, int fieldCount);

// Unlike GetActivePanelPath/GetSelectedNames (which only ever describe
// *the currently active* panel), these address a specific side regardless
// of which one is active -- needed once a plugin's own content occupies
// one panel (Phase 3) and it needs to read or refresh the *other*, real
// one. GetActiveSide tells a plugin which side is currently active, so it
// can compute "the other panel" from a side-scoped query; thin wrappers
// over the same Panel::Path()/SelectionOrCursor()/Refresh() the existing
// active-panel versions already call, just addressed by side instead of
// implicitly "whichever is active."
typedef McPanelSide (__stdcall* McHost_GetActiveSideFn)(void);
typedef void (__stdcall* McHost_GetPanelPathFn)(McPanelSide side, wchar_t* outBuffer, int outBufferSize);
typedef int (__stdcall* McHost_GetPanelSelectedNamesFn)(McPanelSide side, wchar_t** outNames, int outCount,
                                                         int maxNameLength);
// After a plugin writes real files into a panel's own directory (e.g.
// extracting an archive into it), that panel's own entries are stale until
// something re-lists it -- normally only ever triggered by the host's own
// navigation code. Lets a plugin ask for that refresh explicitly rather
// than the user needing to manually leave and re-enter the directory.
typedef void (__stdcall* McHost_RefreshPanelFn)(McPanelSide side);

struct McPluginHost {
    uint32_t structSize;

    McHost_ShowChoiceFn ShowChoice;
    McHost_ShowMessageFn ShowMessage;
    McHost_ShowTextPromptFn ShowTextPrompt;

    McHost_LogFn Log;

    McHost_GetThemeFn GetTheme;

    McHost_RegisterShortcutFn RegisterShortcut;
    McHost_UnregisterShortcutFn UnregisterShortcut;

    McHost_AddMenuItemFn AddMenuItem;
    McHost_RemoveMenuItemFn RemoveMenuItem;
    McHost_SetMenuItemEnabledFn SetMenuItemEnabled;

    McHost_SimulateKeyFn SimulateKey;
    McHost_GetActivePanelPathFn GetActivePanelPath;
    McHost_GetSelectedNamesFn GetSelectedNames;

    // Phase 2 (§7) added no McPluginHost field -- McPlugin_GetHelpMarkdown is
    // a plugin export the host calls, not a host function handed to the
    // plugin. Phase 3 (§8/§14) appended below, MC_PLUGIN_API_VERSION_MINOR
    // bumped accordingly -- everything above this line is unchanged from
    // Phase 1/2, so an older plugin built against either still works
    // unmodified against this host (minor-version backward compatibility,
    // §16).
    McHost_RequestPanelContentFn RequestPanelContent;
    McHost_ReleasePanelContentFn ReleasePanelContent;
    McHost_PanelDrawTextFn PanelDrawText;

    McHost_PublishServiceFn PublishService;
    McHost_QueryServiceFn QueryService;

    // FEA-0007 (§1/§3) appended below -- MC_PLUGIN_API_VERSION_MINOR bumped
    // accordingly; everything above this line is unchanged from Phase 1-3,
    // so an older plugin built against any of them still works unmodified
    // against this host (minor-version backward compatibility, §16).
    McHost_ShowFormFn ShowForm;
    McHost_GetActiveSideFn GetActiveSide;
    McHost_GetPanelPathFn GetPanelPath;
    McHost_GetPanelSelectedNamesFn GetPanelSelectedNames;
    McHost_RefreshPanelFn RefreshPanel;
    // API 1.5: opens the host's modal text viewer on a plugin-owned file.
    void (__stdcall* ShowFileViewer)(const wchar_t* filePath);
    // API 1.6: synchronous operation progress. total == 0 means unknown.
    // Updates are UI-thread calls; animation continues without updates.
    // Close before returning from the operation. Dialogs pause animation.
    bool (__stdcall* BeginProgress)(const wchar_t* title, uint64_t total);
    void (__stdcall* UpdateProgress)(uint64_t completed, uint64_t total, const wchar_t* message);
    void (__stdcall* EndProgress)(void);
    // API 1.7 (IS-0010): drains pending input the same way the host's own
    // Copy/Move/Delete progress loop does, applying any resize seen along
    // the way, and reports whether Esc was among it. A plugin running a
    // long per-item loop under BeginProgress/UpdateProgress/EndProgress
    // calls this once per item (the same shape as FileOps.h's own
    // CancelPoll callback) so Esc actually interrupts the loop instead of
    // sitting unconsumed in the input buffer until the operation finishes.
    bool (__stdcall* PollCancel)(void);
    // API 1.11 (FOP-015): the explicit pause point. The user can pause a running
    // operation with P (or Pause/Break); PollCancel never blocks, so a plugin
    // that supports pausing calls this at SAFE points only -- between items, or
    // between transfer chunks when no network wait is in progress -- never
    // inside a bounded wait. It blocks while the operation is paused, keeping
    // the host's progress box responsive (the box reads "[paused]"), and
    // returns true to continue or false if the user cancelled (Esc) while it
    // waited. Plugins built against API <= 1.10 never call it and simply can't
    // be paused: the host ignores P while one of them is working. Check
    // `host->cbSize > offsetof(McPluginHost, WaitWhilePaused)` before calling
    // if the plugin may run on an older host.
    bool (__stdcall* WaitWhilePaused)(void);

    // API 1.12 (EXT-002): file-system providers, see McFileSystem.h. Needs
    // kMcPluginCapFileSystem. OpenProvider shows `provider` in panel `side`
    // at `startPath` (NULL = its rootPath); `returnSelectName` (may be NULL)
    // is selected in the local folder when the panel leaves it again (e.g.
    // the archive's own file name). On true the host owns the table: it
    // copies it and calls its Close exactly once later. On false (invalid
    // table, or the panel refused to leave where it is -- e.g. the user
    // cancelled saving a modified archive) nothing was taken over and the
    // plugin must free its context itself.
    bool (__stdcall* OpenProvider)(McPanelSide side, const McFsProvider* provider, const wchar_t* startPath,
                                   const wchar_t* returnSelectName);
    // Leaves the provider shown on `side`, if THIS plugin opened it (asks to
    // save pending changes first, like ".."). False if the user cancelled.
    bool (__stdcall* CloseProvider)(McPanelSide side);
    // The `context` of the provider THIS plugin has open on `side`, or NULL
    // (no provider there, or another plugin's). For commands acting on it.
    void* (__stdcall* GetPanelProvider)(McPanelSide side);
    // The current folder inside that provider (truncate-safe); false (buffer
    // untouched) when GetPanelProvider would return NULL.
    bool (__stdcall* GetPanelProviderPath)(McPanelSide side, wchar_t* outBuffer, int outBufferSize);
    // Whether panel `side` shows ANY provider right now (an archive, a server,
    // ... -- this plugin's or another's). GetPanelPath/GetActivePanelPath and
    // the selected names then don't describe a local folder the user can see
    // (GetPanelPath keeps answering with the local folder underneath), so a
    // command working on local files should refuse rather than act there.
    bool (__stdcall* IsPanelInProvider)(McPanelSide side);

    // API 1.13 (FEA-0010): makes panel `side` show the folder `path` (a drive path, a UNC
    // path such as \server\share, any folder the user could type into the command line),
    // like a "go to this folder" from the keyboard: history is kept, a folder that cannot
    // be listed shows the usual banner. Returns false, and changes nothing, when `path` is
    // NULL/empty, or the panel currently shows plugin content or a provider (leave that
    // first). Check `host->structSize > offsetof(McPluginHost, NavigatePanel)` before
    // calling if the plugin may run on an older host.
    bool (__stdcall* NavigatePanel)(McPanelSide side, const wchar_t* path);
};

#ifdef __cplusplus
}  // extern "C"
#endif
