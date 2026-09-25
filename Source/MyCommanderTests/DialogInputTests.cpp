#include "TestFramework.h"

#include "Dialog.h"

#include <windows.h>

using mc::ApplyTextPromptKey;
using mc::BuildOptionRegions;
using mc::DialogOption;
using mc::MatchDialogHotkey;
using mc::TextPromptKeyResult;
using mc::TextPromptOutcome;

// --- MatchDialogHotkey (TST-005) --------------------------------------------

TEST_CASE(MatchDialogHotkey_MatchesExactCase) {
    std::vector<DialogOption> options = {{L'O', L"verwrite"}, {L'S', L"kip"}, {L'C', L"ancel"}};
    CHECK(MatchDialogHotkey(L'S', options) == 1);
}

TEST_CASE(MatchDialogHotkey_IsCaseInsensitive) {
    std::vector<DialogOption> options = {{L'O', L"verwrite"}, {L'S', L"kip"}, {L'C', L"ancel"}};
    CHECK(MatchDialogHotkey(L's', options) == 1);
    CHECK(MatchDialogHotkey(L'o', options) == 0);
}

TEST_CASE(MatchDialogHotkey_NoMatchReturnsMinusOne) {
    std::vector<DialogOption> options = {{L'Y', L"es"}, {L'N', L"o"}};
    CHECK(MatchDialogHotkey(L'X', options) == -1);
}

TEST_CASE(MatchDialogHotkey_NullCharacterReturnsMinusOne) {
    // A KEY_EVENT with no associated character (an arrow key, a bare
    // modifier, a function key) reports UnicodeChar == 0 — must never match
    // any hotkey.
    std::vector<DialogOption> options = {{L'Y', L"es"}, {L'N', L"o"}};
    CHECK(MatchDialogHotkey(0, options) == -1);
}

TEST_CASE(MatchDialogHotkey_EmptyOptionsReturnsMinusOne) {
    CHECK(MatchDialogHotkey(L'Y', {}) == -1);
}

// --- ApplyTextPromptKey (TST-005) -------------------------------------------

TEST_CASE(ApplyTextPromptKey_Escape_Cancels) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"some text", VK_ESCAPE, 0);
    CHECK(result.outcome == TextPromptOutcome::Cancelled);
}

TEST_CASE(ApplyTextPromptKey_Enter_SubmitsTextUnchanged) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"some text", VK_RETURN, 0);
    CHECK(result.outcome == TextPromptOutcome::Submitted);
    CHECK(result.text == L"some text");
}

TEST_CASE(ApplyTextPromptKey_Backspace_RemovesLastCharacter) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"abc", VK_BACK, 0);
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"ab");
}

TEST_CASE(ApplyTextPromptKey_Backspace_OnEmptyTextIsNoOp) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"", VK_BACK, 0);
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text.empty());
}

TEST_CASE(ApplyTextPromptKey_PrintableCharacter_IsAppended) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"ab", 0, L'c');
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"abc");
}

TEST_CASE(ApplyTextPromptKey_ControlCharacterBelowSpace_IsNotAppended) {
    // 0x09 (Tab) is below the 0x20 printable threshold ShowTextPrompt guards
    // on — must leave the text unchanged rather than inserting a raw tab.
    TextPromptKeyResult result = ApplyTextPromptKey(L"ab", 0, static_cast<wchar_t>(0x09));
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"ab");
}

TEST_CASE(ApplyTextPromptKey_ArrowKeyWithNoCharacter_LeavesTextUnchanged) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"abc", VK_LEFT, 0);
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"abc");
}

TEST_CASE(ApplyTextPromptKey_AtCharacterCap_RefusesFurtherAppends) {
    std::wstring fullText(240, L'x');
    TextPromptKeyResult result = ApplyTextPromptKey(fullText, 0, L'y');
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == fullText);
    CHECK(result.text.size() == 240);
}

TEST_CASE(ApplyTextPromptKey_OneBelowCap_StillAccepts) {
    std::wstring almostFull(239, L'x');
    TextPromptKeyResult result = ApplyTextPromptKey(almostFull, 0, L'y');
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text.size() == 240);
    CHECK(result.text.back() == L'y');
}

// --- BuildOptionRegions (IS-0003) --------------------------------------------

TEST_CASE(BuildOptionRegions_SingleOption_StartsAtStartCol) {
    std::vector<DialogOption> options = {{L'Y', L"es"}};
    auto regions = BuildOptionRegions(options, /*row=*/10, /*startCol=*/5);
    CHECK(regions.size() == 1);
    if (regions.size() == 1) {
        CHECK(regions[0].row == 10);
        CHECK(regions[0].startCol == 5);
        CHECK(regions[0].endCol == 5 + 5);  // "[Y]es" is 5 columns wide
    }
}

TEST_CASE(BuildOptionRegions_TwoOptions_SeparatedByTwoColumns) {
    std::vector<DialogOption> options = {{L'Y', L"es"}, {L'N', L"o"}};
    auto regions = BuildOptionRegions(options, /*row=*/10, /*startCol=*/0);
    CHECK(regions.size() == 2);
    if (regions.size() == 2) {
        // "[Y]es" is 5 columns wide, then a 2-column "  " separator.
        CHECK(regions[0].startCol == 0);
        CHECK(regions[0].endCol == 5);
        CHECK(regions[1].startCol == 7);
        CHECK(regions[1].endCol == 7 + 4);  // "[N]o" is 4 columns wide
    }
}

TEST_CASE(BuildOptionRegions_ThreeOptions_RegionsDoNotOverlap) {
    std::vector<DialogOption> options = {{L'O', L"verwrite"}, {L'S', L"kip"}, {L'C', L"ancel"}};
    auto regions = BuildOptionRegions(options, /*row=*/3, /*startCol=*/2);
    CHECK(regions.size() == 3);
    for (size_t i = 1; i < regions.size(); ++i) {
        CHECK(regions[i].startCol >= regions[i - 1].endCol);
    }
}

TEST_CASE(BuildOptionRegions_EmptyOptions_ReturnsNoRegions) {
    CHECK(BuildOptionRegions({}, 3, 2).empty());
}
