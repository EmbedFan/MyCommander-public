#pragma once

// SRC-004 / SRC-005: what "containing text" means in Find files (Alt+F7) --
// pure matching logic, no Console and no filesystem access (CLAUDE.md's
// pure-module precedent). Search.cpp feeds it decoded lines
// (TextFile.h's ForEachTextLine); a match never spans a line break.

#include <cstddef>
#include <memory>
#include <optional>
#include <string>

namespace mc {

struct ContentQuery {
    std::wstring text;
    bool regex = false;      // SRC-005: ECMAScript std::wregex instead of plain text
    bool matchCase = false;
};

// Only the first this-many characters of a line are matched, so a huge
// one-line (minified, binary-ish) file can't stall the search or exhaust
// memory. Plain text is a linear scan, so it gets a generous limit; a regular
// expression can do quadratic work (".*foo" retried from every position), so
// it gets a much smaller one -- together with ContentSearch.cpp's lowered
// backtracking limit, that bounds how long a single line can take, which
// matters because Esc can't interrupt one std::regex_search call.
constexpr size_t kMaxMatchedLineLength = 65536;
constexpr size_t kMaxRegexLineLength = 1024;  // measured: std::regex is slow per step, esp. in Debug

class ContentMatcher {
public:
    // Nullopt with `error` set for an empty text or an invalid regular
    // expression (the library's message), so the caller can report it
    // before walking a single folder.
    static std::optional<ContentMatcher> Create(const ContentQuery& query, std::wstring& error);

    // Plain text: substring, case-insensitive via the Unicode invariant
    // lower-case mapping unless matchCase. Regex: std::regex_search, `icase`
    // with the user's locale so accented letters fold too; a regex that
    // gives up on a line (the library's complexity/stack limit) is treated
    // as no match rather than an error.
    bool Matches(const std::wstring& line) const;

    const ContentQuery& Query() const { return query_; }

private:
    ContentQuery query_;
    std::wstring needle_;  // plain text, already lower-cased unless matchCase
    // Opaque so <regex> is included by ContentSearch.cpp alone, where its
    // complexity limit is lowered (see there); shared so copying is cheap.
    struct Regex;
    std::shared_ptr<Regex> regex_;
};

// SRC-004 binary-file heuristic: a NUL byte anywhere in `data` (the first
// few KB of a file) means binary -- unless `allowUtf16` (the file has a
// UTF-16 BOM or the user chose a UTF-16 encoding), where NULs are normal.
bool LooksBinary(const unsigned char* data, size_t length, bool allowUtf16);

}  // namespace mc
