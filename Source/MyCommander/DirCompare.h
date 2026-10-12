#pragma once

#include "Search.h"  // SearchCancelPoll

#include <cstdint>
#include <filesystem>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// SRC-006: compare two directories by name, size, timestamp, or content.
// Pure (no Console/Dialog dependency) so it's unit-testable on its own;
// main.cpp's DoCompareDirectories owns the form, progress, panel marking and
// results list.

// Cumulative: each method also applies the ones before it.
enum class CompareMethod { Name, Size, Timestamp, Content };

struct CompareOptions {
    CompareMethod method = CompareMethod::Timestamp;
    bool recursive = false;
    uint32_t toleranceSeconds = 2;  // FAT/exFAT/network shares store 2-second times
    bool ignoreOneHour = false;     // DST / FAT local-time shifts
    // What each panel shows (NAV-010 / SRC-003): hidden+system entries only
    // when that side includes them; the mask filters files (never folders,
    // so a filtered compare still recurses). Empty mask = every file.
    bool includeHiddenLeft = false;
    bool includeHiddenRight = false;
    std::wstring fileMaskLeft;
    std::wstring fileMaskRight;
    size_t maxItems = 200000;
};

enum class CompareStatus {
    Identical,
    LeftOnly,
    RightOnly,
    LeftNewer,
    RightNewer,
    Different,     // size/content differs without a newer side (or a folder with differences inside)
    TypeMismatch,  // a file on one side, a folder on the other
    Unreadable,    // the content check (or a subfolder listing) failed on a side
};

struct CompareSide {
    uint64_t size = 0;
    uint64_t lastWriteUtc = 0;  // FILETIME ticks
    uint32_t attributes = 0;
    bool isDirectory = false;
    bool isReparsePoint = false;
};

struct CompareItem {
    std::wstring relativePath;  // L"sub\\file.txt"; a top-level item has no separator
    std::wstring name;          // the last component, as spelled on the left (else the right)
    int depth = 0;              // 0 = top level (the items the panels show)
    std::optional<CompareSide> left;
    std::optional<CompareSide> right;
    CompareStatus status = CompareStatus::Identical;
};

struct CompareOutcome {
    std::vector<CompareItem> items;  // depth-first; per level folders first, then by name
    size_t filesCompared = 0;        // file pairs present on both sides
    uint64_t bytesRead = 0;          // content reads, both sides together
    size_t unreadable = 0;
    bool cancelled = false;
    bool truncated = false;          // hit maxItems
    bool leftListed = true;          // false: the left root itself couldn't be listed
    bool rightListed = true;
};

struct CompareProgress {
    size_t itemsCompared = 0;
    uint64_t bytesRead = 0;
    std::wstring current;  // relative path being compared
};
using CompareProgressCallback = std::function<void(const CompareProgress&)>;

// Pure decision for one pair, no I/O. `contentEqual` is consulted only for
// CompareMethod::Content when both are files of equal size; nullopt there
// means a side couldn't be read (-> Unreadable). For Content, timestamps only
// decide the *direction* of a real difference, never create one.
CompareStatus ClassifyPair(const std::optional<CompareSide>& left, const std::optional<CompareSide>& right,
                           const CompareOptions& options, std::optional<bool> contentEqual = std::nullopt);

// Reads both files side by side in 64 KB chunks, stopping at the first
// difference. nullopt when either can't be opened/read or the cancel poll
// fired (`*cancelled` set). `bytesRead` accumulates both sides' bytes.
std::optional<bool> FilesHaveEqualContent(const std::filesystem::path& a, const std::filesystem::path& b,
                                          const SearchCancelPoll& cancelPoll = {}, uint64_t* bytesRead = nullptr,
                                          bool* cancelled = nullptr);

// Compares `left` and `right` (names matched case-insensitively, `.`/`..`
// never items). Reparse points are compared as entries and never descended
// or content-read (SEC-006). In recursive mode a folder present on both sides
// is Identical only when everything inside is.
CompareOutcome CompareDirectories(const std::filesystem::path& left, const std::filesystem::path& right,
                                  const CompareOptions& options, const SearchCancelPoll& cancelPoll = {},
                                  const CompareProgressCallback& onProgress = {});

// Which side(s) D4's panel marking selects for a status.
inline bool MarksLeft(CompareStatus s) {
    return s != CompareStatus::Identical && s != CompareStatus::RightOnly && s != CompareStatus::RightNewer;
}
inline bool MarksRight(CompareStatus s) {
    return s != CompareStatus::Identical && s != CompareStatus::LeftOnly && s != CompareStatus::LeftNewer;
}

}  // namespace mc
