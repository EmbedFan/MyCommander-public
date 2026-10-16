#include "DiskUsage.h"

#include "DirectoryTree.h"  // FindFilesystemRoot
#include "PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mc {

namespace {

uint64_t ClusterSizeFor(const std::filesystem::path& root) {
    DWORD sectorsPerCluster = 0, bytesPerSector = 0, freeClusters = 0, totalClusters = 0;
    std::filesystem::path volumeRoot = FindFilesystemRoot(root);
    if (!GetDiskFreeSpaceW(volumeRoot.c_str(), &sectorsPerCluster, &bytesPerSector, &freeClusters, &totalClusters) ||
        sectorsPerCluster == 0 || bytesPerSector == 0) {
        return 4096;  // a sane fallback if the volume can't report it (e.g. some network shares)
    }
    return static_cast<uint64_t>(sectorsPerCluster) * bytesPerSector;
}

// GetCompressedFileSizeW gives the real on-disk size for a compressed or
// sparse file directly; every other file is rounded up to the volume's
// cluster size, since NTFS/FAT never allocate less than one cluster.
uint64_t AllocatedSizeOf(const std::filesystem::path& file, uint64_t logicalSize, DWORD attributes,
                         uint64_t clusterSize) {
    if ((attributes & (FILE_ATTRIBUTE_COMPRESSED | FILE_ATTRIBUTE_SPARSE_FILE)) != 0) {
        DWORD high = 0;
        DWORD low = GetCompressedFileSizeW(ToLongPath(file).c_str(), &high);
        if (low != INVALID_FILE_SIZE || GetLastError() == NO_ERROR) {
            return (static_cast<uint64_t>(high) << 32) | low;
        }
    }
    if (clusterSize == 0) return logicalSize;
    return ((logicalSize + clusterSize - 1) / clusterSize) * clusterSize;
}

void WalkDirectory(const std::filesystem::path& dir, uint64_t clusterSize, const DiskUsageCancelPoll& cancelPoll,
                   const DiskUsageProgressCallback& onProgress, DiskUsageResult& result) {
    if (cancelPoll && cancelPoll()) {
        result.cancelled = true;
        return;
    }
    std::filesystem::path pattern = ToLongPath(dir) / L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() != ERROR_FILE_NOT_FOUND) ++result.unreadableFolders;  // an empty dir is not an error
        return;
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        std::filesystem::path entryPath = dir / name;
        bool isDir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        bool isReparsePoint = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        if (isDir) {
            ++result.folders;
            if (!isReparsePoint) {  // SEC-006: never descend into a reparse point
                WalkDirectory(entryPath, clusterSize, cancelPoll, onProgress, result);
                if (result.cancelled) return;
            }
        } else {
            uint64_t size = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            result.bytes += size;
            result.allocated += AllocatedSizeOf(entryPath, size, fd.dwFileAttributes, clusterSize);
            ++result.files;
        }
        if (onProgress) onProgress(DiskUsageProgress{dir.wstring(), result.files, result.bytes});
        if (cancelPoll && cancelPoll()) {
            result.cancelled = true;
            FindClose(h);
            return;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

}  // namespace

DiskUsageResult SumTree(const std::filesystem::path& root, const DiskUsageCancelPoll& cancelPoll,
                        const DiskUsageProgressCallback& onProgress) {
    DiskUsageResult result;
    DWORD attrs = GetAttributesOrInvalid(root);
    if (attrs == INVALID_FILE_ATTRIBUTES) return result;

    uint64_t clusterSize = ClusterSizeFor(root);
    bool isDir = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    if (!isDir) {
        WIN32_FILE_ATTRIBUTE_DATA data{};
        if (GetFileAttributesExW(ToLongPath(root).c_str(), GetFileExInfoStandard, &data)) {
            uint64_t size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
            result.bytes = size;
            result.allocated = AllocatedSizeOf(root, size, data.dwFileAttributes, clusterSize);
            result.files = 1;
        }
        return result;
    }

    bool isReparsePoint = (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    if (!isReparsePoint) WalkDirectory(root, clusterSize, cancelPoll, onProgress, result);
    return result;
}

} // namespace mc
