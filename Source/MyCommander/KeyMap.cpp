#include "KeyMap.h"

#include <algorithm>
#include <cwctype>
#include <set>

namespace mc {

namespace {

KeyChord K(WORD vk, bool ctrl = false, bool shift = false, bool alt = false) { return KeyChord{vk, ctrl, shift, alt}; }
constexpr bool C = true;   // readability in the table below
constexpr bool S = true;
constexpr bool A = true;
constexpr bool _ = false;

struct KeyName {
    WORD vk;
    const wchar_t* names[3];  // names[0] is the canonical spelling
};

const std::vector<KeyName>& NamedKeys() {
    static const std::vector<KeyName> keys = {
        {VK_RETURN, {L"Enter", L"Return", nullptr}},
        {VK_ESCAPE, {L"Esc", L"Escape", nullptr}},
        {VK_TAB, {L"Tab", nullptr, nullptr}},
        {VK_SPACE, {L"Space", nullptr, nullptr}},
        {VK_BACK, {L"Backspace", L"Back", L"BS"}},
        {VK_INSERT, {L"Insert", L"Ins", nullptr}},
        {VK_DELETE, {L"Delete", L"Del", nullptr}},
        {VK_HOME, {L"Home", nullptr, nullptr}},
        {VK_END, {L"End", nullptr, nullptr}},
        {VK_PRIOR, {L"PgUp", L"PageUp", nullptr}},
        {VK_NEXT, {L"PgDn", L"PageDown", nullptr}},
        {VK_UP, {L"Up", nullptr, nullptr}},
        {VK_DOWN, {L"Down", nullptr, nullptr}},
        {VK_LEFT, {L"Left", nullptr, nullptr}},
        {VK_RIGHT, {L"Right", nullptr, nullptr}},
        {VK_ADD, {L"Num+", L"NumPlus", nullptr}},
        {VK_SUBTRACT, {L"Num-", L"NumMinus", nullptr}},
        {VK_MULTIPLY, {L"Num*", L"NumMultiply", nullptr}},
        {VK_DIVIDE, {L"Num/", L"NumDivide", nullptr}},
        {VK_DECIMAL, {L"Num.", L"NumDecimal", nullptr}},
        {VK_PAUSE, {L"Pause", nullptr, nullptr}},
        {VK_APPS, {L"Apps", L"Menu", nullptr}},
    };
    return keys;
}

std::wstring Lower(std::wstring_view s) {
    std::wstring out(s);
    for (auto& c : out) c = static_cast<wchar_t>(std::towlower(c));
    return out;
}

std::wstring Trim(std::wstring_view s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring_view::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return std::wstring(s.substr(b, e - b + 1));
}

std::optional<WORD> ParseKeyName(const std::wstring& lowerName) {
    if (lowerName.size() == 1) {
        wchar_t c = lowerName[0];
        if (c >= L'a' && c <= L'z') return static_cast<WORD>(std::towupper(c));
        if (c >= L'0' && c <= L'9') return static_cast<WORD>(c);
    }
    if (lowerName.size() >= 2 && lowerName[0] == L'f') {
        bool digits = std::all_of(lowerName.begin() + 1, lowerName.end(), [](wchar_t c) { return std::iswdigit(c); });
        if (digits && lowerName.size() <= 3) {
            int n = std::stoi(lowerName.substr(1));
            if (n >= 1 && n <= 24) return static_cast<WORD>(VK_F1 + n - 1);
        }
    }
    if (lowerName.size() == 4 && lowerName.compare(0, 3, L"num") == 0 && std::iswdigit(lowerName[3])) {
        return static_cast<WORD>(VK_NUMPAD0 + (lowerName[3] - L'0'));
    }
    for (const KeyName& k : NamedKeys()) {
        if (std::any_of(std::begin(k.names), std::end(k.names),
                        [&](const wchar_t* n) { return n && Lower(n) == lowerName; }))
            return k.vk;
    }
    return std::nullopt;
}

std::wstring KeyNameOf(WORD vk) {
    if ((vk >= 'A' && vk <= 'Z') || (vk >= '0' && vk <= '9')) return std::wstring(1, static_cast<wchar_t>(vk));
    if (vk >= VK_F1 && vk <= VK_F24) return L"F" + std::to_wstring(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return L"Num" + std::to_wstring(vk - VK_NUMPAD0);
    const auto& named = NamedKeys();
    auto it = std::find_if(std::begin(named), std::end(named), [&](const KeyName& k) { return k.vk == vk; });
    if (it != std::end(named)) return it->names[0];
    wchar_t buf[16];
    swprintf_s(buf, L"VK%02X", vk);
    return buf;
}

std::wstring PluginKeyName(const PluginCommandKey& key) {
    return L"plugin:" + key.first + L"/" + std::to_wstring(key.second);
}

}  // namespace

// ---------------------------------------------------------------------------
// Chords

std::optional<KeyChord> ParseKeyChord(std::wstring_view text) {
    std::wstring s;
    for (wchar_t c : text) {
        if (c != L' ' && c != L'\t') s += static_cast<wchar_t>(std::towlower(c));
    }
    if (s.empty()) return std::nullopt;

    std::vector<std::wstring> tokens;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i) {
        if (i == s.size() || s[i] == L'+') {
            tokens.push_back(s.substr(start, i - start));
            start = i + 1;
        }
    }
    // "Num+" ends in the separator itself: ["...", "num", ""].
    std::wstring keyName = tokens.back();
    size_t modifierCount = tokens.size() - 1;
    if (keyName.empty()) {
        if (tokens.size() < 2 || tokens[tokens.size() - 2] != L"num") return std::nullopt;
        keyName = L"num+";
        modifierCount = tokens.size() - 2;
    }

    KeyChord chord;
    for (size_t i = 0; i < modifierCount; ++i) {
        const std::wstring& m = tokens[i];
        if (m == L"ctrl" || m == L"control") chord.ctrl = true;
        else if (m == L"shift") chord.shift = true;
        else if (m == L"alt") chord.alt = true;
        else return std::nullopt;
    }
    auto vk = ParseKeyName(keyName);
    if (!vk) return std::nullopt;
    chord.vk = *vk;
    return chord;
}

std::wstring FormatKeyChord(const KeyChord& chord) {
    std::wstring s;
    if (chord.ctrl) s += L"Ctrl+";
    if (chord.alt) s += L"Alt+";
    if (chord.shift) s += L"Shift+";
    return s + KeyNameOf(chord.vk);
}

std::wstring FormatKeyChords(const std::vector<KeyChord>& chords) {
    std::wstring s;
    for (const KeyChord& c : chords) {
        if (!s.empty()) s += L", ";
        s += FormatKeyChord(c);
    }
    return s;
}

bool IsFixedChord(const KeyChord& chord) {
    if (chord.vk == VK_TAB && !chord.ctrl && !chord.alt) return true;  // Tab and Shift+Tab
    if (chord.ctrl || chord.shift || chord.alt) return false;
    switch (chord.vk) {
        case VK_UP: case VK_DOWN: case VK_LEFT: case VK_RIGHT: case VK_PRIOR: case VK_NEXT:
        case VK_HOME: case VK_END: case VK_RETURN: case VK_ESCAPE: case VK_BACK:
            return true;
        default:
            return false;
    }
}

bool IsTypingChord(const KeyChord& chord) {
    bool printable = (chord.vk >= 'A' && chord.vk <= 'Z') || (chord.vk >= '0' && chord.vk <= '9') || chord.vk == VK_SPACE ||
                     (chord.vk >= 0xBA && chord.vk <= 0xC0) || (chord.vk >= 0xDB && chord.vk <= 0xDF) || chord.vk == 0xE2 ||
                     (chord.vk >= VK_NUMPAD0 && chord.vk <= VK_NUMPAD9) || chord.vk == VK_DECIMAL || chord.vk == VK_DIVIDE;
    if (!printable) return false;
    return (!chord.ctrl && !chord.alt) || (chord.ctrl && chord.alt);
}

bool IsMenuChord(const KeyChord& chord) {
    if (!chord.alt || chord.ctrl || chord.shift) return false;  // Alt+Shift+<letter> is a command (FOP-025)
    switch (chord.vk) {
        case 'F': case 'S': case 'C': case 'N': case 'V': case 'E':
            return true;
        default:
            return false;
    }
}

// ---------------------------------------------------------------------------
// Commands

const std::vector<CommandInfo>& Commands() {
    using I = CommandId;
    static const std::vector<CommandInfo> commands = {
        {I::Help, L"help", L"Keyboard shortcut reference", {K(VK_F1)}},
        {I::Quit, L"quit", L"Quit MyCommander", {K(VK_F10)}},
        {I::Rename, L"rename", L"Rename the cursor entry", {K(VK_F2)}},
        {I::GroupRename, L"group-rename", L"Group rename the selection", {K('M', C)}},
        {I::DriveLeft, L"drive-left", L"Choose the left panel's drive", {K(VK_F1, _, _, A)}},
        {I::DriveRight, L"drive-right", L"Choose the right panel's drive", {K(VK_F2, _, _, A)}},
        {I::CompareFolders, L"compare-folders", L"Compare the two panels' folders", {K(VK_F2, _, S)}},
        {I::SyncFolders, L"sync-folders", L"Synchronize the two panels' folders", {K(VK_F2, C, S)}},
        {I::View, L"view", L"View the cursor file", {K(VK_F3)}},
        {I::ChooseAction, L"choose-action", L"Choose a view/open action for the cursor file", {K(VK_F3, _, S)}},
        {I::Edit, L"edit", L"Edit the cursor file", {K(VK_F4)}},
        {I::Open, L"open", L"Open the cursor file with its default open action", {K(VK_RETURN, _, S)}},
        {I::Properties, L"properties", L"Show file properties", {K(VK_RETURN, _, _, A)}},
        {I::SortName, L"sort-name", L"Sort by name (again: reverse)", {K(VK_F3, C)}},
        {I::SortExtension, L"sort-extension", L"Sort by extension (again: reverse)", {K(VK_F4, C)}},
        {I::SortSize, L"sort-size", L"Sort by size (again: reverse)", {K(VK_F5, C)}},
        {I::SortTime, L"sort-time", L"Sort by modification time (again: reverse)", {K(VK_F6, C)}},
        {I::SortAttributes, L"sort-attributes", L"Sort by attributes (again: reverse)", {K(VK_F9, C)}},
        {I::Copy, L"copy", L"Copy to the other panel", {K(VK_F5)}},
        {I::CopyAs, L"copy-as", L"Copy the cursor entry under a new name", {K(VK_F5, _, S)}},
        {I::QueueCopy, L"queue-copy", L"Copy in the background", {K(VK_F5, _, _, A)}},
        {I::Move, L"move", L"Move to the other panel", {K(VK_F6)}},
        {I::QueueMove, L"queue-move", L"Move in the background", {K(VK_F6, _, _, A)}},
        {I::MakeDirectory, L"mkdir", L"Create a folder", {K(VK_F7)}},
        {I::NewFile, L"new-file", L"Create an empty file", {K(VK_F7, _, S)}},
        {I::FindFiles, L"find-files", L"Find files (name and content)", {K(VK_F7, _, _, A)}},
        {I::Delete, L"delete", L"Delete to the Recycle Bin", {K(VK_F8), K(VK_DELETE)}},
        {I::DeletePermanent, L"delete-permanent", L"Delete permanently", {K(VK_F8, _, S), K(VK_DELETE, _, S)}},
        {I::QueueDelete, L"queue-delete", L"Delete to the Recycle Bin in the background", {K(VK_F8, _, _, A)}},
        {I::QueueDeletePermanent, L"queue-delete-permanent", L"Delete permanently in the background",
         {K(VK_F8, _, S, A)}},
        {I::Shell, L"shell", L"Open a command shell here", {K(VK_F9)}},
        {I::EditSettings, L"edit-settings", L"Open the configuration file", {K(VK_F11, C)}},
        {I::Filter, L"filter", L"Filter the panel by name", {K('F', C)}},
        {I::Attributes, L"attributes", L"Change attributes", {K('A', C)}},
        {I::ToggleHidden, L"toggle-hidden", L"Show/hide hidden and system entries", {K('H', C)}},
        {I::Refresh, L"refresh", L"Re-read the panel", {K('R', C)}},
        {I::Tree, L"tree", L"Directory tree view", {K('T', C)}},
        {I::SinglePanel, L"single-panel", L"Single-panel layout (on/off)", {K(VK_F1, C)}},
        {I::CompareFiles, L"compare-files", L"Show the differences between two files", {K(VK_F3, _, _, A)}},
        {I::Hotlist, L"hotlist", L"Directory hotlist", {K('D', C)}},
        {I::JobQueue, L"job-queue", L"Background job list", {K('Q', _, _, A)}},
        {I::ToggleSelect, L"toggle-select", L"Select/unselect the cursor entry and move down", {K(VK_INSERT)}},
        {I::SelectMask, L"select-mask", L"Select by mask", {K(VK_ADD)}},
        {I::DeselectMask, L"deselect-mask", L"Unselect by mask", {K(VK_SUBTRACT)}},
        {I::SelectAll, L"select-all", L"Select everything", {K(VK_ADD, C)}},
        {I::DeselectAll, L"deselect-all", L"Unselect everything", {K(VK_SUBTRACT, C)}},
        {I::InvertSelection, L"invert-selection", L"Invert the selection", {K(VK_MULTIPLY)}},
        {I::SelectUp, L"select-up", L"Toggle selection while moving up", {K(VK_UP, _, S)}},
        {I::SelectDown, L"select-down", L"Toggle selection while moving down", {K(VK_DOWN, _, S)}},
        {I::HistoryBack, L"history-back", L"Back to the previous folder", {K(VK_LEFT, _, _, A)}},
        {I::HistoryForward, L"history-forward", L"Forward again", {K(VK_RIGHT, _, _, A)}},
        {I::CommandHistoryUp, L"command-history-up", L"Previous command-line entry", {K(VK_UP, C)}},
        {I::CommandHistoryDown, L"command-history-down", L"Next command-line entry", {K(VK_DOWN, C)}},
        {I::InsertName, L"insert-name", L"Insert the cursor/selected name(s) into the command line",
         {K(VK_RETURN, C)}},
        {I::InsertPath, L"insert-path", L"Insert the cursor/selected full path(s) into the command line",
         {K(VK_RETURN, C, S)}},
        {I::DiskUsage, L"disk-usage", L"Calculate used disk space for the selection", {K(VK_RETURN, _, S, A)}},
        {I::CopyNames, L"copy-names", L"Copy the selected names to the clipboard", {K('C', C, S)}},
        {I::ViewFull, L"view-full", L"Full (detailed) panel view", {K(VK_F7, C)}},
        {I::ViewBrief, L"view-brief", L"Brief (names only) panel view", {K(VK_F8, C)}},
        {I::SwapPanels, L"swap-panels", L"Swap the left and right panels", {K('U', C)}},
        {I::RecentFolders, L"recent-folders", L"Recently visited folders", {K(VK_F12, _, _, A)}},
        {I::LargestFiles, L"largest-files", L"Find the largest files below this folder", {K(VK_F12, C)}},
        {I::FindEmptyFolders, L"find-empty-folders", L"Find and remove empty folders", {K(VK_F7, _, S, A)}},
        {I::CycleLabel, L"cycle-label", L"Cycle the colour label of the selection", {K('L', C)}},
        {I::LabelMenu, L"label-menu", L"Colour labels: set, clear, select by label", {K('L', C, S)}},
        {I::Undo, L"undo", L"Undo the last file operation", {K('Z', C)}},
        {I::PasteFromExplorer, L"paste-from-explorer", L"Paste files copied or cut in Explorer", {K('V', _, S, A)}},
        {I::ReverseSearch, L"reverse-search", L"Search the command history by a fragment", {K('R', _, _, A)}},
        {I::CompletePath, L"complete-path", L"Complete the path typed on the command line", {K(VK_SPACE, C)}},
    };
    return commands;
}

const CommandInfo* FindCommand(std::wstring_view name) {
    std::wstring lower = Lower(name);
    const auto& commands = Commands();
    auto it = std::find_if(commands.begin(), commands.end(), [&](const CommandInfo& c) { return lower == c.name; });
    return it == commands.end() ? nullptr : &*it;
}

const CommandInfo& CommandById(CommandId id) {
    const auto& commands = Commands();
    auto it = std::find_if(commands.begin(), commands.end(), [&](const CommandInfo& c) { return c.id == id; });
    return it == commands.end() ? commands.front() : *it;
}

std::optional<CommandId> CommandForDefaultChord(const KeyChord& chord) {
    for (const CommandInfo& c : Commands()) {
        if (std::find(c.defaults.begin(), c.defaults.end(), chord) != c.defaults.end()) return c.id;
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------
// KeyMap

void KeyMap::Rebuild() {
    byChord_.clear();
    for (const auto& [id, chords] : keys_) {
        for (const KeyChord& c : chords) byChord_.emplace(c, id);
    }
}

KeyMap KeyMap::Defaults() {
    KeyMap m;
    for (const CommandInfo& c : Commands()) m.keys_[c.id] = c.defaults;
    m.Rebuild();
    return m;
}

KeyMap KeyMap::Load(std::wstring_view text, std::vector<std::wstring>& warnings) {
    KeyMap m = Defaults();
    std::set<KeyChord> claimed;
    std::set<CommandId> overridden;
    std::set<PluginCommandKey> overriddenPlugins;
    std::map<KeyChord, std::wstring> claimedBy;

    size_t lineNumber = 0;
    size_t pos = 0;
    while (pos <= text.size()) {
        size_t end = text.find(L'\n', pos);
        if (end == std::wstring_view::npos) end = text.size();
        std::wstring line(text.substr(pos, end - pos));
        pos = end + 1;
        ++lineNumber;
        size_t comment = line.find_first_of(L"#;");
        if (comment != std::wstring::npos) line.resize(comment);
        line = Trim(line);
        if (line.empty()) {
            if (end == text.size()) break;
            continue;
        }
        std::wstring where = L"keys.conf line " + std::to_wstring(lineNumber) + L": ";

        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos) {
            warnings.push_back(where + L"expected \"command = key\"; ignored.");
            continue;
        }
        std::wstring name = Lower(Trim(line.substr(0, eq)));
        std::wstring value = Trim(line.substr(eq + 1));

        const CommandInfo* command = nullptr;
        std::optional<PluginCommandKey> plugin;
        if (name.rfind(L"plugin:", 0) == 0) {
            std::wstring rest = name.substr(7);
            size_t slash = rest.rfind(L'/');
            std::wstring idText = slash == std::wstring::npos ? L"" : rest.substr(slash + 1);
            bool numeric = !idText.empty() && idText.size() <= 9 &&
                           std::all_of(idText.begin(), idText.end(), [](wchar_t c) { return std::iswdigit(c); });
            if (slash == 0 || slash == std::wstring::npos || !numeric) {
                warnings.push_back(where + L"\"" + name + L"\" should be plugin:<plugin-id>/<number>; ignored.");
                continue;
            }
            plugin = PluginCommandKey{rest.substr(0, slash), static_cast<uint32_t>(std::stoul(idText))};
            if (!overriddenPlugins.insert(*plugin).second) {
                warnings.push_back(where + L"\"" + name + L"\" is set twice; this line is ignored.");
                continue;
            }
        } else {
            command = FindCommand(name);
            if (!command) {
                warnings.push_back(where + L"unknown command \"" + name + L"\"; ignored.");
                continue;
            }
            if (!overridden.insert(command->id).second) {
                warnings.push_back(where + L"\"" + name + L"\" is set twice; this line is ignored.");
                continue;
            }
        }
        const std::wstring owner = command ? std::wstring(command->name) : PluginKeyName(*plugin);

        std::vector<KeyChord> chords;
        bool unbind = Lower(value) == L"none";
        if (!unbind) {
            size_t start = 0;
            while (start <= value.size()) {
                size_t comma = value.find(L',', start);
                if (comma == std::wstring::npos) comma = value.size();
                std::wstring keyText = Trim(value.substr(start, comma - start));
                start = comma + 1;
                if (keyText.empty()) {
                    if (comma == value.size()) break;
                    continue;
                }
                auto chord = ParseKeyChord(keyText);
                if (!chord) {
                    warnings.push_back(where + L"\"" + keyText + L"\" isn't a key MyCommander knows; skipped.");
                } else if (IsFixedChord(*chord)) {
                    warnings.push_back(where + FormatKeyChord(*chord) +
                                       L" moves the cursor or edits the command line and can't be rebound; skipped.");
                } else if (IsTypingChord(*chord)) {
                    warnings.push_back(where + FormatKeyChord(*chord) +
                                       L" types a character (add Ctrl or Alt); skipped.");
                } else if (IsMenuChord(*chord)) {
                    warnings.push_back(where + FormatKeyChord(*chord) + L" opens a menu; skipped.");
                } else if (claimed.count(*chord)) {
                    warnings.push_back(where + FormatKeyChord(*chord) + L" is already used by \"" +
                                       claimedBy[*chord] + L"\"; skipped.");
                } else if (std::find(chords.begin(), chords.end(), *chord) == chords.end()) {
                    chords.push_back(*chord);
                }
                if (comma == value.size()) break;
            }
            if (chords.empty()) {
                warnings.push_back(where + L"no usable key for \"" + owner + L"\"; its previous keys stay.");
                if (command) overridden.erase(command->id);
                else overriddenPlugins.erase(*plugin);
                continue;
            }
        }
        for (const KeyChord& c : chords) {
            claimed.insert(c);
            claimedBy[c] = owner;
        }
        if (command) m.keys_[command->id] = chords;
        else m.pluginKeys_[*plugin] = chords;
    }

    // A key the user gave to one command is taken from whichever command had it by default.
    for (auto& [id, chords] : m.keys_) {
        if (overridden.count(id)) continue;
        for (auto it = chords.begin(); it != chords.end();) {
            if (claimed.count(*it)) {
                warnings.push_back(L"keys.conf: " + FormatKeyChord(*it) + L" now belongs to \"" + claimedBy[*it] +
                                   L"\", so \"" + CommandById(id).name + L"\" no longer has it.");
                it = chords.erase(it);
            } else {
                ++it;
            }
        }
    }

    // Quitting must always be possible from the keyboard (KEY-001).
    if (m.keys_[CommandId::Quit].empty()) {
        KeyChord f10 = K(VK_F10);
        for (auto& [id, chords] : m.keys_) chords.erase(std::remove(chords.begin(), chords.end(), f10), chords.end());
        for (auto& [key, chords] : m.pluginKeys_)
            chords.erase(std::remove(chords.begin(), chords.end(), f10), chords.end());
        m.keys_[CommandId::Quit] = {f10};
        warnings.push_back(L"keys.conf: \"quit\" had no key left, so F10 quits again.");
    }

    m.Rebuild();
    return m;
}

const std::vector<KeyChord>& KeyMap::KeysFor(CommandId id) const {
    static const std::vector<KeyChord> none;
    auto it = keys_.find(id);
    return it == keys_.end() ? none : it->second;
}

std::optional<CommandId> KeyMap::CommandFor(const KeyChord& chord) const {
    auto it = byChord_.find(chord);
    if (it == byChord_.end()) return std::nullopt;
    return it->second;
}

bool KeyMap::IsCustomized(CommandId id) const { return KeysFor(id) != CommandById(id).defaults; }

std::optional<std::vector<KeyChord>> KeyMap::PluginKeysFor(const PluginCommandKey& key) const {
    auto it = pluginKeys_.find(key);
    if (it == pluginKeys_.end()) return std::nullopt;
    return it->second;
}

bool KeyMap::IsPluginChord(const KeyChord& chord) const {
    for (const auto& [key, chords] : pluginKeys_) {
        if (std::find(chords.begin(), chords.end(), chord) != chords.end()) return true;
    }
    return false;
}

bool KeyMap::IsFreed(const KeyChord& chord) const {
    return CommandForDefaultChord(chord).has_value() && !byChord_.count(chord) && !IsPluginChord(chord);
}

KeyTranslation KeyMap::Translate(const KeyChord& pressed, bool inPanelContent) const {
    if (auto command = CommandFor(pressed)) {
        // The switch already handles every default key of a command (e.g.
        // both F8 and Delete), so only a non-default key needs rewriting.
        const auto& defaults = CommandById(*command).defaults;
        if (std::find(defaults.begin(), defaults.end(), pressed) != defaults.end()) return {KeyRoute::Unchanged, pressed};
        return {KeyRoute::Remapped, defaults.front()};
    }
    if (IsPluginChord(pressed)) return {KeyRoute::Unchanged, pressed};
    if (IsFreed(pressed)) return {inPanelContent ? KeyRoute::Unchanged : KeyRoute::Dropped, pressed};
    return {KeyRoute::Unchanged, pressed};
}

std::vector<KeyChord> KeyMap::AllBuiltinChords() const {
    std::vector<KeyChord> out;
    for (const auto& [chord, id] : byChord_) out.push_back(chord);
    return out;
}

namespace {
struct InjectedKey {
    KeyChord chord;
    ULONGLONG at = 0;
};
std::vector<InjectedKey>& InjectedKeys() {
    static std::vector<InjectedKey> keys;
    return keys;
}
}  // namespace

void NoteInjectedKey(const KeyChord& chord) { InjectedKeys().push_back({chord, GetTickCount64()}); }

bool ConsumeInjectedKey(const KeyChord& chord) {
    auto& keys = InjectedKeys();
    ULONGLONG now = GetTickCount64();
    keys.erase(std::remove_if(keys.begin(), keys.end(), [&](const InjectedKey& k) { return now - k.at > 1000; }),
               keys.end());
    auto it = std::find_if(keys.begin(), keys.end(), [&](const InjectedKey& k) { return k.chord == chord; });
    if (it == keys.end()) return false;
    keys.erase(it);
    return true;
}

std::optional<std::wstring> KeyMap::RelabelKeyText(std::wstring_view keyText) const {
    // "F8 / Delete", "Alt+F1 / Alt+F2", "Ctrl+F3": every part must be a key;
    // a part whose command was rebound shows the command's current keys.
    std::vector<std::wstring> parts;
    size_t start = 0;
    while (true) {
        size_t sep = keyText.find(L" / ", start);
        parts.emplace_back(keyText.substr(start, sep == std::wstring_view::npos ? std::wstring_view::npos : sep - start));
        if (sep == std::wstring_view::npos) break;
        start = sep + 3;
    }
    bool changed = false;
    std::vector<std::wstring> out;
    for (const std::wstring& part : parts) {
        auto chord = ParseKeyChord(part);
        if (!chord) return std::nullopt;  // not a plain key list (e.g. "Up / Down" is fine, "Numpad +" isn't)
        std::wstring text = part;
        if (auto id = CommandForDefaultChord(*chord); id && IsCustomized(*id)) {
            const auto& keys = KeysFor(*id);
            text = keys.empty() ? L"(unbound)" : FormatKeyChords(keys);
            changed = true;
        }
        if (out.empty() || out.back() != text) out.push_back(text);
    }
    if (!changed) return std::nullopt;
    std::wstring joined;
    for (const std::wstring& t : out) joined += (joined.empty() ? L"" : L" / ") + t;
    return joined;
}

std::wstring DefaultKeysFileText() {
    std::wstring t =
        L"# MyCommander keyboard shortcuts (KEY-005)\n"
        L"#\n"
        L"#   command = key[, key...]\n"
        L"#\n"
        L"# Remove the leading '#' from a line and change its keys. A command you list\n"
        L"# replaces all of its default keys; \"none\" unbinds it; commands not listed keep\n"
        L"# their defaults. Keys: F1..F24, A..Z, 0..9, Enter, Esc, Tab, Space, Backspace,\n"
        L"# Insert, Delete, Home, End, PgUp, PgDn, Up, Down, Left, Right, Num+, Num-,\n"
        L"# Num*, Num/, Num0..Num9, with Ctrl+, Alt+ and Shift+ in front.\n"
        L"#\n"
        L"# Not allowed: plain cursor/editing keys (arrows, PgUp/PgDn, Home/End, Enter,\n"
        L"# Esc, Backspace, Tab), keys that type a character (letters, digits,\n"
        L"# punctuation, Space -- add Ctrl or Alt), and Alt+F/S/C/N/V/E (the menus).\n"
        L"# A key you give to one command is taken from the command that had it.\n"
        L"# \"quit\" always keeps at least one key. Changes apply when you save.\n"
        L"#\n"
        L"# Plugin shortcuts: plugin:<plugin-id>/<command number> = key\n"
        L"\n";
    for (const CommandInfo& c : Commands()) {
        t += L"# " + std::wstring(c.description) + L"\n";
        t += L"# " + std::wstring(c.name) + L" = " + FormatKeyChords(c.defaults) + L"\n";
    }
    return t;
}

}  // namespace mc
