#include "TestFramework.h"

#include "PluginManager.h"

using mc::BuiltinShortcutInventory;
using mc::ConflictsWithBuiltinShortcut;
using mc::IsPluginApiCompatible;
using mc::MenuItemKind;
using mc::MergePluginMenuItems;
using mc::PluginMenuEntry;
using mc::PluginShortcutKey;
using mc::TopMenu;

// --- EXT-003: viewer plugins (pure parts) -------------------------------------

TEST_CASE(ChooseViewRoute_FileActionThenDefaultPluginThenBuiltIn_EXT003) {
    using mc::ViewerCandidate;
    using mc::ViewRoute;
    std::vector<ViewerCandidate> offerOnly = {{L"a", L"Hex dump", kMcViewOffer}};
    std::vector<ViewerCandidate> twoDefaults = {
        {L"a", L"Hex dump", kMcViewOffer}, {L"b", L"Markdown", kMcViewDefault}, {L"c", L"Other", kMcViewDefault}};
    size_t index = 99;
    CHECK(mc::ChooseViewRoute(true, twoDefaults, index) == ViewRoute::FileAction);  // the user's choice wins
    CHECK(mc::ChooseViewRoute(false, twoDefaults, index) == ViewRoute::Plugin);
    CHECK(index == 1);  // the first plugin claiming Default
    CHECK(mc::ChooseViewRoute(false, offerOnly, index) == ViewRoute::BuiltIn);  // Offer is Shift+F3 only
    CHECK(mc::ChooseViewRoute(false, {}, index) == ViewRoute::BuiltIn);
}

TEST_CASE(ViewLineSink_CapsLinesAndBytes_EXT003) {
    mc::ViewLineSink lines(3, 1 << 20);
    lines.Append(L"one");
    lines.Append(nullptr);  // an empty line
    lines.Append(L"three");
    lines.Append(L"four");  // over the line cap
    CHECK(lines.Lines().size() == 3 && lines.Truncated());
    CHECK(lines.Text() == std::wstring(L"one\r\n\r\nthree\r\n") + mc::kViewTruncatedMarker);

    mc::ViewLineSink bytes(1000, 40);  // (len + 2) * 2 bytes per line
    bytes.Append(L"123456");           // 16 bytes
    bytes.Append(L"123456");           // 32
    bytes.Append(L"123456");           // 48 > 40: dropped
    CHECK(bytes.Lines().size() == 2 && bytes.Truncated());

    mc::ViewLineSink plain;
    plain.SetTitle(L"Title");
    plain.Append(L"x");
    CHECK(!plain.Truncated() && plain.Text() == L"x\r\n" && plain.Title() == L"Title");
}

TEST_CASE(IsPluginApiCompatible_HostAcceptsApi110Plugins_EXT003) {
    CHECK(IsPluginApiCompatible(MC_PLUGIN_API_VERSION_MAJOR, MC_PLUGIN_API_VERSION_MINOR, 1, 10));
    CHECK(MC_PLUGIN_API_VERSION_MINOR >= 10);
}

// --- IsPluginApiCompatible (FEA-0002 §16) ------------------------------------

TEST_CASE(IsPluginApiCompatible_ExactMatch_IsCompatible) {
    CHECK(IsPluginApiCompatible(1, 0, 1, 0));
}

TEST_CASE(IsPluginApiCompatible_OlderMinor_IsCompatible) {
    CHECK(IsPluginApiCompatible(1, 3, 1, 1));
}

TEST_CASE(IsPluginApiCompatible_NewerMinor_IsRejected) {
    CHECK(!IsPluginApiCompatible(1, 0, 1, 1));
}

TEST_CASE(IsPluginApiCompatible_DifferentMajor_IsRejectedEvenIfMinorIsOlder) {
    CHECK(!IsPluginApiCompatible(2, 0, 1, 0));
    CHECK(!IsPluginApiCompatible(1, 0, 2, 0));
}

// --- MergePluginMenuItems (FEA-0002 §6) --------------------------------------

namespace {
std::vector<TopMenu> MakeBuiltins() {
    return {
        TopMenu{L"File", L'F', {}},
        TopMenu{L"View", L'V', {}},
    };
}
}  // namespace

TEST_CASE(MergePluginMenuItems_AppendsToMatchingTopMenuByMnemonic) {
    auto builtins = MakeBuiltins();
    std::vector<PluginMenuEntry> overlay = {
        PluginMenuEntry{L"com.example.plugin", L'F', L"My Command", L'M', 42, true},
    };

    auto merged = MergePluginMenuItems(builtins, overlay);

    CHECK(merged.size() == 2);
    CHECK(merged[0].items.size() == 1);
    CHECK(merged[1].items.empty());
    if (merged[0].items.size() == 1) {
        CHECK(merged[0].items[0].label == L"My Command");
        CHECK(merged[0].items[0].kind == MenuItemKind::PluginCommand);
        CHECK(merged[0].items[0].pluginId == L"com.example.plugin");
        CHECK(merged[0].items[0].pluginCommandId == 42);
    }
}

TEST_CASE(MergePluginMenuItems_MnemonicMatchIsCaseInsensitive) {
    auto builtins = MakeBuiltins();
    std::vector<PluginMenuEntry> overlay = {
        PluginMenuEntry{L"p", L'f', L"Lowercase Target", L'X', 1, true},
    };

    auto merged = MergePluginMenuItems(builtins, overlay);

    CHECK(merged[0].items.size() == 1);
}

TEST_CASE(MergePluginMenuItems_UnmatchedTopMenuMnemonicIsSkipped) {
    auto builtins = MakeBuiltins();
    std::vector<PluginMenuEntry> overlay = {
        PluginMenuEntry{L"p", L'Z', L"No Such Menu", L'X', 1, true},
    };

    auto merged = MergePluginMenuItems(builtins, overlay);

    CHECK(merged[0].items.empty());
    CHECK(merged[1].items.empty());
}

TEST_CASE(MergePluginMenuItems_DisabledEntryIsLeftOutEntirely) {
    auto builtins = MakeBuiltins();
    std::vector<PluginMenuEntry> overlay = {
        PluginMenuEntry{L"p", L'F', L"Disabled Item", L'D', 1, /*enabled=*/false},
    };

    auto merged = MergePluginMenuItems(builtins, overlay);

    CHECK(merged[0].items.empty());
}

TEST_CASE(MergePluginMenuItems_DoesNotMutateTheBuiltinsPassedIn) {
    auto builtins = MakeBuiltins();
    std::vector<PluginMenuEntry> overlay = {
        PluginMenuEntry{L"p", L'F', L"Item", L'I', 1, true},
    };

    MergePluginMenuItems(builtins, overlay);

    CHECK(builtins[0].items.empty());  // the original vector, untouched
}

TEST_CASE(MergePluginMenuItems_MultipleEntriesForTheSameMenuAllAppend) {
    auto builtins = MakeBuiltins();
    std::vector<PluginMenuEntry> overlay = {
        PluginMenuEntry{L"p", L'F', L"First", L'1', 1, true},
        PluginMenuEntry{L"p", L'F', L"Second", L'2', 2, true},
    };

    auto merged = MergePluginMenuItems(builtins, overlay);

    CHECK(merged[0].items.size() == 2);
}

// --- ConflictsWithBuiltinShortcut / BuiltinShortcutInventory (FEA-0002 §12) --

TEST_CASE(ConflictsWithBuiltinShortcut_MatchingKeyConflicts) {
    std::vector<PluginShortcutKey> builtins = {{VK_F5, false, false, false}};
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F5, false, false, false}, builtins));
}

TEST_CASE(ConflictsWithBuiltinShortcut_DifferentModifiersDoNotConflict) {
    std::vector<PluginShortcutKey> builtins = {{VK_F5, false, false, false}};
    CHECK(!ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F5, true, false, false}, builtins));
}

TEST_CASE(ConflictsWithBuiltinShortcut_EmptyInventoryNeverConflicts) {
    CHECK(!ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F5, false, false, false}, {}));
}

TEST_CASE(BuiltinShortcutInventory_IncludesKnownAppShortcuts) {
    const auto& inventory = BuiltinShortcutInventory();
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F5, false, false, false}, inventory));   // Copy
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F11, true, false, false}, inventory));   // Ctrl+F11
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{static_cast<WORD>(L'H'), true, false, false}, inventory));  // Ctrl+H
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{static_cast<WORD>(L'R'), true, false, false}, inventory));  // Ctrl+R (IS-0012)
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{static_cast<WORD>(L'T'), true, false, false}, inventory));  // Ctrl+T (UI-013)
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{static_cast<WORD>(L'D'), true, false, false}, inventory));  // Ctrl+D (NAV-007)
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{static_cast<WORD>(L'M'), true, false, false}, inventory));  // Ctrl+M (FOP-017)
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F5, false, false, true}, inventory));   // Alt+F5 (FOP-014)
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{static_cast<WORD>(L'Q'), false, false, true}, inventory));  // Alt+Q (FOP-014)
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F8, false, false, true}, inventory));  // Alt+F8 (queued delete)
    CHECK(ConflictsWithBuiltinShortcut(PluginShortcutKey{VK_F8, false, true, true}, inventory));   // Alt+Shift+F8
}

TEST_CASE(BuiltinShortcutInventory_DoesNotClaimAnObviouslyFreeCombination) {
    const auto& inventory = BuiltinShortcutInventory();
    CHECK(!ConflictsWithBuiltinShortcut(PluginShortcutKey{static_cast<WORD>(L'Q'), true, true, true}, inventory));
}
