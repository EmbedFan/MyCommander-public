#pragma once

// FEA-0002 (MC Plugin), Phase 1: the host-side plugin manager -- discovery,
// identification, loading, lifecycle, the menu/shortcut registries, and
// SEH-wrapped dispatch into plugin code. See
// Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md for the plugin-author-
// facing side of this same boundary, and AIPrompt/features/FEA-0002-plan.md
// for the full design this implements.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "../MyCommanderPluginSdk/McPluginAbi.h"
#include "../MyCommanderPluginSdk/McPluginHost.h"
#include "../MyCommanderPluginSdk/McPluginTypes.h"
#include "Console.h"
#include "Help.h"
#include "KeyMap.h"
#include "Log.h"
#include "MenuBar.h"
#include "Panel.h"
#include "Theme.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

namespace mc {

// Pure, no DLL/Win32 dependency -- see McPluginAbi.h's own version-macro
// comment for the exact compatibility rule this implements.
bool IsPluginApiCompatible(uint32_t hostMajor, uint32_t hostMinor, uint32_t pluginMajor, uint32_t pluginMinor);

// One plugin-registered menu item, tracked in PluginManager's own overlay
// -- kept separate from MenuBar.h's built-in, compile-time
// `std::vector<TopMenu>` so a plugin can never corrupt or shadow a
// built-in item; rendering/dispatch always merges the two
// (MergePluginMenuItems) rather than mutating BuildMenus()'s own static
// data.
struct PluginMenuEntry {
    std::wstring pluginId;
    wchar_t topMenuMnemonic = 0;
    std::wstring label;
    wchar_t itemMnemonic = 0;
    uint32_t commandId = 0;
    bool enabled = true;
};

// Merges `overlay` into `builtins`, producing a fresh `vector<TopMenu>`.
// An overlay entry whose `topMenuMnemonic` doesn't match any built-in top
// menu is silently skipped -- a plugin can only extend an existing top
// menu, not invent a seventh one (the request's own "extend," not
// "replace," wording). A disabled entry is left out of the merged result
// entirely rather than rendered greyed-out, since `MenuItem` has no
// "visible but disabled" rendering state today and adding one would be a
// separate, larger change to `MenuBar.h`'s own drawing code than this
// feature asks for -- functionally equivalent either way (the item can't
// be selected), a deliberate documented simplification. Pure, no
// `PluginManager` dependency, so directly unit-testable against a
// hand-built `builtins` fixture rather than the real `BuildMenus()`.
std::vector<TopMenu> MergePluginMenuItems(const std::vector<TopMenu>& builtins,
                                          const std::vector<PluginMenuEntry>& overlay);

// One keyboard shortcut, either built-in or plugin-registered -- the key
// type for both `BuiltinShortcutInventory()` and `PluginManager`'s own
// live shortcut registry.
struct PluginShortcutKey {
    WORD virtualKeyCode = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;

    bool operator<(const PluginShortcutKey& other) const {
        if (virtualKeyCode != other.virtualKeyCode) return virtualKeyCode < other.virtualKeyCode;
        if (ctrl != other.ctrl) return ctrl < other.ctrl;
        if (shift != other.shift) return shift < other.shift;
        return alt < other.alt;
    }
    bool operator==(const PluginShortcutKey& other) const {
        return virtualKeyCode == other.virtualKeyCode && ctrl == other.ctrl && shift == other.shift &&
              alt == other.alt;
    }
};

// Whether `key` collides with any entry in `builtinKeys` -- pure,
// unit-testable against a hand-built list. `PluginManager`'s own
// conflict check additionally consults other plugins' live registrations
// via its own `std::map`, which is itself already a lookup, not
// duplicated here as a second pure function.
bool ConflictsWithBuiltinShortcut(const PluginShortcutKey& key, const std::vector<PluginShortcutKey>& builtinKeys);

// The fixed inventory of every VK+modifier combination `wmain`'s own
// key-dispatch switch and FEA-0005's menu-mnemonic dispatch already bind
// to something -- hand-maintained, since there is no other single source
// of truth for "every key this app already uses" short of parsing the
// switch itself. A change to `main.cpp`'s own key bindings needs the same
// change made here; not otherwise enforced. Deliberately covers every
// function-key/Ctrl-letter/menu-mnemonic binding (the combinations a
// plugin author would plausibly reach for) rather than every bare
// navigation key (Tab/arrows/Home/End/Page Up/Down, which take no
// modifier at all and are in no realistic sense available for a plugin to
// claim regardless of whether this list enumerates them individually).
const std::vector<PluginShortcutKey>& BuiltinShortcutInventory();

// KEY-005: the same inventory under a user key map -- every default key a
// command still has, plus every key the user bound, minus the defaults the
// user freed (those become available to plugins).
std::vector<PluginShortcutKey> BuiltinShortcutInventory(const KeyMap& keyMap);

// One successfully loaded, active plugin's own state -- the host's
// bookkeeping, never exposed to other plugins.
struct LoadedPlugin {
    std::wstring id;
    std::wstring name;
    std::wstring version;  // IS-0011: McPluginMetadata::version, surfaced on the F1 help page
    HMODULE module = nullptr;
    McPluginState state = kMcPluginDiscovered;
    McPlugin_ShutdownFn shutdownFn = nullptr;
    McPlugin_OnMenuActionFn onMenuActionFn = nullptr;
    McPlugin_OnShortcutFn onShortcutFn = nullptr;
    McPlugin_OnThemeChangedFn onThemeChangedFn = nullptr;
    McPlugin_GetHelpMarkdownFn getHelpMarkdownFn = nullptr;
    // Phase 3, §8: all three set together, only if kMcPluginCapPanel was
    // declared *and* all three exports were present at load time (a
    // partial set, like Menu/Shortcuts/Help's own single-export
    // requirement, is a load-time warning, plugin still activates, just
    // never able to successfully RequestPanelContent).
    McPlugin_OnPanelDrawFn onPanelDrawFn = nullptr;
    McPlugin_OnPanelKeyFn onPanelKeyFn = nullptr;
    McPlugin_OnPanelClickFn onPanelClickFn = nullptr;
    McPlugin_OnPanelMouseFn onPanelMouseFn = nullptr;
    McPlugin_CanHandleFileFn canHandleFileFn = nullptr;
    McPlugin_EnterArchiveFn enterArchiveFn = nullptr;
    McPlugin_OnReceiveFilesFn onReceiveFilesFn = nullptr;  // optional, see McPluginAbi.h (API 1.8)
    // API 1.10 (EXT-003): set together only if kMcPluginCapViewer was declared
    // and both exports exist; getViewerNameFn is optional (the plugin name is used).
    McPlugin_CanViewFileFn canViewFileFn = nullptr;
    McPlugin_RenderViewFn renderViewFn = nullptr;
    McPlugin_GetViewerNameFn getViewerNameFn = nullptr;
};

// --- EXT-003: viewer plugins (pure parts, PluginManagerLogic.cpp) -----------

// One loaded viewer plugin that claims a file (FindViewers).
struct ViewerCandidate {
    std::wstring pluginId;
    std::wstring name;  // McPlugin_GetViewerName, else the plugin's name
    McViewClaim claim = kMcViewNo;
};

// What F3 opens (EXT-003 D3): the user's file-actions.conf view action
// always wins; then the first viewer plugin claiming kMcViewDefault (its
// index in `candidates` goes to `pluginIndex`); then the built-in viewer.
// kMcViewOffer never becomes the default -- it's only listed in Shift+F3.
enum class ViewRoute { FileAction, Plugin, BuiltIn };
ViewRoute ChooseViewRoute(bool hasFileActionDefault, const std::vector<ViewerCandidate>& candidates, size_t& pluginIndex);

// The host side of McViewWriter: collects a plugin's rendered lines, capped
// at `maxLines` lines and `maxBytes` of text (UTF-16 code units * 2), after
// which further lines are dropped and Truncated() is set; the caller then
// appends kViewTruncatedMarker.
class ViewLineSink {
public:
    static constexpr size_t kDefaultMaxLines = 2'000'000;
    static constexpr size_t kDefaultMaxBytes = 256u * 1024u * 1024u;
    explicit ViewLineSink(size_t maxLines = kDefaultMaxLines, size_t maxBytes = kDefaultMaxBytes)
        : maxLines_(maxLines), maxBytes_(maxBytes) {}
    void Append(const wchar_t* text);
    void SetTitle(const wchar_t* title) { title_ = title ? title : L""; }
    const std::vector<std::wstring>& Lines() const { return lines_; }
    const std::wstring& Title() const { return title_; }
    bool Truncated() const { return truncated_; }
    // The lines as one UTF-16 text (CRLF-separated, with a truncation marker if needed).
    std::wstring Text() const;

private:
    size_t maxLines_;
    size_t maxBytes_;
    size_t bytes_ = 0;
    bool truncated_ = false;
    std::vector<std::wstring> lines_;
    std::wstring title_;
};
inline constexpr const wchar_t* kViewTruncatedMarker = L"... (output truncated)";

// FEA-0002 Phase 1: discovers, identifies, validates, loads, and drives
// the lifecycle of every `*.dll` in a Plugins folder; owns the menu
// overlay and shortcut registries; is the single choke point every call
// into plugin code passes through, SEH-wrapped (§15) so a structured
// exception inside a plugin's own code is caught, logged, and treated as
// that one call having failed rather than taking the host down with it --
// see the Developer Guide's own "what isolation actually means" chapter
// for the honest limits of that guarantee. Exactly one instance exists for
// the app's whole lifetime (constructed once in `wmain`), which is what
// lets the `McHost_*` C callback functions (defined in PluginManager.cpp,
// necessarily free functions to be assignable into `McPluginHost`'s plain
// function pointers) reach back into it via a single global pointer,
// attributing each call to "whichever plugin the manager is currently
// calling into" -- safe in this single-threaded app, and exactly how a
// plugin's own calls into `AddMenuItem`/`RegisterShortcut`/etc. get
// scoped to its own identity without an explicit id parameter in the ABI.
class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    // Scans `pluginsDir` for `*.dll`, identifies/validates/loads/
    // initializes every one not named in `disabledIds`, logging every
    // step via `logger`. Safe to call with a nonexistent `pluginsDir`
    // (logs nothing, no-op) -- most installs will never have created the
    // folder.
    void DiscoverAndLoad(const std::filesystem::path& pluginsDir, const std::set<std::wstring>& disabledIds,
                        Logger& logger);

    // Deactivates and unloads every still-loaded plugin, in reverse load
    // order. Safe to call more than once (a second call is a no-op) and
    // called automatically by the destructor if the owner never called it
    // explicitly -- but `main.cpp` calls it explicitly, before other
    // shutdown-only writes (Session/CommandHistory), matching this
    // project's existing "explicit, ordered, normal-exit-only" cleanup
    // convention rather than relying on destructor order.
    void Shutdown();

    // The merged (built-in + plugin overlay) menu set, rebuilt whenever
    // the overlay changes and cached here the rest of the time -- cheap to
    // call every redraw.
    const std::vector<TopMenu>& MergedMenus() const { return mergedMenus_; }

    // Runs the owning plugin's own `OnMenuAction(commandId)` for the menu
    // item identified by (`pluginId`, `commandId`) -- a no-op if no
    // currently-loaded plugin owns that pair (defensive; not reachable in
    // this single-threaded app's own normal click/keypress dispatch, since
    // an item can't be clicked after its owning plugin already unloaded
    // and removed it, but checked anyway rather than assumed).
    void DispatchMenuCommand(const std::wstring& pluginId, uint32_t commandId);

    // Checks the plugin shortcut registry for (`virtualKeyCode`, `ctrl`,
    // `shift`, `alt`); if a plugin owns it, calls that plugin's own
    // `OnShortcut` and returns true (the caller's key-dispatch switch
    // should treat the key as handled); returns false if no plugin claims
    // it, so the caller's existing fallback keeps running unchanged.
    bool DispatchShortcut(WORD virtualKeyCode, bool ctrl, bool shift, bool alt);

    // KEY-005: the live key map (owned by main.cpp). Plugin shortcuts named
    // in keys.conf (plugin:<id>/<commandId>) are registered under the user's
    // keys, and the built-in conflict check follows the map. Calling it
    // again (after a keys.conf reload) re-registers every live shortcut.
    void SetKeyMap(const KeyMap* keyMap);
    // The key a plugin command is currently bound to, for its menu item ("" if none).
    std::wstring ShortcutTextFor(const std::wstring& pluginId, uint32_t commandId) const;

    // Called once, right after `Theme.h`'s `ApplyThemePalette` updates the
    // live globals -- forwards a fresh `McThemeSnapshot` to every loaded
    // plugin that exported an `OnThemeChanged` callback. `main.cpp` wires
    // this in once via `RegisterThemeChangeListener` (`Theme.h`).
    void NotifyThemeChanged(const ThemePalette& palette);

    // Phase 2, §7: fetches (SEH-wrapped) the raw Markdown from every
    // currently-loaded kMcPluginCapHelp plugin's McPlugin_GetHelpMarkdown
    // export, fresh every call -- Help.cpp calls this once each time the F1
    // screen opens rather than caching the result, matching
    // McPlugin_GetHelpMarkdownFn's own "call this fresh" doc comment. A
    // plugin that crashes or returns null is simply omitted (logged), not
    // treated as blocking the other plugins' content.
    std::vector<PluginHelpContent> CollectHelpContent();

    // Lets the first archive-handler plugin claiming filePath take over side.
    // False deliberately leaves the caller's normal file activation intact.
    bool TryEnterAsArchive(McPanelSide side, const std::filesystem::path& filePath);

    // Lets the plugin owning panel content on `contentSide` handle an
    // incoming F5 Copy from `sourceSide` instead of the host's own ordinary
    // real-filesystem copy (API 1.8, McPlugin_OnReceiveFiles). False
    // (no owner on `contentSide`, no such export, or the plugin declined)
    // deliberately leaves the caller's normal copy path intact.
    bool TryReceiveFiles(McPanelSide contentSide, McPanelSide sourceSide);

    // EXT-003: every loaded viewer plugin claiming `fileName` (in load order).
    std::vector<ViewerCandidate> FindViewers(const std::wstring& fileName);
    // Renders `file` with plugin `pluginId` and shows the result in the
    // built-in viewer (titled by the plugin, or "<file> - <viewer>"). False
    // if the plugin isn't loaded, declined, or crashed (logged) -- the caller
    // then falls back to the built-in viewer and says so.
    bool ShowPluginView(const std::wstring& pluginId, const std::filesystem::path& file);

    bool empty() const { return plugins_.empty(); }

    // Must be called once, after `console`/`left`/`right`/`leftActive` all
    // exist and before `DiscoverAndLoad` (a plugin's own `McPlugin_Init`
    // may call straight back into a dialog/theme/panel-query host
    // function) -- gives the Host*Impl methods below something to actually
    // operate on. Stored as raw pointers/references into `wmain`'s own
    // stack-lifetime objects, safe because `PluginManager` itself is a
    // stack-lifetime object in the very same function, destroyed (and
    // `Shutdown()`ed, which stops calling any of these) before they are.
    void SetAppContext(Console& console, Panel& left, Panel& right, bool& leftActive,
                      std::function<void()> redrawBackground);

    // -- Host-callback implementation, called only from the free
    // `McHost_*Fn`-typed thunk functions in PluginManager.cpp (which is
    // what actually gets assigned into each loaded plugin's own
    // `McPluginHost` struct -- a plain C function pointer can't be a
    // member function, so the thunks reach back into the single live
    // `PluginManager` instance and call straight through to these).
    // `Host`-prefixed and grouped here, separate from the class's own
    // public API above, so the split reads as intentional rather than an
    // accident of implementation. Not meant to be called from anywhere
    // else in the host, even though C++ access control can't enforce that
    // distinction for a plain public method the way `friend` could for a
    // handful of exact free-function signatures -- a dozen individually
    // named friend declarations would be more ceremony than this
    // naming convention buys in return.
    bool HostAddMenuItem(wchar_t topMenuMnemonic, const std::wstring& label, wchar_t itemMnemonic, uint32_t commandId,
                        McMenuPlacement placement, wchar_t afterItemMnemonic);
    void HostRemoveMenuItem(uint32_t commandId);
    void HostSetMenuItemEnabled(uint32_t commandId, bool enabled);
    bool HostRegisterShortcut(const McShortcutSpec& spec);
    void HostUnregisterShortcut(uint32_t commandId);
    void HostLog(McLogLevel level, const std::wstring& message);
    const std::wstring& CurrentPluginId() const { return currentPluginId_; }

    // #9/#11/#13's own host-callback implementations -- thin wrappers
    // around Dialog.h/Theme.h/Panel.h, using the context SetAppContext
    // installed. Every `wchar_t*`/count pair follows the same
    // truncate-safely, never-overrun convention McPluginHost.h documents
    // for each.
    int HostShowChoiceImpl(const wchar_t* title, const wchar_t* const* lines, int lineCount,
                          const McDialogOption* options, int optionCount);
    void HostShowMessageImpl(const wchar_t* title, const wchar_t* const* lines, int lineCount);
    bool HostShowTextPromptImpl(const wchar_t* title, const wchar_t* initialText, wchar_t* outBuffer,
                               int outBufferSize);
    static void HostGetThemeImpl(McThemeSnapshot* out);
    void HostSimulateKeyImpl(uint16_t virtualKeyCode, bool ctrl, bool shift, bool alt);
    void HostGetActivePanelPathImpl(wchar_t* outBuffer, int outBufferSize) const;
    int HostGetSelectedNamesImpl(wchar_t** outNames, int outCount, int maxNameLength) const;

    // #8/#14 (Phase 3) host-callback implementations.
    bool HostRequestPanelContent(McPanelSide side, const wchar_t* title);
    void HostReleasePanelContent(McPanelSide side);
    void HostPanelDrawText(McPanelSide side, int16_t x, int16_t y, const wchar_t* text, McPanelColor color);
    bool HostPublishService(const std::wstring& serviceName, void* interfacePtr, uint32_t interfaceVersion);
    void* HostQueryService(const std::wstring& serviceName, uint32_t requestedVersion);

    // FEA-0007, §1/§3: a generic multi-field options dialog plus side-scoped
    // panel queries/refresh -- thin wrappers over Dialog.h's ShowFormDialog
    // and Panel::Path()/SelectionOrCursor()/Refresh(), addressed by side
    // instead of implicitly "whichever is active."
    bool HostShowForm(const wchar_t* title, McFormField* fields, int fieldCount);
    McPanelSide HostGetActiveSide() const;
    void HostGetPanelPath(McPanelSide side, wchar_t* outBuffer, int outBufferSize);
    int HostGetPanelSelectedNames(McPanelSide side, wchar_t** outNames, int outCount, int maxNameLength);
    void HostRefreshPanel(McPanelSide side);
    void HostShowFileViewer(const wchar_t* path);
    bool HostBeginProgress(const wchar_t* title, uint64_t total);
    void HostUpdateProgress(uint64_t completed, uint64_t total, const wchar_t* message);
    void HostEndProgress();
    bool HostPollCancel();

private:
    // Defined out-of-line in PluginManager.cpp as `PluginManager::
    // PanelContentAdapter` -- a nested class, so it can reach this class's
    // private members (currentPluginId_, currentPanelDraw_, logger_)
    // directly, the same "no extra public surface just for one internal
    // helper" reasoning as the Host-prefixed methods' own doc comment
    // above, without a separate `friend` declaration.
    class PanelContentAdapter;

    void RebuildMergedMenus();
    // Finds a loaded, active plugin by id, or nullptr.
    LoadedPlugin* FindPlugin(const std::wstring& id);
    // leftPanel_/rightPanel_ by side, or nullptr if SetAppContext hasn't run yet.
    Panel* PanelForSide(McPanelSide side);
    // Releases whichever side(s) `pluginId` currently owns panel content on,
    // and unpublishes every service it published -- shared by Shutdown()'s
    // per-plugin cleanup loop and (for panel content only) HostReleasePanelContent.
    void ReleasePanelContentOwnedBy(const std::wstring& pluginId);
    void UnpublishServicesOwnedBy(const std::wstring& pluginId);

    Console* console_ = nullptr;
    std::unique_ptr<class AnimatedProgressDialog> progressDialog_;
    std::wstring progressOwner_;
    Panel* leftPanel_ = nullptr;
    Panel* rightPanel_ = nullptr;
    bool* leftActivePtr_ = nullptr;
    std::function<void()> redrawBackground_;

    std::vector<TopMenu> builtinMenus_;
    std::vector<PluginMenuEntry> menuOverlay_;
    std::vector<TopMenu> mergedMenus_;

    std::map<PluginShortcutKey, std::pair<std::wstring, uint32_t>> shortcutRegistry_;  // key -> (pluginId, commandId)
    // KEY-005: each live plugin shortcut's own requested key, kept so a
    // keys.conf reload can re-bind it.
    std::map<std::pair<std::wstring, uint32_t>, PluginShortcutKey> requestedShortcuts_;
    const KeyMap* keyMap_ = nullptr;
    bool BindPluginShortcut(const std::wstring& pluginId, uint32_t commandId, const PluginShortcutKey& requested);

    std::vector<std::unique_ptr<LoadedPlugin>> plugins_;

    Logger* logger_ = nullptr;         // valid only during DiscoverAndLoad/Shutdown/dispatch calls
    std::wstring currentPluginId_;     // whichever plugin the manager is currently calling into

    // Phase 3, §8: indexed by McPanelSide. `panelAdapters_[side]` is the
    // concrete IPanelContent (defined in PluginManager.cpp, never named
    // here) installed on `leftPanel_`/`rightPanel_` via Panel::SetContent
    // -- PluginManager owns it, Panel only holds a non-owning pointer.
    // `panelContentOwner_[side]` is that side's owning plugin id, or empty
    // if unowned.
    std::unique_ptr<IPanelContent> panelAdapters_[2];
    std::wstring panelContentOwner_[2];

    // Set only for the duration of a McPlugin_OnPanelDraw call, so
    // HostPanelDrawText knows where (which Console, which absolute
    // offset/clip region) to translate a plugin's relative-coordinate draw
    // call into -- cleared immediately after that call returns.
    struct PanelDrawContext {
        bool valid = false;
        Console* console = nullptr;
        SHORT x = 0, y = 0, width = 0, height = 0;
        McPanelSide side = kMcPanelLeft;
    };
    PanelDrawContext currentPanelDraw_;

    // Phase 3, §14: host-mediated inter-plugin service registry --
    // `interfacePtr` is opaque to the host (McPluginHost.h's own doc
    // comment on McHost_PublishServiceFn explains why).
    struct ServiceEntry {
        std::wstring ownerPluginId;
        void* interfacePtr = nullptr;
        uint32_t interfaceVersion = 0;
    };
    std::map<std::wstring, ServiceEntry> services_;
};

}  // namespace mc
