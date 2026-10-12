#pragma once

#include "DirCompare.h"

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// SRC-007: directory synchronization planned from a DirCompare outcome.
// Pure plan/summary logic plus two small filesystem helpers (the stale
// check), no Console/Dialog dependency; main.cpp's DoSynchronizeDirectories
// owns the form, preview, mandatory confirmation and execution.

enum class SyncDirection { LeftToRight, RightToLeft, Both };
enum class SyncAction { None, CopyToRight, CopyToLeft, DeleteRight, DeleteLeft };

struct SyncOptions {
    SyncDirection direction = SyncDirection::LeftToRight;
    bool deleteExtras = false;  // one-way only: delete what exists only on the target
};

struct SyncItem {
    CompareItem item;
    SyncAction action = SyncAction::None;    // what will happen (the preview may change it)
    SyncAction proposed = SyncAction::None;  // what the rules proposed
    bool conflict = false;                   // the rules couldn't decide safely
    std::wstring reason;                     // short preview text, e.g. L"target is newer"
};

// Builds the plan: one entry per compared item, in the compare's order.
// A folder present on both sides never gets an action of its own (its
// children carry them); a folder on one side only is copied/deleted whole.
std::vector<SyncItem> BuildSyncPlan(const CompareOutcome& outcome, const SyncOptions& options);

// Whether `action` is allowed for `entry` under `options`: no copy from a
// side that lacks the item, no copy of a folder present on both sides, no
// file-onto-folder copy, and deletions only of one-way "extras" (present
// only on the target) with deleteExtras on.
bool CanApply(const SyncItem& entry, SyncAction action, const SyncOptions& options);

// True when the copy replaces something already on the target side.
bool Overwrites(const SyncItem& entry);

struct SyncSummary {
    size_t copyRight = 0;
    size_t copyLeft = 0;
    size_t overwrite = 0;
    size_t deleteRight = 0;
    size_t deleteLeft = 0;
    size_t conflicts = 0;  // unresolved: conflict && action == None
    uint64_t bytesRight = 0;  // file bytes copied to the right (a whole folder counts 0 -- unknown here)
    uint64_t bytesLeft = 0;
    bool Empty() const { return copyRight + copyLeft + deleteRight + deleteLeft == 0; }
};
SyncSummary Summarize(const std::vector<SyncItem>& plan);

// Execution order: every copy group first, then every delete group; within
// each, one group per (action, relative folder), names in plan order.
struct SyncGroup {
    SyncAction action = SyncAction::None;
    std::filesystem::path relativeDir;  // empty = the roots themselves
    std::vector<size_t> planIndices;    // into the plan
};
std::vector<SyncGroup> GroupForExecution(const std::vector<SyncItem>& plan);

// Re-reads `path` (no reparse following); nullopt if it doesn't exist.
std::optional<CompareSide> ReadSide(const std::filesystem::path& path);

// Stale-preview guard: true when `now` no longer matches what the compare
// saw -- existence, file/folder type, and for files size and write time.
bool SideChanged(const std::optional<CompareSide>& planned, const std::optional<CompareSide>& now);

}  // namespace mc
