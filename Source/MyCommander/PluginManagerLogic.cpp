// FEA-0002 Phase 1: the pure, Console/Panel/Dialog-independent decision
// logic PluginManager (PluginManager.cpp) builds on -- kept in its own
// translation unit, matching Navigation.cpp/DialogInput.cpp's own
// established precedent (TST-005), so the test project can compile and
// exercise it directly without pulling in PluginManager.cpp's real
// Dialog.cpp/Console.cpp/Panel.cpp dependency.

#include "PluginManager.h"

#include <algorithm>
#include <cwctype>

namespace mc {

bool IsPluginApiCompatible(uint32_t hostMajor, uint32_t hostMinor, uint32_t pluginMajor, uint32_t pluginMinor) {
    if (pluginMajor != hostMajor) return false;
    return pluginMinor <= hostMinor;
}

std::vector<TopMenu> MergePluginMenuItems(const std::vector<TopMenu>& builtins,
                                          const std::vector<PluginMenuEntry>& overlay) {
    std::vector<TopMenu> merged = builtins;
    for (const auto& entry : overlay) {
        if (!entry.enabled) continue;
        auto menuIt = std::find_if(merged.begin(), merged.end(), [&](const TopMenu& m) {
            return std::towlower(m.mnemonic) == std::towlower(entry.topMenuMnemonic);
        });
        if (menuIt == merged.end()) continue;  // no matching top menu -- silently skipped, see header comment

        MenuItem item;
        item.label = entry.label;
        item.mnemonic = entry.itemMnemonic;
        item.kind = MenuItemKind::PluginCommand;
        item.pluginId = entry.pluginId;
        item.pluginCommandId = entry.commandId;
        menuIt->items.push_back(std::move(item));
    }
    return merged;
}

bool ConflictsWithBuiltinShortcut(const PluginShortcutKey& key, const std::vector<PluginShortcutKey>& builtinKeys) {
    return std::find(builtinKeys.begin(), builtinKeys.end(), key) != builtinKeys.end();
}

bool CanNavigatePanel(bool panelHasContent, bool panelInProvider) {
    return !panelHasContent && !panelInProvider;
}

const std::vector<PluginShortcutKey>& BuiltinShortcutInventory() {
    static const std::vector<PluginShortcutKey> keys = {
        // Function keys, plain and modified -- see main.cpp's own
        // key-dispatch switch for the source of truth this mirrors.
        {VK_F1, false, false, false}, {VK_F1, false, false, true},                        // F1, Alt+F1
        {VK_F1, true, false, false},                                                        // Ctrl+F1 (single panel, UI-013)
        {VK_F2, false, false, false}, {VK_F2, false, false, true}, {VK_F2, true, false, false},  // F2, Alt+F2, Ctrl+F2
        {VK_F2, false, true, false},                                                        // Shift+F2 (compare, SRC-006)
        {VK_F2, true, true, false},                                                         // Ctrl+Shift+F2 (sync, SRC-007)
        {VK_F3, false, false, false}, {VK_F3, true, false, false},                          // F3, Ctrl+F3
        {VK_F3, false, false, true}, {VK_F3, false, true, true},  // Alt+F3, Shift+Alt+F3 (compare files, SRC-008)
        {VK_F4, false, false, false}, {VK_F4, true, false, false},                          // F4, Ctrl+F4
        {VK_F5, false, false, false}, {VK_F5, true, false, false},                          // F5, Ctrl+F5
        {VK_F6, false, false, false}, {VK_F6, true, false, false},                          // F6, Ctrl+F6
        {VK_F7, false, false, false}, {VK_F7, false, true, false}, {VK_F7, false, false, true},  // F7, Shift+F7, Alt+F7
        {VK_F8, false, false, false}, {VK_F8, false, true, false},                          // F8, Shift+F8
        {VK_F9, false, false, false}, {VK_F9, true, false, false},  // F9, Ctrl+F9 (sort by attributes, LIST-011)
        {VK_F10, false, false, false},
        {VK_F11, true, false, false},  // Ctrl+F11 (IS-0006)
        // Ctrl+letter shortcuts.
        {static_cast<WORD>(L'F'), true, false, false},  // Ctrl+F (filter)
        {static_cast<WORD>(L'A'), true, false, false},  // Ctrl+A (attributes)
        {static_cast<WORD>(L'H'), true, false, false},  // Ctrl+H (hidden/system)
        {static_cast<WORD>(L'R'), true, false, false},  // Ctrl+R (refresh panel, IS-0012)
        {static_cast<WORD>(L'T'), true, false, false},  // Ctrl+T (directory tree, UI-013)
        {static_cast<WORD>(L'D'), true, false, false},  // Ctrl+D (directory hotlist, NAV-007)
        {static_cast<WORD>(L'M'), true, false, false},  // Ctrl+M (group rename, FOP-017)
        {static_cast<WORD>(L'U'), true, false, false},  // Ctrl+U (swap panels, NAV-016)
        {static_cast<WORD>(L'L'), true, false, false}, {static_cast<WORD>(L'L'), true, true, false},  // Ctrl+L, Ctrl+Shift+L (colour labels, LIST-009)
        {static_cast<WORD>(L'Z'), true, false, false},  // Ctrl+Z (undo, FOP-022)
        {static_cast<WORD>(L'V'), false, true, true},   // Alt+Shift+V (paste from Explorer, FOP-025)
        {VK_F12, false, false, true},                   // Alt+F12 (recent folders, NAV-017)
        {VK_F12, true, false, false},                   // Ctrl+F12 (largest files, SRC-010)
        {VK_F7, false, true, true},                     // Alt+Shift+F7 (find empty folders, SRC-009)
        {VK_F5, false, false, true}, {VK_F6, false, false, true},  // Alt+F5, Alt+F6 (queued copy/move, FOP-014)
        {VK_F8, false, false, true}, {VK_F8, false, true, true},   // Alt+F8, Alt+Shift+F8 (queued delete, FOP-014)
        {static_cast<WORD>(L'Q'), false, false, true},  // Alt+Q (job queue, FOP-014)
        {static_cast<WORD>(L'R'), false, false, true},  // Alt+R (reverse history search, CLI-008)
        {VK_SPACE, true, false, false},                 // Ctrl+Space (complete path, NAV-018)
        // The menu bar's own six top-menu mnemonics (FEA-0005): Alt+letter
        // opens that menu from the closed state.
        {static_cast<WORD>(L'F'), false, false, true},  // Alt+F (File)
        {static_cast<WORD>(L'S'), false, false, true},  // Alt+S (Selection)
        {static_cast<WORD>(L'C'), false, false, true},  // Alt+C (Commands)
        {static_cast<WORD>(L'N'), false, false, true},  // Alt+N (Network)
        {static_cast<WORD>(L'V'), false, false, true},  // Alt+V (View)
        {static_cast<WORD>(L'E'), false, false, true},  // Alt+E (Settings)
        // Command-line/insert/select-by-mask keys.
        {VK_RETURN, false, false, false}, {VK_RETURN, true, false, false}, {VK_RETURN, true, true, false},
        {VK_ESCAPE, false, false, false},
        {VK_INSERT, false, false, false},
        {VK_ADD, false, false, false}, {VK_ADD, true, false, false},
        {VK_SUBTRACT, false, false, false}, {VK_SUBTRACT, true, false, false},
        {VK_MULTIPLY, false, false, false},
    };
    static const std::vector<PluginShortcutKey> withCommands = [] {
        std::vector<PluginShortcutKey> all = keys;
        for (const auto& command : Commands()) {
            for (const KeyChord& c : command.defaults) {
                PluginShortcutKey k{c.vk, c.ctrl, c.shift, c.alt};
                if (std::find(all.begin(), all.end(), k) == all.end()) all.push_back(k);
            }
        }
        return all;
    }();
    return withCommands;
}

ViewRoute ChooseViewRoute(bool hasFileActionDefault, const std::vector<ViewerCandidate>& candidates, size_t& pluginIndex) {
    if (hasFileActionDefault) return ViewRoute::FileAction;
    for (size_t i = 0; i < candidates.size(); ++i) {
        if (candidates[i].claim == kMcViewDefault) {
            pluginIndex = i;
            return ViewRoute::Plugin;
        }
    }
    return ViewRoute::BuiltIn;
}

void ViewLineSink::Append(const wchar_t* text) {
    if (truncated_) return;
    std::wstring line = text ? text : L"";
    size_t lineBytes = (line.size() + 2) * sizeof(wchar_t);
    if (lines_.size() >= maxLines_ || bytes_ + lineBytes > maxBytes_) {
        truncated_ = true;
        return;
    }
    bytes_ += lineBytes;
    lines_.push_back(std::move(line));
}

std::wstring ViewLineSink::Text() const {
    std::wstring text;
    text.reserve(bytes_ / sizeof(wchar_t) + 64);
    for (const auto& line : lines_) {
        text += line;
        text += L"\r\n";
    }
    if (truncated_) text += kViewTruncatedMarker;
    return text;
}

std::vector<PluginShortcutKey> BuiltinShortcutInventory(const KeyMap& keyMap) {
    std::vector<PluginShortcutKey> result;
    for (const PluginShortcutKey& k : BuiltinShortcutInventory()) {
        if (!keyMap.IsFreed(KeyChord{k.virtualKeyCode, k.ctrl, k.shift, k.alt})) result.push_back(k);
    }
    for (const KeyChord& c : keyMap.AllBuiltinChords()) {
        PluginShortcutKey k{c.vk, c.ctrl, c.shift, c.alt};
        if (std::find(result.begin(), result.end(), k) == result.end()) result.push_back(k);
    }
    return result;
}

}  // namespace mc
