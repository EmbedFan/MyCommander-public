#pragma once

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

namespace mc {

// FOP-020: Used Disk Area. Pure (no Console/Dialog dependency, matching
// DirCompare.h's own precedent -- it still does real Win32 file I/O, just
// nothing UI-facing) so it's unit-testable on its own; main.cpp's
// DoDiskUsage owns the progress dialog and result list.

struct DiskUsageResult {
    uint64_t bytes = 0;       // apparent (logical) size, every file counted
    uint64_t allocated = 0;   // on-disk, cluster-rounded (or the real compressed/sparse size)
    uint64_t files = 0;
    uint64_t folders = 0;     // the root itself, if it's a directory, is not counted
    uint64_t unreadableFolders = 0;
    bool cancelled = false;
};

using DiskUsageCancelPoll = std::function<bool()>;

struct DiskUsageProgress {
    std::wstring currentFolder;
    uint64_t filesSoFar = 0;
    uint64_t bytesSoFar = 0;
};
using DiskUsageProgressCallback = std::function<void(const DiskUsageProgress&)>;

// Sums `root` (a single file or a directory tree). Never descends into a
// reparse point (SEC-006) -- it is still counted as one folder entry, with
// nothing inside contributing to the totals. A subfolder that can't be
// listed (access denied, etc.) is counted in `unreadableFolders` and
// skipped, not treated as an error for the whole scan.
DiskUsageResult SumTree(const std::filesystem::path& root, const DiskUsageCancelPoll& cancelPoll = {},
                        const DiskUsageProgressCallback& onProgress = {});

} // namespace mc
