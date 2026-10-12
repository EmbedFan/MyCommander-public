#pragma once

#include "ContentSearch.h"
#include "TextFile.h"

#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mc {

struct SearchResult {
    std::filesystem::path fullPath;
    bool isDirectory = false;
    // SRC-004: for a content search, the first matching line (1-based) and its text.
    size_t lineNumber = 0;
    std::wstring lineText;
};

struct SearchOutcome {
    std::vector<SearchResult> results;
    bool cancelled = false;
    bool truncated = false;  // hit the result cap before the walk finished
    size_t filesSearched = 0;    // SRC-004: files whose content was read
    size_t unreadable = 0;       // SRC-004: files that couldn't be opened (locked, permission denied)
    size_t binarySkipped = 0;    // SRC-004: files skipped as binary
};

// Polled once per directory visited -- and, during a content search, while
// reading files -- so a long search can be interrupted early without threads
// (mirrors FileOps.h's CancelPoll).
using SearchCancelPoll = std::function<bool()>;

// SRC-004: called before each file's content is read, with how many files
// have been searched so far -- for a live progress line.
using SearchProgress = std::function<void(size_t filesSearched, const std::filesystem::path& current)>;

struct SearchOptions {
    std::wstring nameMask;                   // WildcardMatch; required
    std::optional<ContentMatcher> content;   // SRC-004/005: set = only files containing a match
    TextEncodingChoice encoding = TextEncodingChoice::Auto;  // SRC-005
    bool skipBinary = true;                  // SRC-004: NUL in the first 8 KB (and no UTF-16) = binary
    size_t maxResults = 5000;
};

// Recursively walks `root`, collecting every entry whose name matches
// `options.nameMask` via PathUtil.h's WildcardMatch (SRC-001) -- and, when
// `options.content` is set, only *files* with at least one matching line,
// reporting the first one (SRC-004/005; folders are walked but never
// listed). Never descends into a reparse point/junction (SEC-006) — a
// matching reparse point itself is still reported, just not expanded; a
// subdirectory that can't be listed is skipped, and a file that can't be
// read is counted in `unreadable`, neither treated as an error. Stops once
// `maxResults` matches are collected, setting `truncated`. Deliberately
// independent of Console so it's unit-testable on its own.
SearchOutcome SearchFiles(const std::filesystem::path& root, const SearchOptions& options,
                          const SearchCancelPoll& cancelPoll = {}, const SearchProgress& onProgress = {});

// SRC-001's original name-only search: SearchFiles with just a name mask.
SearchOutcome SearchFiles(const std::filesystem::path& root, const std::wstring& pattern,
                          const SearchCancelPoll& cancelPoll = {}, size_t maxResults = 5000);

}  // namespace mc
