#pragma once

#include "ContentSearch.h"
#include "FileProvider.h"
#include "TextFile.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mc {

struct SearchResult {
    std::filesystem::path fullPath;
    bool isDirectory = false;
    // SRC-010: a file's logical size, from the directory listing the walk already has.
    uint64_t sizeBytes = 0;
    // SRC-004: for a content search, the first matching line (1-based) and its text.
    size_t lineNumber = 0;
    std::wstring lineText;
};

struct SearchOutcome {
    std::vector<SearchResult> results;
    bool cancelled = false;
    bool truncated = false;  // hit the result cap before the walk finished
    size_t filesSearched = 0;    // SRC-004: files whose content was read (SRC-010: files examined)
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
    // SRC-010: when > 0, the walk keeps only the `largestCount` largest matching
    // files (memory bounded by that count, not by the tree), returned largest
    // first with no `truncated` flag. Files smaller than `minSize` are ignored.
    size_t largestCount = 0;
    uint64_t minSize = 0;
    // SRC-010: false = only the top folder's own entries are examined.
    bool recursive = true;
    // SRC-010: false = hidden and system files (and folders) are skipped. A
    // hidden or system attribute is read only for a file that could enter the
    // top-N list, so the common case costs no extra call.
    bool includeHidden = true;
};

// SRC-010: keeps the best `count` results in `heap` (a heap ordered so its front
// is the weakest kept entry). "Better" = larger size, then the lexically smaller
// path, so ties are deterministic. Pure, so the bound and the tie rule are
// tested directly.
bool IsBetterLargest(const SearchResult& a, const SearchResult& b);
void OfferTopN(std::vector<SearchResult>& heap, SearchResult candidate, size_t count);
// Puts a finished top-N heap into best-first order.
void SortLargestFirst(std::vector<SearchResult>& results);

// SRC-010: parses the Largest files form's number fields. A count is 1..10000;
// a minimum size is a whole number of bytes, 0 when blank. False when out of
// range or not a number, leaving the output unchanged.
bool ParseLargestCount(const std::wstring& text, size_t& count);
bool ParseMinimumSize(const std::wstring& text, uint64_t& bytes);

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

// EXT-002: SRC-001's name search inside a file-system provider (an archive,
// a server): `root` is a provider path and each result's `fullPath` holds a
// provider path too. Names only -- `options.content` is ignored, since a
// content search would fetch every file. Folders are listed one at a time
// (cancel polled per folder); a link is reported but never descended
// (SEC-006); a folder that can't be listed is skipped.
SearchOutcome SearchProvider(IFileProvider& provider, const std::wstring& root, const SearchOptions& options,
                             const SearchCancelPoll& cancelPoll = {}, const SearchProgress& onProgress = {});

// SRC-001's original name-only search: SearchFiles with just a name mask.
SearchOutcome SearchFiles(const std::filesystem::path& root, const std::wstring& pattern,
                          const SearchCancelPoll& cancelPoll = {}, size_t maxResults = 5000);

}  // namespace mc
