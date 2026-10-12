#include "TestFramework.h"

#include "KeyMap.h"
#include "MenuBar.h"
#include "PluginManager.h"

#include <algorithm>
#include <set>

using mc::CommandId;
using mc::FormatKeyChord;
using mc::KeyChord;
using mc::KeyMap;
using mc::KeyRoute;
using mc::ParseKeyChord;

namespace {

KeyChord K(WORD vk, bool ctrl = false, bool shift = false, bool alt = false) { return KeyChord{vk, ctrl, shift, alt}; }

KeyMap LoadText(const std::wstring& text, std::vector<std::wstring>& warnings) { return KeyMap::Load(text, warnings); }

bool Mentions(const std::vector<std::wstring>& warnings, const std::wstring& part) {
    return std::any_of(warnings.begin(), warnings.end(),
                       [&](const std::wstring& w) { return w.find(part) != std::wstring::npos; });
}

}  // namespace

// --- Chords --------------------------------------------------------------------

TEST_CASE(KeyChord_ParseVariants_KEY005) {
    CHECK(ParseKeyChord(L"F5") == std::optional<KeyChord>(K(VK_F5)));
    CHECK(ParseKeyChord(L"ctrl+shift+f2") == std::optional<KeyChord>(K(VK_F2, true, true)));
    CHECK(ParseKeyChord(L" Ctrl + F5 ") == std::optional<KeyChord>(K(VK_F5, true)));
    CHECK(ParseKeyChord(L"Alt+Enter") == std::optional<KeyChord>(K(VK_RETURN, false, false, true)));
    CHECK(ParseKeyChord(L"Return") == std::optional<KeyChord>(K(VK_RETURN)));
    CHECK(ParseKeyChord(L"Num+") == std::optional<KeyChord>(K(VK_ADD)));
    CHECK(ParseKeyChord(L"Ctrl+Num+") == std::optional<KeyChord>(K(VK_ADD, true)));
    CHECK(ParseKeyChord(L"Num-") == std::optional<KeyChord>(K(VK_SUBTRACT)));
    CHECK(ParseKeyChord(L"Del") == std::optional<KeyChord>(K(VK_DELETE)));
    CHECK(ParseKeyChord(L"Delete") == std::optional<KeyChord>(K(VK_DELETE)));
    CHECK(ParseKeyChord(L"control+c") == std::optional<KeyChord>(K('C', true)));
    CHECK(ParseKeyChord(L"F24") == std::optional<KeyChord>(K(VK_F24)));
    CHECK(!ParseKeyChord(L"Ctrl+").has_value());
    CHECK(!ParseKeyChord(L"Foo+F5").has_value());
    CHECK(!ParseKeyChord(L"F25").has_value());
    CHECK(!ParseKeyChord(L"").has_value());
    CHECK(!ParseKeyChord(L"Ctrl++F5").has_value());
}

TEST_CASE(KeyChord_FormatRoundTripsEveryDefault_KEY005) {
    for (const auto& c : mc::Commands()) {
        for (const KeyChord& chord : c.defaults) {
            CHECK(ParseKeyChord(FormatKeyChord(chord)) == std::optional<KeyChord>(chord));
        }
    }
    CHECK(FormatKeyChord(K(VK_F8, false, true, true)) == L"Alt+Shift+F8");
    CHECK(FormatKeyChord(K(VK_ADD, true)) == L"Ctrl+Num+");
}

// --- Defaults --------------------------------------------------------------------

TEST_CASE(KeyMap_DefaultsAreConsistent_KEY005) {
    std::set<KeyChord> seen;
    std::set<std::wstring> names;
    for (const auto& c : mc::Commands()) {
        CHECK(!c.defaults.empty());
        CHECK(names.insert(c.name).second);  // unique names
        for (const KeyChord& chord : c.defaults) {
            CHECK(seen.insert(chord).second);  // no chord is the default of two commands
            CHECK(!mc::IsFixedChord(chord));
            CHECK(!mc::IsTypingChord(chord));
            CHECK(!mc::IsMenuChord(chord));
        }
    }
    KeyMap m = KeyMap::Defaults();
    CHECK(m.CommandFor(K(VK_F5)) == std::optional<CommandId>(CommandId::Copy));
    CHECK(m.CommandFor(K(VK_DELETE)) == std::optional<CommandId>(CommandId::Delete));
    CHECK(!m.IsCustomized(CommandId::Copy));
}

TEST_CASE(KeyMap_DefaultsAreTheBuiltinShortcutInventory_KEY005) {
    // Every default chord is reserved against plugins, so the table can't drift from the inventory.
    const auto& inventory = mc::BuiltinShortcutInventory();
    for (const auto& c : mc::Commands()) {
        for (const KeyChord& chord : c.defaults) {
            mc::PluginShortcutKey key{chord.vk, chord.ctrl, chord.shift, chord.alt};
            CHECK(std::find(inventory.begin(), inventory.end(), key) != inventory.end());
        }
    }
}

TEST_CASE(KeyMap_MenuChordsMatchTheMenuBar_KEY005) {
    for (const auto& menu : mc::BuildMenus()) {
        CHECK(mc::IsMenuChord(K(static_cast<WORD>(menu.mnemonic), false, false, true)));
    }
    CHECK(!mc::IsMenuChord(K('Q', false, false, true)));
}

// --- Load ---------------------------------------------------------------------

TEST_CASE(KeyMap_Load_OverrideUnbindMultipleAndFreed_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"# comment\ncopy = Ctrl+C, F12   ; trailing comment\nshell = none\n\nview = Alt+V3\n", w);
    CHECK((m.KeysFor(CommandId::Copy) == std::vector<KeyChord>{K('C', true), K(VK_F12)}));
    CHECK(m.KeysFor(CommandId::Shell).empty());
    CHECK(m.KeysFor(CommandId::Move) == std::vector<KeyChord>{K(VK_F6)});  // not listed: default
    CHECK(m.IsCustomized(CommandId::Copy));
    CHECK(m.IsFreed(K(VK_F5)));
    CHECK(m.IsFreed(K(VK_F9)));
    CHECK(!m.IsFreed(K(VK_F6)));
    CHECK(Mentions(w, L"Alt+V3"));  // bad key reported, line skipped
    CHECK(m.KeysFor(CommandId::View) == std::vector<KeyChord>{K(VK_F3)});
}

TEST_CASE(KeyMap_Load_RejectedKeysKeepPreviousBinding_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"copy = Up\nmove = X\nmkdir = Shift+7\nview = Alt+F\nedit = Ctrl+Alt+Q\n"
                        L"nonsense = F5\nrename\n",
                        w);
    CHECK(m.KeysFor(CommandId::Copy) == std::vector<KeyChord>{K(VK_F5)});
    CHECK(m.KeysFor(CommandId::Move) == std::vector<KeyChord>{K(VK_F6)});
    CHECK(m.KeysFor(CommandId::MakeDirectory) == std::vector<KeyChord>{K(VK_F7)});
    CHECK(m.KeysFor(CommandId::View) == std::vector<KeyChord>{K(VK_F3)});
    CHECK(m.KeysFor(CommandId::Edit) == std::vector<KeyChord>{K(VK_F4)});
    CHECK(Mentions(w, L"can't be rebound"));
    CHECK(Mentions(w, L"types a character"));
    CHECK(Mentions(w, L"opens a menu"));
    CHECK(Mentions(w, L"unknown command \"nonsense\""));
    CHECK(Mentions(w, L"line 7"));  // "rename" with no '='
}

TEST_CASE(KeyMap_Load_ConflictsUserWinsFirstLineWins_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"move = F5\ncopy = F6\nview = F5, Ctrl+Q\nview = F12\n", w);
    CHECK(m.CommandFor(K(VK_F5)) == std::optional<CommandId>(CommandId::Move));
    CHECK(m.CommandFor(K(VK_F6)) == std::optional<CommandId>(CommandId::Copy));
    CHECK(m.KeysFor(CommandId::View) == std::vector<KeyChord>{K('Q', true)});  // F5 already moved; second line ignored
    CHECK(Mentions(w, L"already used by \"move\""));
    CHECK(Mentions(w, L"set twice"));

    // A user key taken from another command's defaults.
    w.clear();
    m = LoadText(L"shell = Ctrl+F\n", w);
    CHECK(m.CommandFor(K('F', true)) == std::optional<CommandId>(CommandId::Shell));
    CHECK(m.KeysFor(CommandId::Filter).empty());
    CHECK(Mentions(w, L"no longer has it"));
}

TEST_CASE(KeyMap_Load_QuitAlwaysKeepsAKey_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"quit = none\nshell = F10\n", w);
    CHECK(m.KeysFor(CommandId::Quit) == std::vector<KeyChord>{K(VK_F10)});
    CHECK(m.KeysFor(CommandId::Shell).empty());
    CHECK(Mentions(w, L"F10 quits again"));
}

TEST_CASE(KeyMap_Load_PluginOverrides_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"plugin:com.example.hello/1 = Ctrl+Shift+H\nplugin:com.x/2 = none\nplugin:bad = F12\n", w);
    auto hello = m.PluginKeysFor({L"com.example.hello", 1});
    CHECK(hello.has_value() && *hello == std::vector<KeyChord>{K('H', true, true)});
    auto x = m.PluginKeysFor({L"com.x", 2});
    CHECK(x.has_value() && x->empty());
    CHECK(!m.PluginKeysFor({L"com.other", 1}).has_value());
    CHECK(m.IsPluginChord(K('H', true, true)));
    CHECK(Mentions(w, L"plugin:<plugin-id>/<number>"));
}

TEST_CASE(KeyMap_DefaultFileTextLoadsBackToDefaults_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(mc::DefaultKeysFileText(), w);
    CHECK(w.empty());
    for (const auto& c : mc::Commands()) CHECK(m.KeysFor(c.id) == c.defaults);

    // Uncommenting a default line verbatim is also clean.
    w.clear();
    m = LoadText(L"delete = F8, Delete\n", w);
    CHECK(w.empty());
    CHECK(!m.IsCustomized(CommandId::Delete));
}

// --- Help, hint bar, menu, plugins, injected keys ------------------------------

TEST_CASE(KeyMap_RelabelKeyText_ForHelpRows_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"copy = Ctrl+C\ndelete = Ctrl+Delete\ndrive-left = none\n", w);
    CHECK(m.RelabelKeyText(L"F5") == std::optional<std::wstring>(L"Ctrl+C"));
    CHECK(m.RelabelKeyText(L"F8 / Delete") == std::optional<std::wstring>(L"Ctrl+Delete"));
    CHECK(m.RelabelKeyText(L"Alt+F1 / Alt+F2") == std::optional<std::wstring>(L"(unbound) / Alt+F2"));
    CHECK(!m.RelabelKeyText(L"F6").has_value());          // unchanged command
    CHECK(!m.RelabelKeyText(L"Up / Down").has_value());   // fixed keys
    CHECK(!m.RelabelKeyText(L"Numpad + / -").has_value());  // not a plain key list
    CHECK(!KeyMap::Defaults().RelabelKeyText(L"F5").has_value());
}

TEST_CASE(KeyMap_RelabelHintSegments_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"copy = Ctrl+C\nmove = none\n", w);
    auto segments = mc::RelabelHintSegments(mc::BuildDefaultHintSegments(mc::HintContext::File), m);
    auto has = [&](const std::wstring& label) {
        return std::any_of(segments.begin(), segments.end(), [&](const mc::HintSegment& s) { return s.label == label; });
    };
    CHECK(has(L"Ctrl+C Copy"));
    CHECK(!has(L"F5 Copy"));
    CHECK(!has(L"F6 Move"));   // unbound: dropped
    CHECK(has(L"F3 View"));    // unchanged
    // The click action stays the default key (injected, so it bypasses the remap).
    auto copy = std::find_if(segments.begin(), segments.end(), [](const mc::HintSegment& s) { return s.label == L"Ctrl+C Copy"; });
    CHECK(copy != segments.end() && copy->action && copy->action->virtualKeyCode == VK_F5);

    // With no map, BuildHintSegments is exactly the defaults (IS-0007 layout unchanged).
    CHECK(mc::ActiveHintKeyMap() == nullptr);
    CHECK(mc::BuildHintSegments(mc::HintContext::File).size() == mc::BuildDefaultHintSegments(mc::HintContext::File).size());
}

TEST_CASE(KeyMap_InjectedKeysBypassOnce_KEY005) {
    CHECK(!mc::ConsumeInjectedKey(K(VK_F5)));
    mc::NoteInjectedKey(K(VK_F5));
    mc::NoteInjectedKey(K(VK_F5, false, false, true));
    CHECK(!mc::ConsumeInjectedKey(K(VK_F6)));
    CHECK(mc::ConsumeInjectedKey(K(VK_F5, false, false, true)));
    CHECK(mc::ConsumeInjectedKey(K(VK_F5)));
    CHECK(!mc::ConsumeInjectedKey(K(VK_F5)));  // consumed exactly once
}

TEST_CASE(KeyMap_PluginInventoryFollowsTheMap_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"copy = Ctrl+Shift+C\n", w);
    auto inventory = mc::BuiltinShortcutInventory(m);
    auto contains = [&](const KeyChord& c) {
        return std::find(inventory.begin(), inventory.end(), mc::PluginShortcutKey{c.vk, c.ctrl, c.shift, c.alt}) !=
               inventory.end();
    };
    CHECK(contains(K('C', true, true)));  // the user's key is reserved
    CHECK(!contains(K(VK_F5)));           // freed: plugins may take it now
    CHECK(contains(K(VK_F6)));            // untouched default
    CHECK(contains(K(VK_RETURN)));        // fixed
}

TEST_CASE(KeyMap_MenuShowsKeysAndEditKeyBindingsItem_KEY005) {
    const auto& menus = mc::BuildMenus();
    const mc::TopMenu& settings = menus[5];
    int idx = mc::FindItemByMnemonic(settings, L'K');
    CHECK(idx >= 0);
    if (idx >= 0) {
        CHECK(settings.items[static_cast<size_t>(idx)].label == L"Edit key bindings");
        CHECK(settings.items[static_cast<size_t>(idx)].kind == mc::MenuItemKind::EditKeyBindings);
    }
    // The dropdown widens to fit a right-aligned shortcut.
    mc::TopMenu file = menus[0];
    SHORT plain = mc::LayOutOpenDropdown(file, 0, 200).width;
    for (auto& item : file.items) item.keyText = L"Ctrl+Shift+F12";
    CHECK(mc::LayOutOpenDropdown(file, 0, 200).width > plain);
}

// --- Translate --------------------------------------------------------------------

TEST_CASE(KeyMap_Translate_RemapsDropsAndPassesThrough_KEY005) {
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"copy = Ctrl+C\nmove = F5\nplugin:p/1 = Ctrl+Shift+P\n", w);
    auto t = m.Translate(K('C', true), false);
    CHECK(t.route == KeyRoute::Remapped && t.chord == K(VK_F5));  // copy's canonical key
    t = m.Translate(K(VK_F5), false);
    CHECK(t.route == KeyRoute::Remapped && t.chord == K(VK_F6));  // F5 now means move
    t = m.Translate(K(VK_F6), false);
    CHECK(t.route == KeyRoute::Dropped);  // move's old key, unused now
    t = m.Translate(K(VK_F6), /*inPanelContent=*/true);
    CHECK(t.route == KeyRoute::Unchanged);  // a plugin/tree panel keeps its own keys
    CHECK(m.Translate(K(VK_F7), false).route == KeyRoute::Unchanged);           // untouched default
    CHECK(m.Translate(K(VK_UP), false).route == KeyRoute::Unchanged);           // fixed key
    CHECK(m.Translate(K('P', true, true), false).route == KeyRoute::Unchanged);  // plugin key
    CHECK(m.Translate(K('X'), false).route == KeyRoute::Unchanged);             // typing

    KeyMap d = KeyMap::Defaults();
    CHECK(d.Translate(K(VK_DELETE), false).route == KeyRoute::Unchanged);  // a default's own extra key
}

TEST_CASE(KeyMap_SinglePanel_DefaultCtrlF1AndRebindable_UI013) {
    KeyMap d = KeyMap::Defaults();
    CHECK(d.KeysFor(CommandId::SinglePanel) == std::vector<KeyChord>{K(VK_F1, true)});
    CHECK(d.CommandFor(K(VK_F1, true)) == std::optional<CommandId>(CommandId::SinglePanel));
    std::vector<std::wstring> w;
    KeyMap m = LoadText(L"single-panel = Ctrl+Shift+S\n", w);
    CHECK(w.empty());
    auto t = m.Translate(K('S', true, true), false);
    CHECK(t.route == mc::KeyRoute::Remapped && t.chord == K(VK_F1, true));
}

TEST_CASE(KeyMap_CompareFiles_DefaultAltF3_SRC008) {
    KeyMap d = KeyMap::Defaults();
    CHECK(d.KeysFor(CommandId::CompareFiles) == std::vector<KeyChord>{K(VK_F3, false, false, true)});
    CHECK(d.CommandFor(K(VK_F3, false, false, true)) == std::optional<CommandId>(CommandId::CompareFiles));
}
