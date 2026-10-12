#pragma once

// Header-only Unicode case mapping (LCMapStringEx, invariant locale) --
// unlike std::towlower in the "C" locale, it maps accented letters too
// (Á <-> á, Ő <-> ő). Shared by FOP-017's group rename (case conversion) and
// SRC-004's content search (case-insensitive matching).

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <string>

namespace mc {

namespace detail {
inline std::wstring MapCaseInvariant(const std::wstring& text, DWORD flag) {
    if (text.empty()) return text;
    int len = LCMapStringEx(LOCALE_NAME_INVARIANT, flag, text.c_str(), static_cast<int>(text.size()), nullptr, 0,
                            nullptr, nullptr, 0);
    if (len <= 0) return text;
    std::wstring out(static_cast<size_t>(len), L'\0');
    LCMapStringEx(LOCALE_NAME_INVARIANT, flag, text.c_str(), static_cast<int>(text.size()), out.data(), len, nullptr,
                  nullptr, 0);
    return out;
}
}  // namespace detail

inline std::wstring ToLowerInvariant(const std::wstring& text) { return detail::MapCaseInvariant(text, LCMAP_LOWERCASE); }
inline std::wstring ToUpperInvariant(const std::wstring& text) { return detail::MapCaseInvariant(text, LCMAP_UPPERCASE); }

}  // namespace mc
