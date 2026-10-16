#pragma once

// FOP-022: the undo journal -- a bounded, session-only stack of the file
// operations the application itself just performed, each recorded with the
// exact state it left behind, so Ctrl+Z can reverse the most recent one only
// while every affected item is still exactly there. Never written to disk
// (ERR-006: nothing that could be replayed after a crash). Pure apart from
// reading the file system: no Console/Dialog dependency.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mc {

// A cheap, recursive picture of a path: what the undo check compares. A
// reparse point is counted as itself and never descended (SEC-006).
struct PathFingerprint {
    bool exists = false;
    bool isDirectory = false;
    uint64_t files = 0;        // files at or below the path
    uint64_t bytes = 0;        // their total size
    uint64_t newestWrite = 0;  // newest last-write time, FILETIME ticks
    uint32_t attributes = 0;   // Win32 attributes of the path itself
    bool operator==(const PathFingerprint& o) const {
        return exists == o.exists && isDirectory == o.isDirectory && files == o.files && bytes == o.bytes &&
               newestWrite == o.newestWrite && attributes == o.attributes;
    }
};

PathFingerprint FingerprintOf(const std::filesystem::path& path);

enum class UndoKind { Rename, Move, Copy, Attributes, NewFolder, NewFile, Delete };

struct UndoItem {
    std::filesystem::path before;  // Rename/Move: the original path; Copy: the source; others: the path itself
    std::filesystem::path after;   // Rename/Move: the new path; Copy/Create*: the created path; Attributes: the path
    PathFingerprint expected;      // the state `after` was left in
    uint32_t attributesBefore = 0; // Attributes only
                                   // Delete: before == after == the original path, expected = its pre-delete
                                   // fingerprint (files/bytes are what the bin entry is matched on)
};

struct UndoEntry {
    UndoKind kind = UndoKind::Rename;
    std::wstring description;  // "rename a.txt", "move 3 items" -- shown in the undo prompt
    uint64_t startedUtc = 0;   // Delete: FILETIME ticks just before the delete began (bin lookup lower bound)
    std::vector<UndoItem> items;
};

// Capped stack, newest on top; pushing past the cap drops the oldest.
class UndoJournal {
public:
    static constexpr size_t kCapacity = 20;
    void Push(UndoEntry entry);
    bool Empty() const { return entries_.empty(); }
    size_t Size() const { return entries_.size(); }
    const UndoEntry* Top() const { return entries_.empty() ? nullptr : &entries_.back(); }
    void Pop() {
        if (!entries_.empty()) entries_.pop_back();
    }
    void Clear() { entries_.clear(); }

private:
    std::vector<UndoEntry> entries_;
};

// Builders: capture the state the operation just left.
UndoItem MakeMovedItem(const std::filesystem::path& before, const std::filesystem::path& after);  // Rename, Move
UndoItem MakeCreatedItem(const std::filesystem::path& source, const std::filesystem::path& created);  // Copy
UndoItem MakeCreatedItem(const std::filesystem::path& created);                                      // CreateFolder/File
UndoItem MakeAttributeItem(const std::filesystem::path& path, uint32_t attributesBefore);
UndoItem MakeDeletedItem(const std::filesystem::path& original, const PathFingerprint& before);  // Delete

struct UndoCheck {
    bool ok = false;
    std::wstring problem;  // names the first item that no longer matches
};

// Every item must still be in exactly the recorded state; otherwise the whole
// entry is refused and nothing is touched. A Delete entry only checks that the
// original name is free and its folder exists (the bin lookup is `locate`).
UndoCheck CheckUndo(const UndoEntry& entry);

struct UndoActions {
    // Sends a path to the Recycle Bin (copy undo). Must not delete permanently.
    std::function<bool(const std::filesystem::path& path, std::wstring& error)> recycle;
    // Delete undo: is this deleted item (original path, deletion not before
    // `notBeforeUtc`, pre-delete fingerprint) in the Recycle Bin and restorable?
    // Called for every item before any is restored. `problem` says why not.
    std::function<bool(const std::filesystem::path& original, uint64_t notBeforeUtc, const PathFingerprint& before,
                       std::wstring& problem)>
        locate;
    // Puts one such item back (never overwrites).
    std::function<bool(const std::filesystem::path& original, uint64_t notBeforeUtc, const PathFingerprint& before,
                       std::wstring& error)>
        restore;
};

struct UndoResult {
    bool ok = false;
    size_t reversed = 0;
    std::wstring error;
};

// Verifies, then reverses. Returns ok=false (reversed=0) when the check fails.
UndoResult PerformUndo(const UndoEntry& entry, const UndoActions& actions);

}  // namespace mc
