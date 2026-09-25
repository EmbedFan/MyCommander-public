#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mc {

struct SearchResult {
    std::filesystem::path fullPath;
    bool isDirectory = false;
};

struct SearchOutcome {
    std::vector<SearchResult> results;
    bool cancelled = false;
    bool truncated = false;  // hit the result cap before the walk finished
};

// Polled once per directory visited, so a long recursive search can be
// interrupted early without threads (mirrors FileOps.h's CancelPoll).
using SearchCancelPoll = std::function<bool()>;

// Recursively walks `root`, collecting every entry (file or directory)
// whose name matches the case-insensitive wildcard `pattern` via
// PathUtil.h's WildcardMatch (SRC-001). Never descends into a reparse
// point/junction (SEC-006) — a matching reparse point itself is still
// reported, just not expanded; a subdirectory that can't be listed
// (permission denied) is skipped, not treated as an error. Stops once
// `maxResults` matches are collected, setting `truncated`, so an unbounded
// tree can't exhaust memory (mirrors TextFile.h's read cap). Deliberately
// independent of Console so it's unit-testable on its own.
SearchOutcome SearchFiles(const std::filesystem::path& root, const std::wstring& pattern,
                          const SearchCancelPoll& cancelPoll = {}, size_t maxResults = 5000);

}  // namespace mc
