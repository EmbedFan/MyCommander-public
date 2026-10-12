#include "TestFramework.h"

#include "Dialog.h"
#include "TextWidth.h"

#include <windows.h>

using mc::ApplyFormFieldKey;
using mc::ApplyLineEditKey;
using mc::ApplyTextPromptKey;
using mc::BuildOptionRegions;
using mc::ComputeLineEditViewOffset;
using mc::DialogOption;
using mc::FormField;
using mc::FormFieldKind;
using mc::LineEdit;
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
    TextPromptKeyResult result = ApplyTextPromptKey(L"some text", 9, VK_ESCAPE, 0);
    CHECK(result.outcome == TextPromptOutcome::Cancelled);
}

TEST_CASE(ApplyTextPromptKey_Enter_SubmitsTextUnchanged) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"some text", 9, VK_RETURN, 0);
    CHECK(result.outcome == TextPromptOutcome::Submitted);
    CHECK(result.text == L"some text");
}

TEST_CASE(ApplyTextPromptKey_Backspace_RemovesCharacterBeforeCursor) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"abc", 3, VK_BACK, 0);
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"ab");
    CHECK(result.cursor == 2);
}

TEST_CASE(ApplyTextPromptKey_Backspace_OnEmptyTextIsNoOp) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"", 0, VK_BACK, 0);
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text.empty());
    CHECK(result.cursor == 0);
}

TEST_CASE(ApplyTextPromptKey_PrintableCharacter_IsAppendedAtEnd) {
    TextPromptKeyResult result = ApplyTextPromptKey(L"ab", 2, 0, L'c');
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"abc");
    CHECK(result.cursor == 3);
}

TEST_CASE(ApplyTextPromptKey_PrintableCharacter_IsInsertedAtCursor) {
    // IS-0015 Part 3: insertion happens at `cursor`, not always at the end.
    TextPromptKeyResult result = ApplyTextPromptKey(L"ac", 1, 0, L'b');
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"abc");
    CHECK(result.cursor == 2);
}

TEST_CASE(ApplyTextPromptKey_ControlCharacterBelowSpace_IsNotAppended) {
    // 0x09 (Tab) is below the 0x20 printable threshold ShowTextPrompt guards
    // on — must leave the text unchanged rather than inserting a raw tab.
    TextPromptKeyResult result = ApplyTextPromptKey(L"ab", 2, 0, static_cast<wchar_t>(0x09));
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == L"ab");
}

TEST_CASE(ApplyTextPromptKey_LeftRight_MoveCursorWithoutChangingText) {
    // IS-0015 Part 3: Left/Right are no longer true no-ops -- they move the
    // cursor -- but they never change the text itself.
    TextPromptKeyResult left = ApplyTextPromptKey(L"abc", 2, VK_LEFT, 0);
    CHECK(left.outcome == TextPromptOutcome::StillEditing);
    CHECK(left.text == L"abc");
    CHECK(left.cursor == 1);

    TextPromptKeyResult right = ApplyTextPromptKey(L"abc", 1, VK_RIGHT, 0);
    CHECK(right.text == L"abc");
    CHECK(right.cursor == 2);
}

TEST_CASE(ApplyTextPromptKey_AtCharacterCap_RefusesFurtherAppends) {
    std::wstring fullText(240, L'x');
    TextPromptKeyResult result = ApplyTextPromptKey(fullText, fullText.size(), 0, L'y');
    CHECK(result.outcome == TextPromptOutcome::StillEditing);
    CHECK(result.text == fullText);
    CHECK(result.text.size() == 240);
}

TEST_CASE(ApplyTextPromptKey_OneBelowCap_StillAccepts) {
    std::wstring almostFull(239, L'x');
    TextPromptKeyResult result = ApplyTextPromptKey(almostFull, almostFull.size(), 0, L'y');
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

// --- ApplyFormFieldKey (FEA-0007, §1) ----------------------------------

TEST_CASE(FormField_MaskedTextEditsLikeTextButShowsStars_FTP010) {
    FormField field;
    field.kind = FormFieldKind::Text;
    field.masked = true;
    CHECK(ApplyFormFieldKey(field, 'S', L's'));
    CHECK(ApplyFormFieldKey(field, 'E', L'e'));
    CHECK(ApplyFormFieldKey(field, 'C', L'c'));
    CHECK(field.text == L"sec");
    CHECK(mc::DisplayedFieldText(field) == L"***");
    CHECK(ApplyFormFieldKey(field, VK_BACK, 0));
    CHECK(field.text == L"se");
    CHECK(mc::DisplayedFieldText(field) == L"**");
    field.masked = false;
    CHECK(mc::DisplayedFieldText(field) == L"se");
}

TEST_CASE(ApplyFormFieldKey_Checkbox_LeftRightAndSpaceToggle) {
    FormField field;
    field.kind = FormFieldKind::Checkbox;
    field.boolValue = false;
    CHECK(ApplyFormFieldKey(field, VK_LEFT, 0));
    CHECK(field.boolValue);
    CHECK(ApplyFormFieldKey(field, VK_RIGHT, 0));
    CHECK(!field.boolValue);
    CHECK(ApplyFormFieldKey(field, VK_SPACE, L' '));
    CHECK(field.boolValue);
}

TEST_CASE(ApplyFormFieldKey_Checkbox_OtherKeysAreNoOp) {
    FormField field;
    field.kind = FormFieldKind::Checkbox;
    field.boolValue = false;
    CHECK(!ApplyFormFieldKey(field, VK_UP, 0));
    CHECK(!field.boolValue);
}

TEST_CASE(ApplyFormFieldKey_Choice_CyclesAndWraps) {
    FormField field;
    field.kind = FormFieldKind::Choice;
    field.choices = {L"Store", L"Fast", L"Normal", L"Best"};
    field.choiceIndex = 0;
    CHECK(ApplyFormFieldKey(field, VK_LEFT, 0));
    CHECK(field.choiceIndex == 3);  // wraps backward past the start
    CHECK(ApplyFormFieldKey(field, VK_RIGHT, 0));
    CHECK(field.choiceIndex == 0);
    field.choiceIndex = 3;
    CHECK(ApplyFormFieldKey(field, VK_RIGHT, 0));
    CHECK(field.choiceIndex == 0);  // wraps forward past the end
}

TEST_CASE(ApplyFormFieldKey_Choice_EmptyChoicesIsNoOp) {
    FormField field;
    field.kind = FormFieldKind::Choice;
    CHECK(!ApplyFormFieldKey(field, VK_RIGHT, 0));
}

TEST_CASE(ApplyFormFieldKey_Number_DigitsAppendAndClamp) {
    FormField field;
    field.kind = FormFieldKind::Number;
    field.numberValue = 0;
    field.numberMin = 0;
    field.numberMax = 100;
    CHECK(ApplyFormFieldKey(field, 0, L'1'));
    CHECK(field.numberValue == 1);
    CHECK(ApplyFormFieldKey(field, 0, L'2'));
    CHECK(field.numberValue == 12);
    CHECK(ApplyFormFieldKey(field, 0, L'5'));  // 125 clamps to numberMax
    CHECK(field.numberValue == 100);
}

TEST_CASE(ApplyFormFieldKey_Number_BackspaceRemovesLastDigit) {
    FormField field;
    field.kind = FormFieldKind::Number;
    field.numberValue = 123;
    field.numberMin = 0;
    field.numberMax = 9999;
    CHECK(ApplyFormFieldKey(field, VK_BACK, 0));
    CHECK(field.numberValue == 12);
}

TEST_CASE(ApplyFormFieldKey_Number_LeftRightNudgeClamped) {
    FormField field;
    field.kind = FormFieldKind::Number;
    field.numberValue = 5;
    field.numberMin = 0;
    field.numberMax = 5;
    CHECK(ApplyFormFieldKey(field, VK_RIGHT, 0));
    CHECK(field.numberValue == 5);  // already at max
    CHECK(ApplyFormFieldKey(field, VK_LEFT, 0));
    CHECK(field.numberValue == 4);
}

TEST_CASE(ApplyFormFieldKey_Number_NonDigitLetterIsNoOp) {
    FormField field;
    field.kind = FormFieldKind::Number;
    field.numberValue = 5;
    field.numberMin = 0;
    field.numberMax = 100;
    CHECK(!ApplyFormFieldKey(field, 0, L'a'));
    CHECK(field.numberValue == 5);
}

TEST_CASE(ApplyFormFieldKey_Text_AppendAndBackspace) {
    FormField field;
    field.kind = FormFieldKind::Text;
    CHECK(ApplyFormFieldKey(field, 0, L'a'));
    CHECK(field.text == L"a");
    CHECK(ApplyFormFieldKey(field, 0, L'b'));
    CHECK(field.text == L"ab");
    CHECK(ApplyFormFieldKey(field, VK_BACK, 0));
    CHECK(field.text == L"a");
}

TEST_CASE(ApplyFormFieldKey_Text_BackspaceOnEmptyIsRecognizedNoOp) {
    // IS-0015 Part 3: Text now delegates to ApplyLineEditKey, which returns
    // true for VK_BACK even when it's a no-op (cursor already at 0) -- it's
    // a recognized line-editing key, same contract as Home at position 0.
    // (Previously ApplyFormFieldKey's own ad hoc Backspace handling returned
    // false here; that assertion is now stale, not just uncompilable.)
    FormField field;
    field.kind = FormFieldKind::Text;
    CHECK(ApplyFormFieldKey(field, VK_BACK, 0));
    CHECK(field.text.empty());
}

TEST_CASE(ApplyFormFieldKey_Text_InsertAtCursorNotJustAtEnd) {
    FormField field;
    field.kind = FormFieldKind::Text;
    field.text = L"ac";
    field.textCursor = 1;
    CHECK(ApplyFormFieldKey(field, 0, L'b'));
    CHECK(field.text == L"abc");
    CHECK(field.textCursor == 2);
}

TEST_CASE(ApplyFormFieldKey_Text_LeftRightHomeEndMoveCursor) {
    FormField field;
    field.kind = FormFieldKind::Text;
    field.text = L"abc";
    field.textCursor = 3;
    CHECK(ApplyFormFieldKey(field, VK_LEFT, 0));
    CHECK(field.textCursor == 2);
    CHECK(ApplyFormFieldKey(field, VK_HOME, 0));
    CHECK(field.textCursor == 0);
    CHECK(ApplyFormFieldKey(field, VK_END, 0));
    CHECK(field.textCursor == 3);
    CHECK(ApplyFormFieldKey(field, VK_RIGHT, 0));  // already at end, stays clamped
    CHECK(field.textCursor == 3);
}

TEST_CASE(ApplyFormFieldKey_Text_DeleteForwardAtCursor) {
    FormField field;
    field.kind = FormFieldKind::Text;
    field.text = L"abc";
    field.textCursor = 1;
    CHECK(ApplyFormFieldKey(field, VK_DELETE, 0));
    CHECK(field.text == L"ac");
    CHECK(field.textCursor == 1);
}

// --- ApplyLineEditKey (IS-0015 Part 3) ---------------------------------------

TEST_CASE(ApplyLineEditKey_InsertAtStart) {
    LineEdit edit{L"bc", 0};
    CHECK(ApplyLineEditKey(edit, 0, L'a'));
    CHECK(edit.text == L"abc");
    CHECK(edit.cursor == 1);
}

TEST_CASE(ApplyLineEditKey_InsertAtMiddle) {
    LineEdit edit{L"ac", 1};
    CHECK(ApplyLineEditKey(edit, 0, L'b'));
    CHECK(edit.text == L"abc");
    CHECK(edit.cursor == 2);
}

TEST_CASE(ApplyLineEditKey_InsertAtEnd) {
    LineEdit edit{L"ab", 2};
    CHECK(ApplyLineEditKey(edit, 0, L'c'));
    CHECK(edit.text == L"abc");
    CHECK(edit.cursor == 3);
}

TEST_CASE(ApplyLineEditKey_BackspaceAtStartIsNoOp) {
    LineEdit edit{L"abc", 0};
    CHECK(ApplyLineEditKey(edit, VK_BACK, 0));
    CHECK(edit.text == L"abc");
    CHECK(edit.cursor == 0);
}

TEST_CASE(ApplyLineEditKey_BackspaceAtMiddle) {
    LineEdit edit{L"abc", 2};
    CHECK(ApplyLineEditKey(edit, VK_BACK, 0));
    CHECK(edit.text == L"ac");
    CHECK(edit.cursor == 1);
}

TEST_CASE(ApplyLineEditKey_BackspaceAtEnd) {
    LineEdit edit{L"abc", 3};
    CHECK(ApplyLineEditKey(edit, VK_BACK, 0));
    CHECK(edit.text == L"ab");
    CHECK(edit.cursor == 2);
}

TEST_CASE(ApplyLineEditKey_DeleteAtStart) {
    LineEdit edit{L"abc", 0};
    CHECK(ApplyLineEditKey(edit, VK_DELETE, 0));
    CHECK(edit.text == L"bc");
    CHECK(edit.cursor == 0);
}

TEST_CASE(ApplyLineEditKey_DeleteAtMiddle) {
    LineEdit edit{L"abc", 1};
    CHECK(ApplyLineEditKey(edit, VK_DELETE, 0));
    CHECK(edit.text == L"ac");
    CHECK(edit.cursor == 1);
}

TEST_CASE(ApplyLineEditKey_DeleteAtEndIsNoOp) {
    LineEdit edit{L"abc", 3};
    CHECK(ApplyLineEditKey(edit, VK_DELETE, 0));
    CHECK(edit.text == L"abc");
    CHECK(edit.cursor == 3);
}

TEST_CASE(ApplyLineEditKey_LeftClampsAtStart) {
    LineEdit edit{L"abc", 0};
    CHECK(ApplyLineEditKey(edit, VK_LEFT, 0));
    CHECK(edit.cursor == 0);
}

TEST_CASE(ApplyLineEditKey_RightClampsAtEnd) {
    LineEdit edit{L"abc", 3};
    CHECK(ApplyLineEditKey(edit, VK_RIGHT, 0));
    CHECK(edit.cursor == 3);
}

TEST_CASE(ApplyLineEditKey_HomeJumpsToStart) {
    LineEdit edit{L"abc", 2};
    CHECK(ApplyLineEditKey(edit, VK_HOME, 0));
    CHECK(edit.cursor == 0);
}

TEST_CASE(ApplyLineEditKey_EndJumpsToEnd) {
    LineEdit edit{L"abc", 0};
    CHECK(ApplyLineEditKey(edit, VK_END, 0));
    CHECK(edit.cursor == 3);
}

TEST_CASE(ApplyLineEditKey_AtCharacterCap_RefusesFurtherInserts) {
    LineEdit edit{std::wstring(240, L'x'), 240};
    CHECK(!ApplyLineEditKey(edit, 0, L'y'));
    CHECK(edit.text.size() == 240);
}

TEST_CASE(ApplyLineEditKey_UnrecognizedKeyReturnsFalse) {
    LineEdit edit{L"abc", 1};
    CHECK(!ApplyLineEditKey(edit, VK_RETURN, 0));
    CHECK(edit.text == L"abc");
    CHECK(edit.cursor == 1);
}

// --- ComputeLineEditViewOffset (IS-0016) ------------------------------------

TEST_CASE(LineEditViewOffset_TextThatFits_NeverScrolls_IS0016) {
    CHECK(ComputeLineEditViewOffset(L"abc", 3, 0, 10) == 0);
    // Even with a stale offset from earlier, longer text.
    CHECK(ComputeLineEditViewOffset(L"abc", 3, 7, 10) == 0);
}

TEST_CASE(LineEditViewOffset_CursorAtEndOfLongText_SitsOnLastColumn_IS0016) {
    // "6789" plus the end-of-text cursor cell fills the 5-column window.
    CHECK(ComputeLineEditViewOffset(L"0123456789", 10, 0, 5) == 6);
}

TEST_CASE(LineEditViewOffset_HomeAfterScrolling_ReturnsToZero_IS0016) {
    CHECK(ComputeLineEditViewOffset(L"0123456789", 0, 6, 5) == 0);
}

TEST_CASE(LineEditViewOffset_MovingWithinWindow_DoesNotScroll_IS0016) {
    CHECK(ComputeLineEditViewOffset(L"0123456789", 7, 6, 5) == 6);
    CHECK(ComputeLineEditViewOffset(L"0123456789", 6, 6, 5) == 6);
    // One step past the left edge scrolls by exactly one.
    CHECK(ComputeLineEditViewOffset(L"0123456789", 5, 6, 5) == 5);
}

TEST_CASE(LineEditViewOffset_AfterTextShrinks_SlidesBackToFillWindow_IS0016) {
    // Was "0123456789" scrolled to 6; three Backspaces left "0123456".
    // "3456" plus the cursor cell fills the window again, no blank tail.
    CHECK(ComputeLineEditViewOffset(L"0123456", 7, 6, 5) == 3);
}

TEST_CASE(LineEditViewOffset_FullWidthText_MeasuredInColumns_IS0016) {
    std::wstring wide(6, L'\x6F22');  // six 2-column CJK ideographs, 12 columns
    // End-of-text: two ideographs (4 columns) + cursor cell (1) fit in 6.
    CHECK(ComputeLineEditViewOffset(wide, 6, 0, 6) == 4);
    // Cursor *on* an ideograph needs its own 2 columns: from offset 1, the
    // two ideographs before it (4) + the cursor's own (2) fill exactly 6.
    CHECK(ComputeLineEditViewOffset(wide, 3, 0, 6) == 1);
}

TEST_CASE(LineEditViewOffset_CursorAlwaysVisible_WhileSweepingBothWays_IS0016) {
    std::wstring text = L"C:\\very\\long\\path\\\x6F22\x5B57\\with\\mixed\\widths\\file.txt";
    const size_t viewWidth = 9;
    size_t offset = 0;
    auto check = [&](size_t cursor) {
        offset = ComputeLineEditViewOffset(text, cursor, offset, viewWidth);
        CHECK(offset <= cursor);
        size_t cell = cursor < text.size() ? static_cast<size_t>(mc::CharDisplayWidth(text[cursor])) : 1;
        size_t used = static_cast<size_t>(mc::StringDisplayWidth(text.substr(offset, cursor - offset))) + cell;
        CHECK(used <= viewWidth);
    };
    for (size_t c = 0; c <= text.size(); ++c) check(c);
    for (size_t c = text.size() + 1; c-- > 0;) check(c);
}

// --- ApplyListChoiceKey (FEA-0009, ShowListChoice) -----------------------------

TEST_CASE(ListChoice_UpDownClampAtEnds_NoWrap_FEA0009) {
    mc::ListChoiceState s;
    CHECK(mc::ApplyListChoiceKey(s, 3, 10, VK_UP));
    CHECK(s.cursor == 0);
    mc::ApplyListChoiceKey(s, 3, 10, VK_DOWN);
    mc::ApplyListChoiceKey(s, 3, 10, VK_DOWN);
    mc::ApplyListChoiceKey(s, 3, 10, VK_DOWN);
    CHECK(s.cursor == 2);
}

TEST_CASE(ListChoice_ScrollKeepsCursorVisible_FEA0009) {
    mc::ListChoiceState s;
    for (int i = 0; i < 6; ++i) mc::ApplyListChoiceKey(s, 20, 5, VK_DOWN);
    CHECK(s.cursor == 6);
    CHECK(s.top == 2);  // cursor on the last visible row
    mc::ApplyListChoiceKey(s, 20, 5, VK_UP);
    CHECK(s.top == 2);  // moving within the window doesn't scroll
}

TEST_CASE(ListChoice_PageHomeEnd_FEA0009) {
    mc::ListChoiceState s;
    mc::ApplyListChoiceKey(s, 20, 5, VK_NEXT);
    CHECK(s.cursor == 5);
    mc::ApplyListChoiceKey(s, 20, 5, VK_END);
    CHECK(s.cursor == 19 && s.top == 15);
    mc::ApplyListChoiceKey(s, 20, 5, VK_PRIOR);
    CHECK(s.cursor == 14 && s.top == 14);
    mc::ApplyListChoiceKey(s, 20, 5, VK_HOME);
    CHECK(s.cursor == 0 && s.top == 0);
}

TEST_CASE(ListChoice_UnrecognizedKeyOrEmptyList_ReturnsFalse_FEA0009) {
    mc::ListChoiceState s;
    CHECK(!mc::ApplyListChoiceKey(s, 5, 5, VK_RETURN));
    CHECK(!mc::ApplyListChoiceKey(s, 0, 5, VK_DOWN));
    CHECK(s.cursor == 0 && s.top == 0);
}

TEST_CASE(LineEditViewOffset_ZeroWidthView_DoesNotHang_IS0016) {
    CHECK(ComputeLineEditViewOffset(L"abc", 2, 0, 0) == 2);
}
