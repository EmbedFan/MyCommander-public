#include "HistorySearch.h"

#include "TextCase.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mc {

std::optional<size_t> FindHistoryMatch(const std::vector<std::wstring>& history, const std::wstring& text,
                                       size_t startIndex) {
    if (history.empty()) return std::nullopt;
    if (startIndex >= history.size()) startIndex = history.size() - 1;
    const std::wstring needle = ToLowerInvariant(text);
    for (size_t i = startIndex + 1; i-- > 0;) {
        if (needle.empty() || ToLowerInvariant(history[i]).find(needle) != std::wstring::npos) return i;
    }
    return std::nullopt;
}

SearchKeyAction ClassifySearchKey(unsigned short virtualKey, bool ctrl, bool shift, bool alt, wchar_t unicodeChar) {
    if (virtualKey == 'R' && alt && !ctrl && !shift) return SearchKeyAction::StepOlder;
    if (!ctrl && !alt) {
        if (virtualKey == VK_RETURN) return SearchKeyAction::Accept;
        if (virtualKey == VK_ESCAPE) return SearchKeyAction::Cancel;
        if (virtualKey == VK_BACK) return SearchKeyAction::Backspace;
        if (unicodeChar >= 32 && unicodeChar != 127) return SearchKeyAction::Insert;
    }
    return SearchKeyAction::Other;
}

}  // namespace mc
