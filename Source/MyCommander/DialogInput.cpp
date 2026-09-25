// TST-005: MatchDialogHotkey/ApplyTextPromptKey are pure, Console-
// independent dialog-input logic, declared in Dialog.h alongside the real
// Console-driving ShowChoiceDialog/ShowTextPrompt but defined here in their
// own translation unit (matching Navigation.cpp's precedent) so the test
// project can compile and unit-test them directly without pulling in
// Dialog.cpp's/Console.cpp's real Win32 console dependency.

#include "Dialog.h"
#include "TextWidth.h"

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

TextPromptKeyResult ApplyTextPromptKey(const std::wstring& currentText, WORD virtualKeyCode, wchar_t unicodeChar) {
    TextPromptKeyResult result;
    result.text = currentText;
    if (virtualKeyCode == VK_ESCAPE) {
        result.outcome = TextPromptOutcome::Cancelled;
        return result;
    }
    if (virtualKeyCode == VK_RETURN) {
        result.outcome = TextPromptOutcome::Submitted;
        return result;
    }
    if (virtualKeyCode == VK_BACK) {
        if (!result.text.empty()) result.text.pop_back();
        return result;
    }
    if (unicodeChar >= 0x20 && result.text.size() < 240) {
        result.text.push_back(unicodeChar);
    }
    return result;
}

} // namespace mc
