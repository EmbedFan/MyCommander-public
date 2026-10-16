// LIST-009: colour labels on files and folders. Pure logic (no Console, no
// filesystem access): the label store keyed by full path and the small text file
// it is saved in. `main.cpp` draws the labels in the panel, owns the commands and
// keeps labels attached when the application itself renames, moves or deletes.
//
// A label lives *outside* the labelled files: nothing is written into, beside or
// onto them (no alternate data stream, no sidecar file in their folder). It is
// stored in %APPDATA%\MyCommander\labels.conf, one `label = path` line each:
//
//   # comment
//   1 = C:\work\report.docx
//   4 = D:\media\holiday
//
// The first '=' splits a line, so a path may contain '='. Lookups are lexical:
// case-insensitive, '/' and '\' alike, a trailing separator ignored, and never
// touch the disk (an offline share can't stall drawing or loading).
#pragma once

#include <cstddef>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace mc {

// Label numbers are 1..kLabelCount; 0 means "no label".
constexpr int kLabelCount = 6;
// The store refuses to grow past this many labelled paths.
constexpr size_t kMaxLabels = 5000;

struct LabelEntry {
    std::filesystem::path path;
    int label = 0;
};

class LabelStore {
public:
    explicit LabelStore(size_t cap = kMaxLabels) : cap_(cap) {}

    // 0 when `path` has no label.
    int Get(const std::filesystem::path& path) const;

    // Sets (1..kLabelCount) or clears (0) the label of `path`. False (nothing
    // changed) for an out-of-range label, or when a *new* path would exceed the
    // cap; changing or clearing an existing one always succeeds.
    bool Set(const std::filesystem::path& path, int label);

    // True if `path` itself or anything beneath it carries a label. Cheap and
    // lexical: lets callers skip disk probes for the common unlabelled case.
    bool HasAnyUnder(const std::filesystem::path& path) const;

    // A file or folder was renamed/moved: the label of `from`, and of everything
    // beneath it, now belongs to the same place under `to`. Returns how many
    // labels moved (an existing label at the destination is replaced).
    size_t Move(const std::filesystem::path& from, const std::filesystem::path& to);

    // `path` and everything beneath it is gone. Returns how many labels dropped.
    size_t RemoveUnder(const std::filesystem::path& path);

    // Drops every label for which `isMissing(path)` is true; returns how many.
    // The predicate is the caller's (it may probe the disk); the store itself
    // never does.
    template <typename Pred>
    size_t RemoveIf(Pred isMissing) {
        size_t before = entries_.size();
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (isMissing(it->second.path)) it = entries_.erase(it);
            else ++it;
        }
        return before - entries_.size();
    }

    size_t Size() const { return entries_.size(); }
    bool Empty() const { return entries_.empty(); }
    // Every labelled path, in a stable order (by path key), for saving and listing.
    std::vector<LabelEntry> Entries() const;
    void Clear() { entries_.clear(); }
    // Replaces the content with `entries` (invalid labels dropped, duplicates:
    // the last one wins, capped).
    void Replace(const std::vector<LabelEntry>& entries);

private:
    size_t cap_;
    // key (LabelPathKey) -> entry. Ordered, so "everything beneath a folder" is
    // one contiguous range, and a lookup per visible row stays O(log n).
    std::map<std::wstring, LabelEntry> entries_;
};

// The lexical comparison key (exposed for tests).
std::wstring LabelPathKey(const std::filesystem::path& path);

struct ParseLabelsResult {
    std::vector<LabelEntry> entries;
    std::vector<std::wstring> warnings;  // one line per skipped line
};
ParseLabelsResult ParseLabelsFile(const std::wstring& text);
std::wstring FormatLabelsFile(const std::vector<LabelEntry>& entries);

std::filesystem::path LabelsFilePath();
// A missing file is the expected first run: no labels, no warnings.
ParseLabelsResult LoadLabelsFrom(const std::filesystem::path& path);
bool SaveLabelsTo(const std::filesystem::path& path, const std::vector<LabelEntry>& entries);

}  // namespace mc
