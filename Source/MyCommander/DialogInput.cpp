// TST-005: MatchDialogHotkey/ApplyTextPromptKey are pure, Console-
// independent dialog-input logic, declared in Dialog.h alongside the real
// Console-driving ShowChoiceDialog/ShowTextPrompt but defined here in their
// own translation unit (matching Navigation.cpp's precedent) so the test
// project can compile and unit-test them directly without pulling in
// Dialog.cpp's/Console.cpp's real Win32 console dependency.

#include "Dialog.h"
#include "TextWidth.h"

#include <algorithm>
#include <cwctype>

namespace mc {

std::vector<ClickableRegion> BuildOptionRegions(const std::vector<DialogOption>& options, SHORT row, SHORT startCol) {
    std::vector<ClickableRegion> regions;
    SHORT col = startCol;
    for (size_t i = 0; i < options.size(); ++i) {
        if (i) col = static_cast<SHORT>(col + 2);  // "  " separator, matches ShowChoiceDialog's optionsLine join
        std::wstring segment = L"[" + std::wstring(1, options[i].hotkey) + L"]" + options[i].label;
        SHORT segWidth = static_cast<SHORT>(StringDisplayWidth(segment));
        regions.push_back({row, col, static_cast<SHORT>(col + segWidth)});
        col = static_cast<SHORT>(col + segWidth);
    }
    return regions;
}

int MatchDialogHotkey(wchar_t ch, const std::vector<DialogOption>& options) {
    if (ch == 0) return -1;
    for (size_t i = 0; i < options.size(); ++i) {
        if (std::towlower(ch) == std::towlower(options[i].hotkey)) return static_cast<int>(i);
    }
    return -1;
}

bool ApplyLineEditKey(LineEdit& edit, WORD virtualKeyCode, wchar_t unicodeChar) {
    switch (virtualKeyCode) {
        case VK_LEFT:
            if (edit.cursor > 0) --edit.cursor;
            return true;
        case VK_RIGHT:
            if (edit.cursor < edit.text.size()) ++edit.cursor;
            return true;
        case VK_HOME:
            edit.cursor = 0;
            return true;
        case VK_END:
            edit.cursor = edit.text.size();
            return true;
        case VK_BACK:
            if (edit.cursor > 0) {
                edit.text.erase(edit.cursor - 1, 1);
                --edit.cursor;
            }
            return true;
        case VK_DELETE:
            if (edit.cursor < edit.text.size()) {
                edit.text.erase(edit.cursor, 1);
            }
            return true;
        default:
            break;
    }
    if (unicodeChar >= 0x20 && edit.text.size() < 240) {
        edit.text.insert(edit.cursor, 1, unicodeChar);
        ++edit.cursor;
        return true;
    }
    return false;
}

bool ApplyListChoiceKey(ListChoiceState& state, int itemCount, int visibleRows, WORD virtualKeyCode) {
    if (itemCount <= 0) return false;
    visibleRows = std::max(1, visibleRows);
    switch (virtualKeyCode) {
        case VK_UP: --state.cursor; break;
        case VK_DOWN: ++state.cursor; break;
        case VK_PRIOR: state.cursor -= visibleRows; break;
        case VK_NEXT: state.cursor += visibleRows; break;
        case VK_HOME: state.cursor = 0; break;
        case VK_END: state.cursor = itemCount - 1; break;
        default: return false;
    }
    state.cursor = std::clamp(state.cursor, 0, itemCount - 1);
    if (state.cursor < state.top) state.top = state.cursor;
    if (state.cursor >= state.top + visibleRows) state.top = state.cursor - visibleRows + 1;
    state.top = std::clamp(state.top, 0, std::max(0, itemCount - visibleRows));
    return true;
}

size_t ComputeLineEditViewOffset(const std::wstring& text, size_t cursor, size_t previousOffset,
                                 size_t viewWidth) {
    cursor = std::min(cursor, text.size());
    if (viewWidth == 0) return cursor;
    auto widthOf = [&](size_t from, size_t to) {
        return static_cast<size_t>(StringDisplayWidth(text.substr(from, to - from)));
    };
    // The cell the cursor itself occupies: the character under it, or one
    // blank column at end-of-text.
    size_t cursorCell = cursor < text.size() ? static_cast<size_t>(CharDisplayWidth(text[cursor])) : 1;

    // Everything (plus the end-of-text cursor cell) fits: never scroll.
    if (widthOf(0, text.size()) + 1 <= viewWidth) return 0;

    size_t offset = std::min(previousOffset, cursor);
    // Cursor ran off the right edge: scroll right just far enough.
    while (offset < cursor && widthOf(offset, cursor) + cursorCell > viewWidth) ++offset;
    // Text shrank (Backspace/Delete near the end): scroll back left while the
    // whole tail from the earlier offset still fits, so the box doesn't show
    // blank space on the right while text is hidden on the left.
    while (offset > 0 && widthOf(offset - 1, text.size()) + 1 <= viewWidth) --offset;
    return offset;
}

TextPromptKeyResult ApplyTextPromptKey(const std::wstring& currentText, size_t currentCursor, WORD virtualKeyCode,
                                       wchar_t unicodeChar) {
    TextPromptKeyResult result;
    result.text = currentText;
    result.cursor = currentCursor;
    if (virtualKeyCode == VK_ESCAPE) {
        result.outcome = TextPromptOutcome::Cancelled;
        return result;
    }
    if (virtualKeyCode == VK_RETURN) {
        result.outcome = TextPromptOutcome::Submitted;
        return result;
    }
    LineEdit edit{result.text, result.cursor};
    if (ApplyLineEditKey(edit, virtualKeyCode, unicodeChar)) {
        result.text = edit.text;
        result.cursor = edit.cursor;
    }
    return result;
}

std::wstring DisplayedFieldText(const FormField& field) {
    return field.masked ? std::wstring(field.text.size(), L'*') : field.text;
}

bool ApplyFormFieldKey(FormField& field, WORD virtualKeyCode, wchar_t unicodeChar) {
    auto clampNumber = [&]() { field.numberValue = std::clamp(field.numberValue, field.numberMin, field.numberMax); };

    switch (field.kind) {
        case FormFieldKind::Checkbox:
            if (virtualKeyCode == VK_LEFT || virtualKeyCode == VK_RIGHT || virtualKeyCode == VK_SPACE) {
                field.boolValue = !field.boolValue;
                return true;
            }
            return false;

        case FormFieldKind::Choice: {
            if (field.choices.empty()) return false;
            int count = static_cast<int>(field.choices.size());
            if (virtualKeyCode == VK_LEFT) {
                field.choiceIndex = (field.choiceIndex - 1 + count) % count;
                return true;
            }
            if (virtualKeyCode == VK_RIGHT) {
                field.choiceIndex = (field.choiceIndex + 1) % count;
                return true;
            }
            return false;
        }

        case FormFieldKind::Number:
            if (virtualKeyCode == VK_LEFT) {
                field.numberValue -= 1;
                clampNumber();
                return true;
            }
            if (virtualKeyCode == VK_RIGHT) {
                field.numberValue += 1;
                clampNumber();
                return true;
            }
            if (virtualKeyCode == VK_BACK) {
                field.numberValue /= 10;
                clampNumber();
                return true;
            }
            if (unicodeChar >= L'0' && unicodeChar <= L'9') {
                field.numberValue = field.numberValue * 10 + (unicodeChar - L'0');
                clampNumber();
                return true;
            }
            return false;

        case FormFieldKind::Text: {
            LineEdit edit{field.text, field.textCursor};
            if (!ApplyLineEditKey(edit, virtualKeyCode, unicodeChar)) return false;
            field.text = edit.text;
            field.textCursor = edit.cursor;
            return true;
        }
    }
    return false;
}

} // namespace mc
