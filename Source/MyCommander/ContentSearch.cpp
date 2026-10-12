// SRC-005: MSVC's std::regex stops (throws error_complexity) after this many
// backtracking steps per search attempt. Its default (10 million) let a
// pathological pattern such as (a+)+b stall the UI for well over 30 seconds
// on one 29-character line, and even 1 million still took ~8 s in a Debug
// build -- Esc can't interrupt a single regex_search call. This must come
// before <regex> is included, so <regex> is included by this file only (the
// header keeps the regex opaque); no other translation unit uses <regex>.
#define _REGEX_MAX_COMPLEXITY_COUNT 200000L

#include "ContentSearch.h"

#include "TextCase.h"
#include "Utf8TextFile.h"

#include <locale>
#include <regex>

namespace mc {

struct ContentMatcher::Regex {
    std::wregex re;
};

std::optional<ContentMatcher> ContentMatcher::Create(const ContentQuery& query, std::wstring& error) {
    if (query.text.empty()) {
        error = L"no text to search for";
        return std::nullopt;
    }
    ContentMatcher matcher;
    matcher.query_ = query;
    if (!query.regex) {
        matcher.needle_ = query.matchCase ? query.text : ToLowerInvariant(query.text);
        return matcher;
    }
    try {
        auto holder = std::make_shared<Regex>();
        try {
            holder->re.imbue(std::locale(""));  // the user's locale: icase folds accented letters too
        } catch (const std::exception&) {
            // no usable user locale: keep the classic one
        }
        auto flags = std::regex_constants::ECMAScript;
        if (!query.matchCase) flags |= std::regex_constants::icase;
        holder->re.assign(query.text, flags);
        matcher.regex_ = std::move(holder);
    } catch (const std::regex_error& e) {
        error = L"invalid regular expression: " + Utf8ToWide(e.what());
        return std::nullopt;
    }
    return matcher;
}

bool ContentMatcher::Matches(const std::wstring& line) const {
    const size_t limit = regex_ ? kMaxRegexLineLength : kMaxMatchedLineLength;
    const std::wstring limited = line.size() > limit ? line.substr(0, limit) : line;
    if (!regex_) {
        if (query_.matchCase) return limited.find(needle_) != std::wstring::npos;
        return ToLowerInvariant(limited).find(needle_) != std::wstring::npos;
    }
    try {
        return std::regex_search(limited, regex_->re);
    } catch (const std::regex_error&) {
        return false;  // gave up (complexity/stack limit): no match on this line
    }
}

bool LooksBinary(const unsigned char* data, size_t length, bool allowUtf16) {
    if (allowUtf16) return false;
    for (size_t i = 0; i < length; ++i)
        if (data[i] == 0) return true;
    return false;
}

}  // namespace mc
