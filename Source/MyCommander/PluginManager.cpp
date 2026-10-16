#include "PluginManager.h"

#include "Dialog.h"
#include "MenuBar.h"
#include "PathUtil.h"
#include "PauseToken.h"
#include "PluginProviderAdapter.h"
#include "Strings.h"
#include "Utf8TextFile.h"
#include "Viewer.h"

#include <algorithm>
#include <iterator>
#include <cwctype>

namespace mc {

// IsPluginApiCompatible/MergePluginMenuItems/ConflictsWithBuiltinShortcut/
// BuiltinShortcutInventory (all declared in PluginManager.h) are defined in
// PluginManagerLogic.cpp, not here -- pure, Console/Panel/Dialog-independent
// decision logic kept in its own translation unit (TST-005 precedent) so
// the test project can compile and exercise it directly without pulling in
// this file's real Dialog.cpp/Console.cpp/Panel.cpp dependency.

namespace {

// FEA-0002 Phase 1, §15: every call *into* plugin code goes through one of
// these tiny wrappers, isolated into their own leaf functions (no C++
// objects with destructors declared inside the __try itself) so MSVC's
// "cannot mix C++ object unwinding with SEH in the same function" (C2712)
// never applies here. A structured exception (access violation,
// divide-by-zero, ...) inside the call is caught and reported as that one
// call having failed -- see the Developer Guide's own chapter on what this
// isolation guarantee does and does not cover.
bool SehCallGetInfo(McPlugin_GetInfoFn fn, const McPluginMetadata** outInfo) {
    __try {
        *outInfo = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallInit(McPlugin_InitFn fn, const McPluginHost* host, bool* outResult) {
    __try {
        *outResult = fn(host);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallShutdown(McPlugin_ShutdownFn fn) {
    __try {
        fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallOnMenuAction(McPlugin_OnMenuActionFn fn, uint32_t commandId) {
    __try {
        fn(commandId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallOnShortcut(McPlugin_OnShortcutFn fn, uint32_t commandId) {
    __try {
        fn(commandId);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallOnThemeChanged(McPlugin_OnThemeChangedFn fn, const McThemeSnapshot* theme) {
    __try {
        fn(theme);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallGetHelpMarkdown(McPlugin_GetHelpMarkdownFn fn, const wchar_t** outMarkdown) {
    __try {
        *outMarkdown = fn();
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallOnPanelDraw(McPlugin_OnPanelDrawFn fn, McPanelSide side, uint16_t width, uint16_t height, bool active) {
    __try {
        fn(side, width, height, active);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallOnPanelKey(McPlugin_OnPanelKeyFn fn, McPanelSide side, uint16_t virtualKeyCode, uint32_t controlKeyState) {
    __try {
        fn(side, virtualKeyCode, controlKeyState);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}
void __stdcall ThunkShowFileViewer(const wchar_t* path);
bool SehCallOnPanelMouse(McPlugin_OnPanelMouseFn fn, McPanelSide side, int16_t x, int16_t y,
                        uint32_t buttons, uint32_t flags, uint32_t control) {
    __try { fn(side, x, y, buttons, flags, control); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SehCallOnPanelClick(McPlugin_OnPanelClickFn fn, McPanelSide side, int16_t relativeX, int16_t relativeY,
                         bool doubleClick) {
    __try {
        fn(side, relativeX, relativeY, doubleClick);
        return true;
    } __except (EXCEPTION_EXECUTE_HANDLER) {
        return false;
    }
}

bool SehCallCanHandleFile(McPlugin_CanHandleFileFn fn, const wchar_t* fileName, bool* outResult) {
    __try { *outResult = fn(fileName); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SehCallEnterArchive(McPlugin_EnterArchiveFn fn, McPanelSide side, const wchar_t* filePath, bool* outResult) {
    __try { *outResult = fn(side, filePath); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SehCallOnReceiveFiles(McPlugin_OnReceiveFilesFn fn, McPanelSide side, McPanelSide sourceSide, bool* outResult) {
    __try { *outResult = fn(side, sourceSide); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

// EXT-003: viewer plugins.
bool SehCallCanViewFile(McPlugin_CanViewFileFn fn, const wchar_t* fileName, McViewClaim* outClaim) {
    __try { *outClaim = fn(fileName); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SehCallGetViewerName(McPlugin_GetViewerNameFn fn, const wchar_t** outName) {
    __try { *outName = fn(); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

bool SehCallRenderView(McPlugin_RenderViewFn fn, const wchar_t* filePath, const McViewWriter* writer, bool* outResult) {
    __try { *outResult = fn(filePath, writer); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

void __stdcall ViewWriterAppendLine(void* context, const wchar_t* text) {
    if (context) static_cast<ViewLineSink*>(context)->Append(text);
}

void __stdcall ViewWriterSetTitle(void* context, const wchar_t* title) {
    if (context) static_cast<ViewLineSink*>(context)->SetTitle(title);
}

// FEA-0002 Phase 1: exactly one PluginManager exists for the app's whole
// lifetime (constructed once in wmain) -- this is what lets the free
// McHost_*Fn thunk functions below (which, being plain C function
// pointers, cannot be member functions or capturing lambdas) reach back
// into it.
PluginManager* g_activeManager = nullptr;

// -- Host callback thunks: the actual McHost_*Fn values installed into
// every loaded plugin's own McPluginHost. Each is a thin forward to
// PluginManager's own Host*-prefixed public method (see PluginManager.h's
// own comment on that naming convention).

int __stdcall ThunkShowChoice(const wchar_t* title, const wchar_t* const* lines, int lineCount,
                              const McDialogOption* options, int optionCount) {
    // Dialog display itself is host-side, Console-owning code -- not
    // wrapped in SEH (it never runs plugin code), consistent with only
    // wrapping calls *into* a plugin, not every host operation a plugin's
    // own call happens to trigger.
    if (!g_activeManager) return -1;
    return g_activeManager->HostShowChoiceImpl(title, lines, lineCount, options, optionCount);
}

void __stdcall ThunkShowMessage(const wchar_t* title, const wchar_t* const* lines, int lineCount) {
    if (!g_activeManager) return;
    g_activeManager->HostShowMessageImpl(title, lines, lineCount);
}

bool __stdcall ThunkShowTextPrompt(const wchar_t* title, const wchar_t* initialText, wchar_t* outBuffer,
                                   int outBufferSize) {
    if (!g_activeManager) return false;
    return g_activeManager->HostShowTextPromptImpl(title, initialText, outBuffer, outBufferSize);
}

void __stdcall ThunkLog(McLogLevel level, const wchar_t* message) {
    if (!g_activeManager || !message) return;
    g_activeManager->HostLog(level, message);
}

void __stdcall ThunkGetTheme(McThemeSnapshot* out) {
    if (!g_activeManager || !out) return;
    g_activeManager->HostGetThemeImpl(out);
}

bool __stdcall ThunkRegisterShortcut(const McShortcutSpec* spec) {
    if (!g_activeManager || !spec) return false;
    return g_activeManager->HostRegisterShortcut(*spec);
}

void __stdcall ThunkUnregisterShortcut(uint32_t commandId) {
    if (!g_activeManager) return;
    g_activeManager->HostUnregisterShortcut(commandId);
}

bool __stdcall ThunkAddMenuItem(wchar_t topMenuMnemonic, const wchar_t* label, wchar_t itemMnemonic,
                                uint32_t commandId, McMenuPlacement placement, wchar_t afterItemMnemonic) {
    if (!g_activeManager || !label) return false;
    return g_activeManager->HostAddMenuItem(topMenuMnemonic, label, itemMnemonic, commandId, placement,
                                            afterItemMnemonic);
}

void __stdcall ThunkRemoveMenuItem(uint32_t commandId) {
    if (!g_activeManager) return;
    g_activeManager->HostRemoveMenuItem(commandId);
}

void __stdcall ThunkSetMenuItemEnabled(uint32_t commandId, bool enabled) {
    if (!g_activeManager) return;
    g_activeManager->HostSetMenuItemEnabled(commandId, enabled);
}

void __stdcall ThunkSimulateKey(uint16_t virtualKeyCode, bool ctrl, bool shift, bool alt) {
    if (!g_activeManager) return;
    g_activeManager->HostSimulateKeyImpl(virtualKeyCode, ctrl, shift, alt);
}

void __stdcall ThunkGetActivePanelPath(wchar_t* outBuffer, int outBufferSize) {
    if (!g_activeManager || !outBuffer || outBufferSize <= 0) return;
    g_activeManager->HostGetActivePanelPathImpl(outBuffer, outBufferSize);
}

int __stdcall ThunkGetSelectedNames(wchar_t** outNames, int outCount, int maxNameLength) {
    if (!g_activeManager) return 0;
    return g_activeManager->HostGetSelectedNamesImpl(outNames, outCount, maxNameLength);
}

bool __stdcall ThunkRequestPanelContent(McPanelSide side, const wchar_t* title) {
    if (!g_activeManager) return false;
    return g_activeManager->HostRequestPanelContent(side, title);
}

void __stdcall ThunkReleasePanelContent(McPanelSide side) {
    if (!g_activeManager) return;
    g_activeManager->HostReleasePanelContent(side);
}

void __stdcall ThunkPanelDrawText(McPanelSide side, int16_t x, int16_t y, const wchar_t* text, McPanelColor color) {
    if (!g_activeManager) return;
    g_activeManager->HostPanelDrawText(side, x, y, text, color);
}

bool __stdcall ThunkPublishService(const wchar_t* serviceName, void* interfacePtr, uint32_t interfaceVersion) {
    if (!g_activeManager || !serviceName) return false;
    return g_activeManager->HostPublishService(serviceName, interfacePtr, interfaceVersion);
}

void* __stdcall ThunkQueryService(const wchar_t* serviceName, uint32_t requestedVersion) {
    if (!g_activeManager || !serviceName) return nullptr;
    return g_activeManager->HostQueryService(serviceName, requestedVersion);
}

bool __stdcall ThunkShowForm(const wchar_t* title, McFormField* fields, int fieldCount) {
    if (!g_activeManager || !title) return false;
    return g_activeManager->HostShowForm(title, fields, fieldCount);
}

McPanelSide __stdcall ThunkGetActiveSide(void) {
    if (!g_activeManager) return kMcPanelLeft;
    return g_activeManager->HostGetActiveSide();
}

void __stdcall ThunkGetPanelPath(McPanelSide side, wchar_t* outBuffer, int outBufferSize) {
    if (!g_activeManager || !outBuffer || outBufferSize <= 0) return;
    g_activeManager->HostGetPanelPath(side, outBuffer, outBufferSize);
}

int __stdcall ThunkGetPanelSelectedNames(McPanelSide side, wchar_t** outNames, int outCount, int maxNameLength) {
    if (!g_activeManager) return 0;
    return g_activeManager->HostGetPanelSelectedNames(side, outNames, outCount, maxNameLength);
}

void __stdcall ThunkRefreshPanel(McPanelSide side) {
    if (!g_activeManager) return;
    g_activeManager->HostRefreshPanel(side);
}
void __stdcall ThunkShowFileViewer(const wchar_t* path) {
    if (g_activeManager) g_activeManager->HostShowFileViewer(path);
}
bool __stdcall ThunkBeginProgress(const wchar_t* title, uint64_t total) {
    return g_activeManager && g_activeManager->HostBeginProgress(title, total);
}
void __stdcall ThunkUpdateProgress(uint64_t completed, uint64_t total, const wchar_t* message) {
    if (g_activeManager) g_activeManager->HostUpdateProgress(completed, total, message);
}
void __stdcall ThunkEndProgress() {
    if (g_activeManager) g_activeManager->HostEndProgress();
}
bool __stdcall ThunkPollCancel() {
    return g_activeManager && g_activeManager->HostPollCancel();
}
bool __stdcall ThunkWaitWhilePaused() {
    return !g_activeManager || g_activeManager->HostWaitWhilePaused();
}
bool __stdcall ThunkOpenProvider(McPanelSide side, const McFsProvider* provider, const wchar_t* startPath,
                                 const wchar_t* returnSelectName) {
    return g_activeManager && g_activeManager->HostOpenProvider(side, provider, startPath, returnSelectName);
}
bool __stdcall ThunkCloseProvider(McPanelSide side) {
    return g_activeManager && g_activeManager->HostCloseProvider(side);
}
void* __stdcall ThunkGetPanelProvider(McPanelSide side) {
    return g_activeManager ? g_activeManager->HostGetPanelProvider(side) : nullptr;
}
bool __stdcall ThunkIsPanelInProvider(McPanelSide side) {
    return g_activeManager && g_activeManager->HostIsPanelInProvider(side);
}
bool __stdcall ThunkNavigatePanel(McPanelSide side, const wchar_t* path) {
    return g_activeManager && g_activeManager->HostNavigatePanel(side, path);
}
bool __stdcall ThunkGetPanelProviderPath(McPanelSide side, wchar_t* outBuffer, int outBufferSize) {
    if (!g_activeManager || !outBuffer || outBufferSize <= 0) return false;
    return g_activeManager->HostGetPanelProviderPath(side, outBuffer, outBufferSize);
}

}  // namespace

// FEA-0002 Phase 3, §8: PluginManager's own concrete IPanelContent,
// forwarding Draw/OnKey/OnMouseClick to the owning plugin's exported
// OnPanelDraw/OnPanelKey/OnPanelClick (SEH-wrapped, like every other call
// into plugin code). A nested class (declared only as `class
// PanelContentAdapter;` in PluginManager.h) so it can set
// `manager_.currentPluginId_`/`currentPanelDraw_` directly -- Panel::
// Content()/PluginManager's own panelAdapters_ both hold it only as a
// plain IPanelContent*/unique_ptr<IPanelContent>, so the concrete type
// itself stays entirely private to this translation unit.
class PluginManager::PanelContentAdapter : public IPanelContent {
public:
    PanelContentAdapter(PluginManager& manager, std::wstring pluginId, McPanelSide side, McPlugin_OnPanelDrawFn drawFn,
                        McPlugin_OnPanelKeyFn keyFn, McPlugin_OnPanelClickFn clickFn, McPlugin_OnPanelMouseFn mouseFn)
        : manager_(manager), pluginId_(std::move(pluginId)), side_(side), drawFn_(drawFn), keyFn_(keyFn),
          clickFn_(clickFn), mouseFn_(mouseFn) {}

    void Draw(Console& console, SHORT x, SHORT y, SHORT width, SHORT height, bool active) override {
        const auto previousDraw = manager_.currentPanelDraw_;
        const auto previousPlugin = manager_.currentPluginId_;
        manager_.currentPanelDraw_ = {true, &console, x, y, width, height, side_};
        manager_.currentPluginId_ = pluginId_;
        bool called = SehCallOnPanelDraw(drawFn_, side_, static_cast<uint16_t>(std::max<SHORT>(0, width)),
                                        static_cast<uint16_t>(std::max<SHORT>(0, height)), active);
        manager_.currentPluginId_ = previousPlugin;
        manager_.currentPanelDraw_ = previousDraw;
        if (!called && manager_.logger_) {
            manager_.logger_->Log(LogLevel::Error, L"Plugin \"" + pluginId_ + L"\": crashed drawing its panel content");
        }
    }
    // IS-0018: the typed character isn't part of the plugin ABI
    // (McPlugin_OnPanelKey), so it's deliberately not forwarded.
    void OnKey(WORD virtualKeyCode, DWORD controlKeyState, wchar_t /*unicodeChar*/) override {
        // The callback may release and destroy this adapter. Retain everything
        // needed after it returns in local variables.
        auto* manager = &manager_;
        const auto id = pluginId_;
        const auto previousPlugin = manager->currentPluginId_;
        manager->currentPluginId_ = id;
        bool called = SehCallOnPanelKey(keyFn_, side_, virtualKeyCode, controlKeyState);
        manager->HostEndProgress();
        manager->currentPluginId_ = previousPlugin;
        if (!called && manager->logger_) {
            manager->logger_->Log(LogLevel::Error, L"Plugin \"" + id + L"\": crashed handling a panel key");
        }
    }
    void OnMouseClick(SHORT relativeX, SHORT relativeY, bool doubleClick) override {
        auto* manager = &manager_;
        const auto previousPlugin = manager->currentPluginId_;
        const auto id = pluginId_;
        manager->currentPluginId_ = id;
        bool called = SehCallOnPanelClick(clickFn_, side_, relativeX, relativeY, doubleClick);
        manager->HostEndProgress();
        manager->currentPluginId_ = previousPlugin;
        if (!called && manager->logger_) {
            manager->logger_->Log(LogLevel::Error, L"Plugin \"" + id + L"\": crashed handling a panel click");
        }
    }
    void OnMouseEvent(SHORT x, SHORT y, DWORD buttons, DWORD flags, DWORD control) override {
        if (!mouseFn_) { IPanelContent::OnMouseEvent(x, y, buttons, flags, control); return; }
        auto* manager = &manager_;
        const auto id = pluginId_;
        const auto previousPlugin = manager->currentPluginId_;
        manager->currentPluginId_ = id;
        bool called = SehCallOnPanelMouse(mouseFn_, side_, x, y, buttons, flags, control);
        manager->HostEndProgress();
        manager->currentPluginId_ = previousPlugin;
        if (!called && manager->logger_)
            manager->logger_->Log(LogLevel::Error, L"Plugin \"" + id + L"\": crashed handling panel mouse input");
    }

private:
    using PanelDrawContext = PluginManager::PanelDrawContext;
    PluginManager& manager_;
    std::wstring pluginId_;
    McPanelSide side_;
    McPlugin_OnPanelDrawFn drawFn_;
    McPlugin_OnPanelKeyFn keyFn_;
    McPlugin_OnPanelClickFn clickFn_;
    McPlugin_OnPanelMouseFn mouseFn_;
};

PluginManager::PluginManager() { g_activeManager = this; }

PluginManager::~PluginManager() {
    Shutdown();
    if (g_activeManager == this) g_activeManager = nullptr;
}

void PluginManager::RebuildMergedMenus() { mergedMenus_ = MergePluginMenuItems(builtinMenus_, menuOverlay_); }

LoadedPlugin* PluginManager::FindPlugin(const std::wstring& id) {
    auto it = std::find_if(plugins_.begin(), plugins_.end(), [&](const auto& p) { return p->id == id; });
    return it == plugins_.end() ? nullptr : it->get();
}

void PluginManager::DiscoverAndLoad(const std::filesystem::path& pluginsDir, const std::set<std::wstring>& disabledIds,
                                    Logger& logger) {
    logger_ = &logger;
    builtinMenus_ = BuildMenus();
    RebuildMergedMenus();

    std::error_code ec;
    if (!std::filesystem::exists(pluginsDir, ec) || !std::filesystem::is_directory(pluginsDir, ec)) return;

    for (const auto& entry : std::filesystem::directory_iterator(pluginsDir, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::filesystem::path dllPath = entry.path();
        if (_wcsicmp(dllPath.extension().c_str(), L".dll") != 0) continue;

        HMODULE module = LoadLibraryW(dllPath.c_str());
        if (!module) {
            logger.Log(LogLevel::Warning, L"Plugin: could not load \"" + dllPath.wstring() + L"\"");
            continue;
        }

        auto getInfoFn = reinterpret_cast<McPlugin_GetInfoFn>(GetProcAddress(module, "McPlugin_GetInfo"));
        if (!getInfoFn) {
            logger.Log(LogLevel::Warning,
                      L"Plugin: \"" + dllPath.wstring() + L"\" has no McPlugin_GetInfo export -- not an MC Plugin");
            FreeLibrary(module);
            continue;
        }

        const McPluginMetadata* info = nullptr;
        if (!SehCallGetInfo(getInfoFn, &info) || !info) {
            logger.Log(LogLevel::Warning, L"Plugin: \"" + dllPath.wstring() + L"\" crashed calling McPlugin_GetInfo");
            FreeLibrary(module);
            continue;
        }
        if (info->magic != MC_PLUGIN_MAGIC) {
            logger.Log(LogLevel::Warning, L"Plugin: \"" + dllPath.wstring() + L"\" magic mismatch -- not an MC Plugin");
            FreeLibrary(module);
            continue;
        }

        std::wstring id = info->id ? info->id : L"";
        std::wstring name = info->name ? info->name : id;
        if (id.empty()) {
            logger.Log(LogLevel::Warning, L"Plugin: \"" + dllPath.wstring() + L"\" declared an empty id -- rejected");
            FreeLibrary(module);
            continue;
        }

        if (!IsPluginApiCompatible(MC_PLUGIN_API_VERSION_MAJOR, MC_PLUGIN_API_VERSION_MINOR, info->apiVersionMajor,
                                   info->apiVersionMinor)) {
            logger.Log(LogLevel::Warning, L"Plugin \"" + id + L"\": API version " +
                                          std::to_wstring(info->apiVersionMajor) + L"." +
                                          std::to_wstring(info->apiVersionMinor) + L" incompatible with host " +
                                          std::to_wstring(MC_PLUGIN_API_VERSION_MAJOR) + L"." +
                                          std::to_wstring(MC_PLUGIN_API_VERSION_MINOR));
            FreeLibrary(module);
            continue;
        }

        if (disabledIds.count(id) != 0) {
            logger.Log(LogLevel::Info, L"Plugin \"" + id + L"\": disabled via Config::disabledPluginIds");
            FreeLibrary(module);
            continue;
        }

        auto plugin = std::make_unique<LoadedPlugin>();
        plugin->id = id;
        plugin->name = name;
        plugin->version = info->version ? info->version : L"";
        plugin->apiMinor = info->apiVersionMinor;
        plugin->module = module;
        plugin->state = kMcPluginValidated;

        if (info->capabilities & kMcPluginCapMenu) {
            plugin->onMenuActionFn =
                reinterpret_cast<McPlugin_OnMenuActionFn>(GetProcAddress(module, "McPlugin_OnMenuAction"));
            if (!plugin->onMenuActionFn) {
                logger.Log(LogLevel::Warning,
                          L"Plugin \"" + id + L"\": declared kMcPluginCapMenu but has no McPlugin_OnMenuAction export");
            }
        }
        if (info->capabilities & kMcPluginCapShortcuts) {
            plugin->onShortcutFn =
                reinterpret_cast<McPlugin_OnShortcutFn>(GetProcAddress(module, "McPlugin_OnShortcut"));
            if (!plugin->onShortcutFn) {
                logger.Log(LogLevel::Warning, L"Plugin \"" + id +
                                              L"\": declared kMcPluginCapShortcuts but has no McPlugin_OnShortcut export");
            }
        }
        if (info->capabilities & kMcPluginCapHelp) {
            plugin->getHelpMarkdownFn =
                reinterpret_cast<McPlugin_GetHelpMarkdownFn>(GetProcAddress(module, "McPlugin_GetHelpMarkdown"));
            if (!plugin->getHelpMarkdownFn) {
                logger.Log(LogLevel::Warning, L"Plugin \"" + id +
                                              L"\": declared kMcPluginCapHelp but has no McPlugin_GetHelpMarkdown export");
            }
        }
        if (info->capabilities & kMcPluginCapPanel) {
            auto drawFn = reinterpret_cast<McPlugin_OnPanelDrawFn>(GetProcAddress(module, "McPlugin_OnPanelDraw"));
            auto keyFn = reinterpret_cast<McPlugin_OnPanelKeyFn>(GetProcAddress(module, "McPlugin_OnPanelKey"));
            auto clickFn = reinterpret_cast<McPlugin_OnPanelClickFn>(GetProcAddress(module, "McPlugin_OnPanelClick"));
            if (drawFn && keyFn && clickFn) {
                plugin->onPanelDrawFn = drawFn;
                plugin->onPanelKeyFn = keyFn;
                plugin->onPanelClickFn = clickFn;
                plugin->onPanelMouseFn = reinterpret_cast<McPlugin_OnPanelMouseFn>(GetProcAddress(module, "McPlugin_OnPanelMouse"));
                // Optional (API 1.8) -- see McPluginAbi.h's own doc comment.
                plugin->onReceiveFilesFn =
                    reinterpret_cast<McPlugin_OnReceiveFilesFn>(GetProcAddress(module, "McPlugin_OnReceiveFiles"));
            } else {
                logger.Log(LogLevel::Warning, L"Plugin \"" + id + L"\": declared kMcPluginCapPanel but is missing one "
                                              L"or more of McPlugin_OnPanelDraw/OnPanelKey/OnPanelClick");
            }
        }
        if (info->capabilities & kMcPluginCapArchiveHandler) {
            plugin->canHandleFileFn = reinterpret_cast<McPlugin_CanHandleFileFn>(GetProcAddress(module, "McPlugin_CanHandleFile"));
            plugin->enterArchiveFn = reinterpret_cast<McPlugin_EnterArchiveFn>(GetProcAddress(module, "McPlugin_EnterArchive"));
            if (!plugin->canHandleFileFn || !plugin->enterArchiveFn) {
                logger.Log(LogLevel::Warning, L"Plugin \"" + id + L"\": declared kMcPluginCapArchiveHandler but is missing McPlugin_CanHandleFile or McPlugin_EnterArchive");
                plugin->canHandleFileFn = nullptr;
                plugin->enterArchiveFn = nullptr;
            }
        }
        if (info->capabilities & kMcPluginCapViewer) {
            plugin->canViewFileFn = reinterpret_cast<McPlugin_CanViewFileFn>(GetProcAddress(module, "McPlugin_CanViewFile"));
            plugin->renderViewFn = reinterpret_cast<McPlugin_RenderViewFn>(GetProcAddress(module, "McPlugin_RenderView"));
            plugin->getViewerNameFn =
                reinterpret_cast<McPlugin_GetViewerNameFn>(GetProcAddress(module, "McPlugin_GetViewerName"));
            if (!plugin->canViewFileFn || !plugin->renderViewFn) {
                logger.Log(LogLevel::Warning, L"Plugin \"" + id + L"\": declared kMcPluginCapViewer but is missing "
                                              L"McPlugin_CanViewFile or McPlugin_RenderView");
                plugin->canViewFileFn = nullptr;
                plugin->renderViewFn = nullptr;
            }
        }
        plugin->fileSystem = (info->capabilities & kMcPluginCapFileSystem) != 0;
        plugin->onThemeChangedFn =
            reinterpret_cast<McPlugin_OnThemeChangedFn>(GetProcAddress(module, "McPlugin_OnThemeChanged"));
        plugin->shutdownFn = reinterpret_cast<McPlugin_ShutdownFn>(GetProcAddress(module, "McPlugin_Shutdown"));

        auto initFn = reinterpret_cast<McPlugin_InitFn>(GetProcAddress(module, "McPlugin_Init"));
        plugin->state = kMcPluginLoaded;

        static McPluginHost host = {
            sizeof(McPluginHost), &ThunkShowChoice,        &ThunkShowMessage,   &ThunkShowTextPrompt,
            &ThunkLog,           &ThunkGetTheme,           &ThunkRegisterShortcut, &ThunkUnregisterShortcut,
            &ThunkAddMenuItem,   &ThunkRemoveMenuItem,     &ThunkSetMenuItemEnabled,
            &ThunkSimulateKey,   &ThunkGetActivePanelPath, &ThunkGetSelectedNames,
            &ThunkRequestPanelContent, &ThunkReleasePanelContent, &ThunkPanelDrawText,
            &ThunkPublishService,      &ThunkQueryService,
            &ThunkShowForm,            &ThunkGetActiveSide,       &ThunkGetPanelPath,
            &ThunkGetPanelSelectedNames, &ThunkRefreshPanel,
            &ThunkShowFileViewer,
            &ThunkBeginProgress, &ThunkUpdateProgress, &ThunkEndProgress,
            &ThunkPollCancel,
            &ThunkWaitWhilePaused,
            &ThunkOpenProvider, &ThunkCloseProvider, &ThunkGetPanelProvider, &ThunkGetPanelProviderPath,
            &ThunkIsPanelInProvider,
            &ThunkNavigatePanel,
        };

        bool initOk = true;
        if (initFn) {
            currentPluginId_ = id;
            bool initResult = false;
            bool called = SehCallInit(initFn, &host, &initResult);
            HostEndProgress();
            currentPluginId_.clear();
            initOk = called && initResult;
            if (!called) logger.Log(LogLevel::Error, L"Plugin \"" + id + L"\": crashed calling McPlugin_Init");
            else if (!initResult) logger.Log(LogLevel::Warning, L"Plugin \"" + id + L"\": McPlugin_Init returned false");
        }

        if (!initOk) {
            FreeLibrary(module);
            continue;
        }

        plugin->state = kMcPluginActive;
        logger.Log(LogLevel::Info, L"Plugin \"" + id + L"\" (" + name + L") loaded and active");
        plugins_.push_back(std::move(plugin));
    }
}

void PluginManager::Shutdown() {
    progressDialog_.reset();
    progressOwner_.clear();
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        LoadedPlugin& plugin = **it;
        // Remove every menu item/shortcut this plugin still owns *before*
        // calling its own Shutdown -- the host is the source of truth for
        // cleanup (§5), not the plugin's own bookkeeping.
        menuOverlay_.erase(std::remove_if(menuOverlay_.begin(), menuOverlay_.end(),
                                          [&](const PluginMenuEntry& e) { return e.pluginId == plugin.id; }),
                          menuOverlay_.end());
        for (auto shortcutIt = shortcutRegistry_.begin(); shortcutIt != shortcutRegistry_.end();) {
            if (shortcutIt->second.first == plugin.id) shortcutIt = shortcutRegistry_.erase(shortcutIt);
            else ++shortcutIt;
        }
        for (auto requestedIt = requestedShortcuts_.begin(); requestedIt != requestedShortcuts_.end();) {
            if (requestedIt->first.first == plugin.id) requestedIt = requestedShortcuts_.erase(requestedIt);
            else ++requestedIt;
        }
        // Phase 3, §8/§14: same host-owns-cleanup rule extended to panel
        // content and published services.
        ReleasePanelContentOwnedBy(plugin.id);
        ReleaseProvidersOwnedBy(plugin.id);
        UnpublishServicesOwnedBy(plugin.id);

        if (plugin.shutdownFn) {
            currentPluginId_ = plugin.id;
            bool called = SehCallShutdown(plugin.shutdownFn);
            HostEndProgress();
            currentPluginId_.clear();
            if (!called && logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + plugin.id + L"\": crashed calling McPlugin_Shutdown");
        }
        FreeLibrary(plugin.module);
    }
    plugins_.clear();
    RebuildMergedMenus();
}

void PluginManager::DispatchMenuCommand(const std::wstring& pluginId, uint32_t commandId) {
    LoadedPlugin* plugin = FindPlugin(pluginId);
    if (!plugin || !plugin->onMenuActionFn) return;
    currentPluginId_ = pluginId;
    bool called = SehCallOnMenuAction(plugin->onMenuActionFn, commandId);
    HostEndProgress();
    currentPluginId_.clear();
    if (!called && logger_) {
        logger_->Log(LogLevel::Error, L"Plugin \"" + pluginId + L"\": crashed handling a menu command");
    }
}

bool PluginManager::TryEnterAsArchive(McPanelSide side, const std::filesystem::path& filePath) {
    const std::wstring name = filePath.filename().wstring();
    const std::wstring fullPath = filePath.wstring();
    for (auto& plugin : plugins_) {
        if (!plugin->canHandleFileFn || !plugin->enterArchiveFn) continue;
        currentPluginId_ = plugin->id;
        bool matches = false;
        bool called = SehCallCanHandleFile(plugin->canHandleFileFn, name.c_str(), &matches);
        HostEndProgress();
        currentPluginId_.clear();
        if (!called) {
            if (logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed checking an archive file");
            continue;
        }
        if (!matches) continue;
        currentPluginId_ = plugin->id;
        bool entered = false;
        called = SehCallEnterArchive(plugin->enterArchiveFn, side, fullPath.c_str(), &entered);
        HostEndProgress();
        currentPluginId_.clear();
        if (!called && logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed entering an archive");
        return called && entered;
    }
    return false;
}

bool PluginManager::TryReceiveFiles(McPanelSide contentSide, McPanelSide sourceSide) {
    const std::wstring& ownerId = panelContentOwner_[contentSide == kMcPanelLeft ? 0 : 1];
    if (ownerId.empty()) return false;
    LoadedPlugin* plugin = FindPlugin(ownerId);
    if (!plugin || !plugin->onReceiveFilesFn) return false;
    currentPluginId_ = plugin->id;
    bool handled = false;
    bool called = SehCallOnReceiveFiles(plugin->onReceiveFilesFn, contentSide, sourceSide, &handled);
    HostEndProgress();
    currentPluginId_.clear();
    if (!called && logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed handling incoming files");
    return called && handled;
}

std::vector<ViewerCandidate> PluginManager::FindViewers(const std::wstring& fileName) {
    std::vector<ViewerCandidate> result;
    const std::wstring savedPlugin = currentPluginId_;  // may be called from inside a plugin's ShowFileViewer
    for (auto& plugin : plugins_) {
        if (!plugin->canViewFileFn || !plugin->renderViewFn) continue;
        currentPluginId_ = plugin->id;
        McViewClaim claim = kMcViewNo;
        bool called = SehCallCanViewFile(plugin->canViewFileFn, fileName.c_str(), &claim);
        if (!called) {
            if (logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed checking a file to view");
            continue;
        }
        if (claim != kMcViewOffer && claim != kMcViewDefault) continue;
        std::wstring name = plugin->name;
        const wchar_t* pluginName = nullptr;
        if (plugin->getViewerNameFn && SehCallGetViewerName(plugin->getViewerNameFn, &pluginName) && pluginName &&
            *pluginName)
            name = pluginName;
        result.push_back(ViewerCandidate{plugin->id, name, claim});
    }
    currentPluginId_ = savedPlugin;
    return result;
}

bool PluginManager::ShowPluginView(const std::wstring& pluginId, const std::filesystem::path& file) {
    LoadedPlugin* plugin = FindPlugin(pluginId);
    if (!console_ || !plugin || !plugin->renderViewFn) return false;
    ViewLineSink sink;
    McViewWriter writer{sizeof(McViewWriter), &sink, &ViewWriterAppendLine, &ViewWriterSetTitle};
    const std::wstring savedPlugin = currentPluginId_;
    currentPluginId_ = plugin->id;
    bool produced = false;
    bool called = SehCallRenderView(plugin->renderViewFn, file.c_str(), &writer, &produced);
    if (savedPlugin.empty()) HostEndProgress();  // a progress box the renderer forgot to close
    currentPluginId_ = savedPlugin;
    if (!called) {
        if (logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed rendering \"" + file.wstring() + L"\"");
        return false;
    }
    if (!produced) {
        if (logger_) logger_->Log(LogLevel::Warning, L"Plugin \"" + plugin->id + L"\": could not view \"" + file.wstring() + L"\"");
        return false;
    }
    if (sink.Truncated() && logger_)
        logger_->Log(LogLevel::Warning, L"Plugin \"" + plugin->id + L"\": view of \"" + file.wstring() + L"\" truncated");

    // Shown through the built-in viewer from a temporary UTF-8 file (with a
    // BOM), so paging stays bounded-memory (VEE-004) and search/hex work.
    wchar_t tempDir[MAX_PATH], tempPath[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tempDir) || !GetTempFileNameW(tempDir, L"mcv", 0, tempPath)) return false;
    if (!WriteUtf8TextFileAtomic(tempPath, L"﻿" + sink.Text())) {
        DeleteFileW(tempPath);
        return false;
    }
    std::wstring viewerName = plugin->name;
    const wchar_t* pluginViewerName = nullptr;
    if (plugin->getViewerNameFn && SehCallGetViewerName(plugin->getViewerNameFn, &pluginViewerName) && pluginViewerName &&
        *pluginViewerName)
        viewerName = pluginViewerName;
    ViewerStart start;
    start.title = sink.Title().empty() ? file.filename().wstring() + L" — " + viewerName : sink.Title();
    auto* progress = AnimatedProgressDialog::Current();
    if (progress) progress->Pause();
    ShowTextFileViewer(*console_, tempPath, start);
    if (progress) progress->Resume();
    DeleteFileW(tempPath);
    return true;
}

bool PluginManager::DispatchShortcut(WORD virtualKeyCode, bool ctrl, bool shift, bool alt) {
    auto it = shortcutRegistry_.find(PluginShortcutKey{virtualKeyCode, ctrl, shift, alt});
    if (it == shortcutRegistry_.end()) return false;
    LoadedPlugin* plugin = FindPlugin(it->second.first);
    if (!plugin || !plugin->onShortcutFn) return true;  // registered but not (or no longer) callable -- still "handled"
    currentPluginId_ = plugin->id;
    bool called = SehCallOnShortcut(plugin->onShortcutFn, it->second.second);
    HostEndProgress();
    currentPluginId_.clear();
    if (!called && logger_) {
        logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed handling a shortcut");
    }
    return true;
}

namespace {
// §11: packs a Win32 WORD console attribute into (fg, bg) small-integer
// slots (low nibble = foreground 0-15 incl. intensity bit, next nibble =
// background 0-7) -- a plugin author shouldn't need to know
// FOREGROUND_INTENSITY exists. Shared by NotifyThemeChanged (push, on a
// theme change) and HostGetThemeImpl (pull, on demand) so the two can
// never disagree about the packing.
McThemeSnapshot BuildThemeSnapshot(const ThemePalette& palette) {
    McThemeSnapshot snapshot{};
    snapshot.structSize = sizeof(McThemeSnapshot);
    auto pack = [](WORD attr, uint8_t& fg, uint8_t& bg) {
        fg = static_cast<uint8_t>(attr & 0x0F);
        bg = static_cast<uint8_t>((attr >> 4) & 0x0F);
    };
    pack(palette.attrNormal, snapshot.normalFg, snapshot.normalBg);
    pack(palette.attrDir, snapshot.dirFg, snapshot.dirBg);
    pack(palette.attrSelected, snapshot.selectedFg, snapshot.selectedBg);
    pack(palette.attrHeaderActive, snapshot.headerActiveFg, snapshot.headerActiveBg);
    pack(palette.attrHeaderInactive, snapshot.headerInactiveFg, snapshot.headerInactiveBg);
    pack(palette.attrStatusBar, snapshot.statusBarFg, snapshot.statusBarBg);
    pack(palette.attrCommandLine, snapshot.commandLineFg, snapshot.commandLineBg);
    pack(palette.attrBroken, snapshot.brokenFg, snapshot.brokenBg);
    pack(palette.attrBrokenBanner, snapshot.brokenBannerFg, snapshot.brokenBannerBg);
    pack(palette.attrBorder, snapshot.borderFg, snapshot.borderBg);
    pack(palette.attrTitle, snapshot.titleFg, snapshot.titleBg);
    pack(palette.attrBody, snapshot.bodyFg, snapshot.bodyBg);
    pack(palette.attrSection, snapshot.sectionFg, snapshot.sectionBg);
    pack(palette.attrKey, snapshot.keyFg, snapshot.keyBg);
    return snapshot;
}

// FEA-0002 Phase 1: a synthetic key-down/key-up pair injected into the
// console's own input queue -- the exact same WriteConsoleInputW technique
// IS-0003's hint-bar clicks and FEA-0005's SimulatedKey menu items already
// use (main.cpp's own SimulateKeyPress), reimplemented here rather than
// shared across the main.cpp/PluginManager.cpp boundary, since main.cpp's
// version is a local, unexported function operating on HintAction -- a
// small, self-contained duplication judged simpler than restructuring an
// already-working, already-tested call site to export it.
void InjectKeyPress(HANDLE inputHandle, WORD virtualKeyCode, bool ctrl, bool shift, bool alt) {
    DWORD controlKeyState = 0;
    if (ctrl) controlKeyState |= LEFT_CTRL_PRESSED;
    if (shift) controlKeyState |= SHIFT_PRESSED;
    if (alt) controlKeyState |= LEFT_ALT_PRESSED;

    INPUT_RECORD records[2] = {};
    for (auto& record : records) {
        record.EventType = KEY_EVENT;
        record.Event.KeyEvent.wRepeatCount = 1;
        record.Event.KeyEvent.wVirtualKeyCode = virtualKeyCode;
        record.Event.KeyEvent.wVirtualScanCode = static_cast<WORD>(MapVirtualKeyW(virtualKeyCode, MAPVK_VK_TO_VSC));
        record.Event.KeyEvent.dwControlKeyState = controlKeyState;
    }
    records[0].Event.KeyEvent.bKeyDown = TRUE;
    records[1].Event.KeyEvent.bKeyDown = FALSE;

    // KEY-005: a plugin simulates a *default* key; it must not be remapped.
    NoteInjectedKey(KeyChord{virtualKeyCode, ctrl, shift, alt});
    DWORD written = 0;
    WriteConsoleInputW(inputHandle, records, 2, &written);
}

// Copies `text` into `outBuffer` (capacity `outBufferSize`, in wchar_t
// units including the null terminator), truncating rather than
// overrunning -- the one small helper every Host*Impl string-out
// parameter below shares.
void CopyTruncated(const std::wstring& text, wchar_t* outBuffer, int outBufferSize) {
    if (!outBuffer || outBufferSize <= 0) return;
    size_t count = std::min(text.size(), static_cast<size_t>(outBufferSize - 1));
    std::copy(text.begin(), text.begin() + static_cast<std::ptrdiff_t>(count), outBuffer);
    outBuffer[count] = L'\0';
}
}  // namespace

void PluginManager::NotifyThemeChanged(const ThemePalette& palette) {
    McThemeSnapshot snapshot = BuildThemeSnapshot(palette);
    for (auto& plugin : plugins_) {
        if (!plugin->onThemeChangedFn) continue;
        currentPluginId_ = plugin->id;
        bool called = SehCallOnThemeChanged(plugin->onThemeChangedFn, &snapshot);
        HostEndProgress();
        currentPluginId_.clear();
        if (!called && logger_) {
            logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed handling a theme change");
        }
    }
}

std::vector<PluginHelpContent> PluginManager::CollectHelpContent() {
    std::vector<PluginHelpContent> result;
    for (auto& plugin : plugins_) {
        if (!plugin->getHelpMarkdownFn) continue;
        currentPluginId_ = plugin->id;
        const wchar_t* markdown = nullptr;
        bool called = SehCallGetHelpMarkdown(plugin->getHelpMarkdownFn, &markdown);
        HostEndProgress();
        currentPluginId_.clear();
        if (!called) {
            if (logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + plugin->id + L"\": crashed calling McPlugin_GetHelpMarkdown");
            continue;
        }
        if (!markdown) continue;
        result.push_back(PluginHelpContent{plugin->name, plugin->version, markdown});
    }
    return result;
}

bool PluginManager::HostAddMenuItem(wchar_t topMenuMnemonic, const std::wstring& label, wchar_t itemMnemonic,
                                    uint32_t commandId, McMenuPlacement placement, wchar_t afterItemMnemonic) {
    if (currentPluginId_.empty()) return false;
    (void)placement;
    (void)afterItemMnemonic;  // kMcMenuPlacementAfter's exact insertion index is a Phase 1.1 refinement;
                              // Phase 1 always appends (still satisfies "insert"/"append" as "append", the
                              // simpler, always-correct-per-menu fallback MergePluginMenuItems itself documents).
    PluginMenuEntry entry;
    entry.pluginId = currentPluginId_;
    entry.topMenuMnemonic = topMenuMnemonic;
    entry.label = label;
    entry.itemMnemonic = itemMnemonic;
    entry.commandId = commandId;
    entry.enabled = true;
    menuOverlay_.push_back(std::move(entry));
    RebuildMergedMenus();
    return true;
}

void PluginManager::HostRemoveMenuItem(uint32_t commandId) {
    if (currentPluginId_.empty()) return;
    menuOverlay_.erase(std::remove_if(menuOverlay_.begin(), menuOverlay_.end(),
                                      [&](const PluginMenuEntry& e) {
                                          return e.pluginId == currentPluginId_ && e.commandId == commandId;
                                      }),
                      menuOverlay_.end());
    RebuildMergedMenus();
}

void PluginManager::HostSetMenuItemEnabled(uint32_t commandId, bool enabled) {
    if (currentPluginId_.empty()) return;
    for (auto& entry : menuOverlay_) {
        if (entry.pluginId == currentPluginId_ && entry.commandId == commandId) entry.enabled = enabled;
    }
    RebuildMergedMenus();
}

// KEY-005: registers one plugin command under the keys keys.conf gives it
// (plugin:<id>/<commandId>), or under the plugin's own requested key when
// keys.conf doesn't mention it. True when every key it should have got was
// free -- "none" in keys.conf registers nothing and still counts as success,
// so the plugin doesn't report a conflict that is really the user's choice.
bool PluginManager::BindPluginShortcut(const std::wstring& pluginId, uint32_t commandId,
                                       const PluginShortcutKey& requested) {
    std::vector<PluginShortcutKey> wanted;
    std::optional<std::vector<KeyChord>> overrides =
        keyMap_ ? keyMap_->PluginKeysFor({pluginId, commandId}) : std::nullopt;
    if (overrides) {
        std::transform(overrides->begin(), overrides->end(), std::back_inserter(wanted),
                       [](const KeyChord& c) { return PluginShortcutKey{c.vk, c.ctrl, c.shift, c.alt}; });
    } else {
        wanted.push_back(requested);
    }
    const std::vector<PluginShortcutKey> builtin =
        keyMap_ ? BuiltinShortcutInventory(*keyMap_) : BuiltinShortcutInventory();
    bool allBound = true;
    for (const PluginShortcutKey& key : wanted) {
        // A key the user explicitly gave this command beats a built-in default it shadows only
        // through keys.conf itself (KeyMap::Load already took it away from the built-in command).
        if (ConflictsWithBuiltinShortcut(key, builtin) || shortcutRegistry_.count(key) != 0) {
            allBound = false;
            if (logger_) {
                logger_->Log(LogLevel::Warning, L"Plugin \"" + pluginId + L"\": shortcut " +
                                                    FormatKeyChord(KeyChord{key.virtualKeyCode, key.ctrl, key.shift,
                                                                            key.alt}) +
                                                    L" is already in use; not registered.");
            }
            continue;
        }
        shortcutRegistry_[key] = {pluginId, commandId};
    }
    return allBound;
}

bool PluginManager::HostRegisterShortcut(const McShortcutSpec& spec) {
    if (currentPluginId_.empty()) return false;
    PluginShortcutKey requested{spec.virtualKeyCode, spec.ctrl, spec.shift, spec.alt};
    requestedShortcuts_[{currentPluginId_, spec.commandId}] = requested;
    return BindPluginShortcut(currentPluginId_, spec.commandId, requested);
}

void PluginManager::HostUnregisterShortcut(uint32_t commandId) {
    if (currentPluginId_.empty()) return;
    requestedShortcuts_.erase({currentPluginId_, commandId});
    for (auto it = shortcutRegistry_.begin(); it != shortcutRegistry_.end();) {
        if (it->second.first == currentPluginId_ && it->second.second == commandId) it = shortcutRegistry_.erase(it);
        else ++it;
    }
}

void PluginManager::SetKeyMap(const KeyMap* keyMap) {
    keyMap_ = keyMap;
    // Re-register every live plugin shortcut under the new map.
    shortcutRegistry_.clear();
    for (const auto& [owner, requested] : requestedShortcuts_) BindPluginShortcut(owner.first, owner.second, requested);
}

std::wstring PluginManager::ShortcutTextFor(const std::wstring& pluginId, uint32_t commandId) const {
    for (const auto& [key, owner] : shortcutRegistry_) {
        if (owner.first == pluginId && owner.second == commandId)
            return FormatKeyChord(KeyChord{key.virtualKeyCode, key.ctrl, key.shift, key.alt});
    }
    return L"";
}

void PluginManager::HostLog(McLogLevel level, const std::wstring& message) {
    if (!logger_ || currentPluginId_.empty()) return;
    LogLevel mapped = LogLevel::Info;
    switch (level) {
        case kMcLogError: mapped = LogLevel::Error; break;
        case kMcLogWarning: mapped = LogLevel::Warning; break;
        case kMcLogInfo: mapped = LogLevel::Info; break;
        case kMcLogDebug: mapped = LogLevel::Debug; break;
    }
    logger_->Log(mapped, L"[" + currentPluginId_ + L"] " + message);
}

void PluginManager::SetAppContext(Console& console, Panel& left, Panel& right, bool& leftActive,
                                  std::function<void()> redrawBackground) {
    console_ = &console;
    leftPanel_ = &left;
    rightPanel_ = &right;
    leftActivePtr_ = &leftActive;
    redrawBackground_ = std::move(redrawBackground);
}

int PluginManager::HostShowChoiceImpl(const wchar_t* title, const wchar_t* const* lines, int lineCount,
                                      const McDialogOption* options, int optionCount) {
    if (!console_ || !title || (lineCount > 0 && !lines) || (optionCount > 0 && !options)) return -1;
    std::vector<std::wstring> lineVec;
    for (int i = 0; i < lineCount; ++i) lineVec.push_back(lines[i] ? lines[i] : L"");
    std::vector<DialogOption> optionVec;
    for (int i = 0; i < optionCount; ++i) {
        optionVec.push_back(DialogOption{options[i].hotkey, options[i].label ? options[i].label : L""});
    }
    auto redraw = redrawBackground_ ? redrawBackground_ : [] {};
    return ShowChoiceDialog(*console_, title, lineVec, optionVec, redraw);
}

void PluginManager::HostShowMessageImpl(const wchar_t* title, const wchar_t* const* lines, int lineCount) {
    if (!console_ || !title || (lineCount > 0 && !lines)) return;
    std::vector<std::wstring> lineVec;
    for (int i = 0; i < lineCount; ++i) lineVec.push_back(lines[i] ? lines[i] : L"");
    auto redraw = redrawBackground_ ? redrawBackground_ : [] {};
    ShowMessage(*console_, title, lineVec, redraw);
}

bool PluginManager::HostShowTextPromptImpl(const wchar_t* title, const wchar_t* initialText, wchar_t* outBuffer,
                                           int outBufferSize) {
    if (!console_ || !title || !outBuffer || outBufferSize <= 0) return false;
    auto redraw = redrawBackground_ ? redrawBackground_ : [] {};
    auto result = ShowTextPrompt(*console_, title, initialText ? initialText : L"", redraw);
    if (!result) return false;
    CopyTruncated(*result, outBuffer, outBufferSize);
    return true;
}

void PluginManager::HostGetThemeImpl(McThemeSnapshot* out) {
    if (!out) return;
    ThemePalette live{kAttrNormal,       kAttrDir,          kAttrSelected,      kAttrHeaderActive,
                      kAttrHeaderInactive, kAttrStatusBar,   kAttrCommandLine,   kAttrBroken,
                      kAttrBrokenBanner, kAttrBorder,        kAttrTitle,         kAttrBody,
                      kAttrSection,      kAttrKey};
    *out = BuildThemeSnapshot(live);
}
void PluginManager::HostShowFileViewer(const wchar_t* path) {
    if (!console_ || !path || !*path) return;
    // EXT-003 D3: a viewer plugin claiming this file as its default shows it;
    // never an external program from file-actions.conf (the file is usually a
    // plugin's temporary copy, deleted as soon as this returns).
    std::filesystem::path file(path);
    std::vector<ViewerCandidate> viewers = FindViewers(file.filename().wstring());
    size_t index = 0;
    if (ChooseViewRoute(false, viewers, index) == ViewRoute::Plugin && ShowPluginView(viewers[index].pluginId, file)) return;
    auto* progress = AnimatedProgressDialog::Current();
    if (progress) progress->Pause();
    ShowTextFileViewer(*console_, std::filesystem::path(path));
    if (progress) progress->Resume();
}
bool PluginManager::HostBeginProgress(const wchar_t* title, uint64_t total) {
    if (!console_ || !title || currentPluginId_.empty() || progressDialog_ || currentPanelDraw_.valid) return false;
    try {
        progressOwner_ = currentPluginId_;
        progressDialog_ = std::make_unique<AnimatedProgressDialog>(*console_, title, total, redrawBackground_);
    } catch (...) {
        progressOwner_.clear();
        return false;
    }
    return true;
}
void PluginManager::HostUpdateProgress(uint64_t completed, uint64_t total, const wchar_t* message) {
    if (progressDialog_ && currentPluginId_ == progressOwner_)
        progressDialog_->Update(completed, total, message ? message : L"Working...");
}
void PluginManager::HostEndProgress() {
    if (progressDialog_ && currentPluginId_ == progressOwner_) {
        progressDialog_.reset(); progressOwner_.clear();
    }
}
bool PluginManager::HostPollCancel() {
    if (!console_) return false;
    // FOP-015: P toggles the shared pause, but only for a plugin that can act on it
    // (API >= 1.11 and so able to call WaitWhilePaused); for any other plugin the
    // key is swallowed, as every non-Esc key always was.
    ProgressKeys keys = PollProgressKeys(*console_);
    if (pause_) {
        if (keys.cancel) {
            pause_->Resume();
        } else if (keys.togglePause) {
            const LoadedPlugin* plugin = FindPlugin(currentPluginId_);
            if (plugin && plugin->apiMinor >= 11) pause_->Toggle();
        }
    }
    return keys.cancel;
}

bool PluginManager::HostWaitWhilePaused() {
    if (!console_ || !pause_ || !pause_->Paused()) return true;
    AnimatedProgressDialog* box = AnimatedProgressDialog::Current();
    if (box) box->SetUserPaused(true);
    bool cancelled = false;
    pause_->WaitWhilePaused([&]() {
        ProgressKeys keys = PollProgressKeys(*console_);
        if (keys.cancel) {
            cancelled = true;
            return true;
        }
        if (keys.togglePause) pause_->Resume();
        return false;
    });
    pause_->Resume();
    if (box) box->SetUserPaused(false);
    return !cancelled;
}

void PluginManager::HostSimulateKeyImpl(uint16_t virtualKeyCode, bool ctrl, bool shift, bool alt) {
    if (!console_) return;
    InjectKeyPress(console_->InputHandle(), static_cast<WORD>(virtualKeyCode), ctrl, shift, alt);
}

void PluginManager::HostGetActivePanelPathImpl(wchar_t* outBuffer, int outBufferSize) const {
    if (!leftPanel_ || !rightPanel_ || !leftActivePtr_) return;
    const Panel& active = *leftActivePtr_ ? *leftPanel_ : *rightPanel_;
    CopyTruncated(active.Path().wstring(), outBuffer, outBufferSize);
}

int PluginManager::HostGetSelectedNamesImpl(wchar_t** outNames, int outCount, int maxNameLength) const {
    if (!leftPanel_ || !rightPanel_ || !leftActivePtr_) return 0;
    const Panel& active = *leftActivePtr_ ? *leftPanel_ : *rightPanel_;
    std::vector<std::wstring> names = active.SelectionOrCursor();
    if (outNames && outCount > 0 && maxNameLength > 0) {
        for (int i = 0; i < outCount && i < static_cast<int>(names.size()); ++i) {
            CopyTruncated(names[static_cast<size_t>(i)], outNames[i], maxNameLength);
        }
    }
    return static_cast<int>(names.size());
}

// -- Phase 3, §8: panel content -----------------------------------------

Panel* PluginManager::PanelForSide(McPanelSide side) {
    if (side == kMcPanelLeft) return leftPanel_;
    if (side == kMcPanelRight) return rightPanel_;
    return nullptr;
}

bool PluginManager::HostRequestPanelContent(McPanelSide side, const wchar_t* title) {
    if (currentPluginId_.empty()) return false;
    Panel* panel = PanelForSide(side);
    if (!panel) return false;
    size_t sideIndex = side == kMcPanelLeft ? 0 : 1;
    if (!panelContentOwner_[sideIndex].empty() && panelContentOwner_[sideIndex] != currentPluginId_) return false;
    // FEA-0010: a view the host itself installed (the directory tree) is not replaced behind its back.
    if (panelContentOwner_[sideIndex].empty() && panel->Content() != nullptr) return false;

    LoadedPlugin* plugin = FindPlugin(currentPluginId_);
    if (!plugin || !plugin->onPanelDrawFn || !plugin->onPanelKeyFn || !plugin->onPanelClickFn) return false;

    panelAdapters_[sideIndex] = std::make_unique<PanelContentAdapter>(*this, currentPluginId_, side,
                                                                       plugin->onPanelDrawFn, plugin->onPanelKeyFn,
                                                                       plugin->onPanelClickFn, plugin->onPanelMouseFn);
    panelContentOwner_[sideIndex] = currentPluginId_;
    panel->SetContent(panelAdapters_[sideIndex].get(), title ? title : L"");
    return true;
}

void PluginManager::HostReleasePanelContent(McPanelSide side) {
    if (currentPluginId_.empty()) return;
    size_t sideIndex = side == kMcPanelLeft ? 0 : 1;
    if (panelContentOwner_[sideIndex] != currentPluginId_) return;
    Panel* panel = PanelForSide(side);
    if (panel) panel->SetContent(nullptr);
    panelAdapters_[sideIndex].reset();
    panelContentOwner_[sideIndex].clear();
}

void PluginManager::HostPanelDrawText(McPanelSide side, int16_t x, int16_t y, const wchar_t* text, McPanelColor color) {
    if (!text || !currentPanelDraw_.valid || currentPanelDraw_.side != side || !currentPanelDraw_.console) return;
    if (y < 0 || y >= currentPanelDraw_.height) return;

    std::wstring line(text);
    int startX = x;
    if (startX < 0) {
        size_t skip = static_cast<size_t>(-startX);
        if (skip >= line.size()) return;
        line = line.substr(skip);
        startX = 0;
    }
    int available = currentPanelDraw_.width - startX;
    if (available <= 0) return;
    if (static_cast<int>(line.size()) > available) line.resize(static_cast<size_t>(available));

    WORD attr = kAttrNormal;
    switch (color) {
        case kMcPanelColorNormal: attr = kAttrNormal; break;
        case kMcPanelColorHighlight:
            attr = static_cast<WORD>((kAttrSelected & kColorMask) | FOREGROUND_INTENSITY | BACKGROUND_BLUE);
            break;
        case kMcPanelColorSection: attr = kAttrSection; break;
        case kMcPanelColorWarning: attr = kAttrBroken; break;
        case kMcPanelColorSelected: attr = kAttrSelected; break;
    }
    currentPanelDraw_.console->PutText(static_cast<SHORT>(currentPanelDraw_.x + startX),
                                       static_cast<SHORT>(currentPanelDraw_.y + y), line, attr);
}

void PluginManager::ReleasePanelContentOwnedBy(const std::wstring& pluginId) {
    for (size_t sideIndex = 0; sideIndex < 2; ++sideIndex) {
        if (panelContentOwner_[sideIndex] != pluginId) continue;
        Panel* panel = PanelForSide(sideIndex == 0 ? kMcPanelLeft : kMcPanelRight);
        if (panel) panel->SetContent(nullptr);
        panelAdapters_[sideIndex].reset();
        panelContentOwner_[sideIndex].clear();
    }
}

// -- Phase 3, §14: inter-plugin services ---------------------------------

bool PluginManager::HostPublishService(const std::wstring& serviceName, void* interfacePtr, uint32_t interfaceVersion) {
    if (currentPluginId_.empty() || serviceName.empty()) return false;
    auto it = services_.find(serviceName);
    if (it != services_.end() && it->second.ownerPluginId != currentPluginId_) return false;
    services_[serviceName] = ServiceEntry{currentPluginId_, interfacePtr, interfaceVersion};
    return true;
}

void* PluginManager::HostQueryService(const std::wstring& serviceName, uint32_t requestedVersion) {
    auto it = services_.find(serviceName);
    if (it == services_.end() || it->second.interfaceVersion < requestedVersion) return nullptr;
    return it->second.interfacePtr;
}

void PluginManager::UnpublishServicesOwnedBy(const std::wstring& pluginId) {
    for (auto it = services_.begin(); it != services_.end();) {
        if (it->second.ownerPluginId == pluginId) it = services_.erase(it);
        else ++it;
    }
}

// -- FEA-0007, §1/§3: form dialog and side-scoped panel queries ----------

bool PluginManager::HostShowForm(const wchar_t* title, McFormField* fields, int fieldCount) {
    if (!console_ || !title || (fieldCount > 0 && !fields)) return false;

    std::vector<FormField> nativeFields;
    nativeFields.reserve(static_cast<size_t>(fieldCount));
    for (int i = 0; i < fieldCount; ++i) {
        const McFormField& src = fields[i];
        FormField field;
        field.label = src.label ? src.label : L"";
        switch (src.kind) {
            case kMcFormFieldCheckbox:
                field.kind = FormFieldKind::Checkbox;
                field.boolValue = src.boolValue;
                break;
            case kMcFormFieldNumber:
                field.kind = FormFieldKind::Number;
                field.numberValue = src.numberValue;
                field.numberMin = src.numberMin;
                field.numberMax = src.numberMax;
                break;
            case kMcFormFieldChoice:
                field.kind = FormFieldKind::Choice;
                field.choiceIndex = src.choiceIndex;
                for (int c = 0; c < src.choiceCount; ++c) {
                    field.choices.push_back(src.choices && src.choices[c] ? src.choices[c] : L"");
                }
                break;
            case kMcFormFieldText:
            case kMcFormFieldPassword:  // API 1.9, FTP-010: a masked Text field
                field.kind = FormFieldKind::Text;
                field.masked = src.kind == kMcFormFieldPassword;
                field.text = src.textBuffer ? src.textBuffer : L"";
                // IS-0016: start at the end of a pre-filled value, like
                // ShowTextPrompt, so typing appends rather than prepends.
                field.textCursor = field.text.size();
                break;
        }
        nativeFields.push_back(std::move(field));
    }

    auto redraw = redrawBackground_ ? redrawBackground_ : [] {};
    bool confirmed = ShowFormDialog(*console_, title, nativeFields, redraw);

    // Write every field's (possibly edited) value back -- even on
    // cancellation, matching McHost_ShowFormFn's own documented "fields left
    // however the user had them edited" contract (the caller is told not to
    // trust them via the `false` return, not left with stale pre-dialog data).
    for (int i = 0; i < fieldCount; ++i) {
        McFormField& dst = fields[i];
        const FormField& field = nativeFields[static_cast<size_t>(i)];
        switch (dst.kind) {
            case kMcFormFieldCheckbox:
                dst.boolValue = field.boolValue;
                break;
            case kMcFormFieldNumber:
                dst.numberValue = field.numberValue;
                break;
            case kMcFormFieldChoice:
                dst.choiceIndex = field.choiceIndex;
                break;
            case kMcFormFieldText:
            case kMcFormFieldPassword:
                CopyTruncated(field.text, dst.textBuffer, dst.textBufferSize);
                break;
        }
    }
    return confirmed;
}

McPanelSide PluginManager::HostGetActiveSide() const {
    return (leftActivePtr_ && *leftActivePtr_) ? kMcPanelLeft : kMcPanelRight;
}

void PluginManager::HostGetPanelPath(McPanelSide side, wchar_t* outBuffer, int outBufferSize) {
    Panel* panel = PanelForSide(side);
    if (!panel) return;
    CopyTruncated(panel->Path().wstring(), outBuffer, outBufferSize);
}

int PluginManager::HostGetPanelSelectedNames(McPanelSide side, wchar_t** outNames, int outCount, int maxNameLength) {
    Panel* panel = PanelForSide(side);
    if (!panel) return 0;
    std::vector<std::wstring> names = panel->SelectionOrCursor();
    if (outNames && outCount > 0 && maxNameLength > 0) {
        for (int i = 0; i < outCount && i < static_cast<int>(names.size()); ++i) {
            CopyTruncated(names[static_cast<size_t>(i)], outNames[i], maxNameLength);
        }
    }
    return static_cast<int>(names.size());
}

void PluginManager::HostRefreshPanel(McPanelSide side) {
    Panel* panel = PanelForSide(side);
    if (!panel) return;
    panel->Refresh();
    // IS-0015/NAV-012: a plugin asking to refresh one side shouldn't leave
    // the other panel stale if it happens to show the exact same directory
    // -- the same fix applied to every host-side Do* operation's own
    // single-panel refresh.
    Panel* other = PanelForSide(side == kMcPanelLeft ? kMcPanelRight : kMcPanelLeft);
    if (other && PathsEquivalent(panel->Path(), other->Path())) other->Refresh();
}

// --- API 1.12 (EXT-002): file-system providers -----------------------------

PluginProviderAdapter* PluginManager::OwnedProvider(McPanelSide side, const std::wstring& pluginId) {
    Panel* panel = PanelForSide(side);
    if (!panel || !panel->InProvider()) return nullptr;
    auto* adapter = dynamic_cast<PluginProviderAdapter*>(panel->Provider());
    return adapter && adapter->OwnerId() == pluginId ? adapter : nullptr;
}

bool PluginManager::HostOpenProvider(McPanelSide side, const McFsProvider* table, const wchar_t* startPath,
                                     const wchar_t* returnSelectName) {
    if (currentPluginId_.empty() || !table) return false;
    const std::wstring owner = currentPluginId_;
    LoadedPlugin* plugin = FindPlugin(owner);
    Panel* panel = PanelForSide(side);
    if (!plugin || !panel) return false;
    if (!plugin->fileSystem) {
        if (logger_) logger_->Log(LogLevel::Warning, L"Plugin \"" + owner + L"\": OpenProvider needs kMcPluginCapFileSystem");
        return false;
    }
    std::wstring problem;
    if (!PluginProviderAdapter::Validate(*table, problem)) {
        if (logger_) logger_->Log(LogLevel::Warning, L"Plugin \"" + owner + L"\": invalid McFsProvider: " + problem);
        return false;
    }
    size_t sideIndex = side == kMcPanelLeft ? 0 : 1;
    if (!panelContentOwner_[sideIndex].empty()) {
        if (panelContentOwner_[sideIndex] != owner) return false;
        panel->SetContent(nullptr);  // the plugin's own panel content gives way to its provider
        panelAdapters_[sideIndex].reset();
        panelContentOwner_[sideIndex].clear();
    }
    if (panel->InProvider() && !panel->CloseProvider(false)) return false;

    auto scope = [this, owner](bool entering) {
        if (entering) {
            scopeStack_.push_back(currentPluginId_);
            currentPluginId_ = owner;
        } else if (!scopeStack_.empty()) {
            currentPluginId_ = scopeStack_.back();
            scopeStack_.pop_back();
        }
    };
    auto crash = [this, owner](const std::wstring& what) {
        if (logger_) logger_->Log(LogLevel::Error, L"Plugin \"" + owner + L"\": " + what);
    };
    auto adapter = std::make_shared<PluginProviderAdapter>(*table, owner, scope, crash);
    std::wstring start = startPath && *startPath ? std::wstring(startPath) : std::wstring(table->rootPath);
    // Listing happens right here, re-entering the plugin from inside its own
    // OpenProvider call -- allowed (single-threaded); the table must be ready.
    panel->OpenProvider(adapter, start, returnSelectName ? returnSelectName : L"",
                        [this](IFileProvider& provider) { return ConfirmLeaveProvider(provider); });
    return true;
}

bool PluginManager::HostCloseProvider(McPanelSide side) {
    Panel* panel = PanelForSide(side);
    PluginProviderAdapter* adapter = OwnedProvider(side, currentPluginId_);
    if (!panel || !adapter) return false;
    // The plugin's own explicit command (Disconnect, ...) needs no "Leave?"
    // question -- only unsaved changes are worth asking about.
    return panel->CloseProvider(/*force=*/!adapter->HasPendingChanges());
}

void* PluginManager::HostGetPanelProvider(McPanelSide side) {
    PluginProviderAdapter* adapter = OwnedProvider(side, currentPluginId_);
    return adapter ? adapter->Context() : nullptr;
}

bool PluginManager::HostGetPanelProviderPath(McPanelSide side, wchar_t* outBuffer, int outBufferSize) {
    if (!OwnedProvider(side, currentPluginId_)) return false;
    CopyTruncated(PanelForSide(side)->ProviderPath(), outBuffer, outBufferSize);
    return true;
}

bool PluginManager::HostIsPanelInProvider(McPanelSide side) {
    Panel* panel = PanelForSide(side);
    return panel && panel->InProvider();
}

bool PluginManager::HostNavigatePanel(McPanelSide side, const wchar_t* path) {
    Panel* panel = PanelForSide(side);
    if (!panel || !path || !*path) return false;
    if (!CanNavigatePanel(panel->Content() != nullptr, panel->InProvider())) return false;
    panel->NavigateTo(std::filesystem::path(path));
    return true;
}

bool PluginManager::ConfirmLeaveProvider(IFileProvider& provider) {
    if (!console_) return true;
    std::function<void()> redraw = redrawBackground_ ? redrawBackground_ : [] {};
    if (!provider.HasPendingChanges()) {
        if (!provider.AsksBeforeLeaving()) return true;
        int leave = ShowChoiceDialog(*console_, kStrProviderLeaveTitle, {kStrProviderLeaveQuestion + provider.DisplayRoot() + L"?"},
                                     {{L'Y', kStrYesLabel}, {L'N', kStrNoLabel}}, redraw);
        return leave == 0;
    }
    int choice = ShowChoiceDialog(*console_, kStrProviderSaveTitle, {provider.DisplayRoot(), kStrProviderSaveQuestion},
                                  {{L'S', kStrSaveLabel}, {L'D', kStrDiscardLabel}, {L'C', kStrCancelLabel}}, redraw);
    if (choice == 1) return true;
    if (choice != 0) return false;
    std::wstring error;
    ProviderStatus status = provider.Commit({}, error);
    if (status == ProviderStatus::Ok) return true;
    ShowMessage(*console_, kStrProviderSaveFailedTitle,
                {provider.DisplayRoot(), error.empty() ? std::wstring(ProviderStatusText(status)) : error}, redraw);
    return false;
}

void PluginManager::ReleaseProvidersOwnedBy(const std::wstring& pluginId) {
    for (McPanelSide side : {kMcPanelLeft, kMcPanelRight}) {
        if (OwnedProvider(side, pluginId)) PanelForSide(side)->CloseProvider(true);
    }
}

}  // namespace mc
