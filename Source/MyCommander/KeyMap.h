#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace mc {

// KEY-005: user-rebindable keyboard shortcuts, read from
// %APPDATA%\MyCommander\keys.conf. Pure (no Console/Panel dependency), own
// KeyMapTests.cpp. main.cpp keeps its key-dispatch switch working on the
// *default* keys and translates each pressed chord through Translate()
// first; menus and hint clicks inject default keys that bypass it.

struct KeyChord {
    WORD vk = 0;
    bool ctrl = false;
    bool shift = false;
    bool alt = false;
    bool operator==(const KeyChord& o) const {
        return vk == o.vk && ctrl == o.ctrl && shift == o.shift && alt == o.alt;
    }
    bool operator<(const KeyChord& o) const {
        if (vk != o.vk) return vk < o.vk;
        if (ctrl != o.ctrl) return ctrl < o.ctrl;
        if (shift != o.shift) return shift < o.shift;
        return alt < o.alt;
    }
};

// "Ctrl+Shift+F2", "alt + enter", "Num+", "Del", "F5" (case-insensitive,
// spaces ignored). nullopt for anything unrecognized.
std::optional<KeyChord> ParseKeyChord(std::wstring_view text);
// Canonical spelling ("Ctrl+Alt+Shift+Key"), used by help, hints, menus and messages.
std::wstring FormatKeyChord(const KeyChord& chord);
std::wstring FormatKeyChords(const std::vector<KeyChord>& chords);  // "F8, Delete"; L"" when empty

enum class CommandId {
    Help, Quit, Rename, GroupRename, DriveLeft, DriveRight, CompareFolders, SyncFolders,
    View, ChooseAction, Edit, Open, Properties,
    SortName, SortExtension, SortSize, SortTime, SortAttributes,
    Copy, CopyAs, QueueCopy, Move, QueueMove, MakeDirectory, NewFile, FindFiles,
    Delete, DeletePermanent, QueueDelete, QueueDeletePermanent,
    Shell, EditSettings, Filter, Attributes, ToggleHidden, Refresh, Tree, SinglePanel, CompareFiles, Hotlist, JobQueue,
    ToggleSelect, SelectMask, DeselectMask, SelectAll, DeselectAll, InvertSelection, SelectUp, SelectDown,
    HistoryBack, HistoryForward, CommandHistoryUp, CommandHistoryDown, InsertName, InsertPath,
    // MENU-STUBS: FOP-020/SEL-008/UI-017.
    DiskUsage, CopyNames, ViewFull, ViewBrief,
    // NAV-016, NAV-017.
    SwapPanels, RecentFolders, LargestFiles,
    // SRC-009, LIST-009.
    FindEmptyFolders, CycleLabel, LabelMenu,
    // FOP-022, FOP-025.
    Undo, PasteFromExplorer,
    // CLI-008.
    ReverseSearch,
    // NAV-018.
    CompletePath,
};

struct CommandInfo {
    CommandId id;
    const wchar_t* name;         // keys.conf name, e.g. L"copy"
    const wchar_t* description;  // one line for the default file's comments
    std::vector<KeyChord> defaults;  // today's keys; defaults.front() is the canonical key main.cpp's switch handles
};
const std::vector<CommandInfo>& Commands();
const CommandInfo* FindCommand(std::wstring_view name);
const CommandInfo& CommandById(CommandId id);
// The built-in command a *default* chord belongs to, if any.
std::optional<CommandId> CommandForDefaultChord(const KeyChord& chord);

// Keys that are never rebindable: plain cursor/editing keys and Tab.
bool IsFixedChord(const KeyChord& chord);
// Keys that would steal typing: printable keys without Ctrl/Alt, and
// Ctrl+Alt+printable (AltGr on many layouts).
bool IsTypingChord(const KeyChord& chord);
// Alt+<letter> keys that open the top menus (MenuBar.h's six mnemonics).
bool IsMenuChord(const KeyChord& chord);

// A plugin shortcut, as named in keys.conf: "plugin:<pluginId>/<commandId>".
using PluginCommandKey = std::pair<std::wstring, uint32_t>;

enum class KeyRoute { Unchanged, Remapped, Dropped };
struct KeyTranslation {
    KeyRoute route = KeyRoute::Unchanged;
    KeyChord chord;  // the chord to dispatch (the canonical one when Remapped)
};

class KeyMap {
public:
    static KeyMap Defaults();
    // Applies keys.conf text over the defaults. Every problem becomes one
    // line in `warnings`; valid lines still apply (KEY-006).
    static KeyMap Load(std::wstring_view text, std::vector<std::wstring>& warnings);

    const std::vector<KeyChord>& KeysFor(CommandId id) const;
    std::optional<CommandId> CommandFor(const KeyChord& chord) const;
    bool IsCustomized(CommandId id) const;  // its keys differ from the defaults

    // Plugin overrides: nullopt = no line for it (the plugin's own key applies).
    std::optional<std::vector<KeyChord>> PluginKeysFor(const PluginCommandKey& key) const;
    bool IsPluginChord(const KeyChord& chord) const;

    // A default chord whose command was moved elsewhere and nothing else took.
    bool IsFreed(const KeyChord& chord) const;

    // The main loop's step: a user-bound chord becomes its command's
    // canonical chord; a freed default is dropped on the main screen but
    // passed through unchanged inside panel content (tree/plugin), whose own
    // keys must keep working; everything else is unchanged.
    KeyTranslation Translate(const KeyChord& pressed, bool inPanelContent) const;

    // Every chord bound to a built-in command right now (for the plugin conflict check).
    std::vector<KeyChord> AllBuiltinChords() const;

    // A help/hint key text ("F8 / Delete", "Ctrl+F3") with any rebound
    // command's keys substituted; nullopt when nothing in it changed or it
    // isn't a plain list of keys.
    std::optional<std::wstring> RelabelKeyText(std::wstring_view keyText) const;

private:
    std::map<CommandId, std::vector<KeyChord>> keys_;
    std::map<KeyChord, CommandId> byChord_;
    std::map<PluginCommandKey, std::vector<KeyChord>> pluginKeys_;
    void Rebuild();
};

// A fully-commented keys.conf: the rules, then every command with its
// default keys commented out. Loads back to exactly the defaults.
std::wstring DefaultKeysFileText();

// Injected key presses (menu items, hint-bar clicks, a plugin's
// SimulateKey) carry *default* keys and must reach main.cpp's switch
// untranslated. The injector notes each one; the main loop consumes the
// matching key-down instead of translating it. Entries older than about a
// second are discarded, so a lost injection can't swallow a later real key.
void NoteInjectedKey(const KeyChord& chord);
bool ConsumeInjectedKey(const KeyChord& chord);

}  // namespace mc
