// SRC-009: finding empty folders under a chosen folder. Pure logic (no Console,
// no Dialog): a recursive Win32 listing walk in the same shape as DiskUsage.cpp
// / DirCompare.cpp (FindFirstFileExW, cancel poll, progress callback), unit-
// tested on real temp trees. `main.cpp` owns the options form, the results list
// and the confirmed Recycle Bin deletion (FileOps::DeleteItems).
//
// What "empty" means is deliberately conservative -- the cost of a wrong
// answer is a deleted folder that mattered:
//   * a folder holding any file is not empty (a hidden or system file counts);
//   * a reparse point (junction/symlink) is never entered, never reported and
//     makes its parent non-empty (SEC-006: it is an entry, and what it points to
//     is not this tree's);
//   * a folder that cannot be listed is unknown, not empty: never reported,
//     counted in `unreadable`, and it keeps its parent non-empty;
//   * a hidden/system *folder* the scan is told not to enter is treated as
//     content too, so deleting its parent can never take it along unseen.
#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace mc {

struct EmptyFolderOptions {
    // Enter hidden/system folders. Off: such a folder is skipped, and counts as
    // content for its parent.
    bool includeHidden = false;
    // A folder whose subfolders are all empty is itself empty; the topmost
    // folder of such a chain is reported once (with the number of folders under
    // it). Off: only folders with no subfolder at all are empty.
    bool chainsCountAsEmpty = true;
    // Thumbs.db and desktop.ini (case-insensitive) don't count as content.
    bool ignoreJunkFiles = true;
    size_t maxResults = 5000;
};

struct EmptyFolder {
    std::filesystem::path path;
    size_t emptyChildren = 0;  // empty folders below it (0 for a plain empty folder)
};

struct EmptyFoldersOutcome {
    std::vector<EmptyFolder> folders;
    bool cancelled = false;
    bool truncated = false;     // hit maxResults before the walk finished
    size_t unreadable = 0;      // folders that could not be listed (never reported)
};

using EmptyFoldersCancelPoll = std::function<bool()>;
// Called with the folder being listed, so a caller can show a live line.
using EmptyFoldersProgress = std::function<void(const std::filesystem::path& current)>;

// Walks `root` (never reported itself). Folders are returned in the order the
// walk meets them.
EmptyFoldersOutcome FindEmptyFolders(const std::filesystem::path& root, const EmptyFolderOptions& options,
                                     const EmptyFoldersCancelPoll& cancelPoll = {},
                                     const EmptyFoldersProgress& onProgress = {});

// Re-checks one folder right now with the same rules (used immediately before a
// deletion, so a file that appeared since the scan keeps its folder). False for
// a folder that is not empty, cannot be listed, is a reparse point, or is gone.
bool IsFolderEmptyNow(const std::filesystem::path& folder, const EmptyFolderOptions& options);

}  // namespace mc
