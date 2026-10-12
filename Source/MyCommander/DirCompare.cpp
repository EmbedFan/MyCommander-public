#include "DirCompare.h"

#include "PathUtil.h"

#include <algorithm>
#include <cstring>
#include <memory>

namespace fs = std::filesystem;

namespace mc {

namespace {

constexpr uint64_t kTicksPerSecond = 10'000'000ULL;  // FILETIME is in 100 ns units
constexpr DWORD kChunkBytes = 64 * 1024;

// <0, 0, >0 -- ordinal, case-insensitive (how NTFS itself matches names).
int CompareNames(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) -
           CSTR_EQUAL;
}

// 0 when equal within the tolerance (or exactly one hour apart, if asked),
// else which side is newer.
int CompareTimes(uint64_t left, uint64_t right, const CompareOptions& options) {
    uint64_t diff = left > right ? left - right : right - left;
    uint64_t tolerance = static_cast<uint64_t>(options.toleranceSeconds) * kTicksPerSecond;
    if (diff <= tolerance) return 0;
    if (options.ignoreOneHour) {
        const uint64_t hour = 3600 * kTicksPerSecond;
        uint64_t offHour = diff > hour ? diff - hour : hour - diff;
        if (offHour <= tolerance) return 0;
    }
    return left > right ? 1 : -1;
}

using Listing = std::vector<std::pair<std::wstring, CompareSide>>;

// Lists `dir` with FindFirstFileExW (name, size, time and attributes in one
// call), sorted by name case-insensitively. False if it can't be listed.
bool ListDirectory(const fs::path& dir, bool includeHidden, const std::wstring& mask, Listing& out) {
    fs::path pattern = ToLongPath(dir) / L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;  // an empty drive root
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        if ((fd.dwFileAttributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0 && !includeHidden) continue;
        CompareSide side;
        side.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
        if (!side.isDirectory && !mask.empty() && !WildcardMatch(name, mask)) continue;
        side.isReparsePoint = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
        side.attributes = fd.dwFileAttributes;
        side.size = side.isDirectory ? 0 : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        side.lastWriteUtc =
            (static_cast<uint64_t>(fd.ftLastWriteTime.dwHighDateTime) << 32) | fd.ftLastWriteTime.dwLowDateTime;
        out.emplace_back(std::move(name), side);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    std::sort(out.begin(), out.end(), [](const auto& a, const auto& b) { return CompareNames(a.first, b.first) < 0; });
    return true;
}

struct HandleCloser {
    void operator()(HANDLE h) const {
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }
};
using UniqueHandle = std::unique_ptr<void, HandleCloser>;

UniqueHandle OpenForReading(const fs::path& p) {
    HANDLE h = CreateFileW(ToLongPath(p).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    return UniqueHandle(h == INVALID_HANDLE_VALUE ? nullptr : h);
}

// Reads up to `size` bytes, looping over short reads. False on a read error.
bool ReadFull(HANDLE h, char* buffer, DWORD size, DWORD& got) {
    got = 0;
    while (got < size) {
        DWORD n = 0;
        if (!ReadFile(h, buffer + got, size - got, &n, nullptr)) return false;
        if (n == 0) break;  // end of file
        got += n;
    }
    return true;
}

std::optional<bool> ContentEqual(const fs::path& a, const fs::path& b, const SearchCancelPoll& cancelPoll,
                                 uint64_t* bytesRead, bool* cancelled, const std::function<void()>& onChunk) {
    if (cancelled) *cancelled = false;
    UniqueHandle ha = OpenForReading(a);
    UniqueHandle hb = OpenForReading(b);
    if (!ha || !hb) return std::nullopt;
    std::vector<char> bufA(kChunkBytes), bufB(kChunkBytes);
    for (;;) {
        if (cancelPoll && cancelPoll()) {
            if (cancelled) *cancelled = true;
            return std::nullopt;
        }
        DWORD gotA = 0, gotB = 0;
        if (!ReadFull(ha.get(), bufA.data(), kChunkBytes, gotA) || !ReadFull(hb.get(), bufB.data(), kChunkBytes, gotB)) {
            return std::nullopt;
        }
        if (bytesRead) *bytesRead += static_cast<uint64_t>(gotA) + gotB;
        if (onChunk) onChunk();
        if (gotA != gotB) return false;  // sizes differ (or one changed while reading)
        if (gotA == 0) return true;
        if (std::memcmp(bufA.data(), bufB.data(), gotA) != 0) return false;
    }
}

struct Context {
    const fs::path& leftRoot;
    const fs::path& rightRoot;
    const CompareOptions& options;
    const SearchCancelPoll& cancelPoll;
    const CompareProgressCallback& onProgress;
    CompareOutcome& out;
    size_t itemsCompared = 0;

    void Report(const std::wstring& current) {
        if (onProgress) onProgress(CompareProgress{itemsCompared, out.bytesRead, current});
    }
};

struct MergedEntry {
    std::wstring name;
    std::optional<CompareSide> left;
    std::optional<CompareSide> right;
    bool AnyDirectory() const { return (left && left->isDirectory) || (right && right->isDirectory); }
};

std::vector<MergedEntry> Merge(const Listing& left, const Listing& right) {
    std::vector<MergedEntry> merged;
    size_t i = 0, j = 0;
    while (i < left.size() || j < right.size()) {
        int cmp = i >= left.size() ? 1 : j >= right.size() ? -1 : CompareNames(left[i].first, right[j].first);
        if (cmp == 0) {
            merged.push_back({left[i].first, left[i].second, right[j].second});
            ++i;
            ++j;
        } else if (cmp < 0) {
            merged.push_back({left[i].first, left[i].second, std::nullopt});
            ++i;
        } else {
            merged.push_back({right[j].first, std::nullopt, right[j].second});
            ++j;
        }
    }
    // Folders first, names kept in order within each group.
    std::stable_partition(merged.begin(), merged.end(), [](const MergedEntry& e) { return e.AnyDirectory(); });
    return merged;
}

// Compares one level; returns true when every item in it is Identical.
bool CompareLevel(Context& ctx, const fs::path& relDir, int depth, const Listing& left, const Listing& right) {
    CompareOutcome& out = ctx.out;
    bool allIdentical = true;
    for (MergedEntry& entry : Merge(left, right)) {
        if (out.cancelled || out.truncated) return false;
        if (ctx.cancelPoll && ctx.cancelPoll()) {
            out.cancelled = true;
            return false;
        }
        if (out.items.size() >= ctx.options.maxItems) {
            out.truncated = true;
            return false;
        }

        fs::path rel = relDir.empty() ? fs::path(entry.name) : relDir / entry.name;
        size_t index = out.items.size();
        out.items.push_back(CompareItem{rel.wstring(), entry.name, depth, entry.left, entry.right});
        ++ctx.itemsCompared;
        ctx.Report(rel.wstring());

        const auto& l = entry.left;
        const auto& r = entry.right;
        bool bothFiles = l && r && !l->isDirectory && !r->isDirectory;
        if (bothFiles) ++out.filesCompared;

        std::optional<bool> contentEqual;
        if (ctx.options.method == CompareMethod::Content && bothFiles && l->size == r->size) {
            if (l->isReparsePoint || r->isReparsePoint) {
                contentEqual = true;  // SEC-006: never read through a reparse point
            } else {
                bool cancelled = false;
                size_t chunks = 0;
                std::wstring current = rel.wstring();
                contentEqual = ContentEqual(ctx.leftRoot / rel, ctx.rightRoot / rel, ctx.cancelPoll, &out.bytesRead,
                                            &cancelled, [&] {
                                                if ((++chunks & 15) == 0) ctx.Report(current);  // every 1 MB
                                            });
                if (cancelled) {
                    out.items.pop_back();
                    out.cancelled = true;
                    return false;
                }
            }
        }

        CompareStatus status = ClassifyPair(l, r, ctx.options, contentEqual);
        if (status == CompareStatus::Unreadable) ++out.unreadable;

        if (ctx.options.recursive && l && r && l->isDirectory && r->isDirectory && !l->isReparsePoint &&
            !r->isReparsePoint) {
            Listing childLeft, childRight;
            bool okLeft = ListDirectory(ctx.leftRoot / rel, ctx.options.includeHiddenLeft, ctx.options.fileMaskLeft,
                                        childLeft);
            bool okRight = ListDirectory(ctx.rightRoot / rel, ctx.options.includeHiddenRight,
                                         ctx.options.fileMaskRight, childRight);
            if (!okLeft || !okRight) {
                status = CompareStatus::Unreadable;
                ++out.unreadable;
            } else if (!CompareLevel(ctx, rel, depth + 1, childLeft, childRight)) {
                status = CompareStatus::Different;
            }
        }

        out.items[index].status = status;
        if (status != CompareStatus::Identical) allIdentical = false;
    }
    return allIdentical;
}

}  // namespace

CompareStatus ClassifyPair(const std::optional<CompareSide>& left, const std::optional<CompareSide>& right,
                           const CompareOptions& options, std::optional<bool> contentEqual) {
    if (!left && !right) return CompareStatus::Identical;
    if (!right) return CompareStatus::LeftOnly;
    if (!left) return CompareStatus::RightOnly;
    if (left->isDirectory != right->isDirectory) return CompareStatus::TypeMismatch;
    if (left->isDirectory) return CompareStatus::Identical;  // recursive roll-up is the caller's job

    auto newerOr = [&](CompareStatus fallback) {
        int t = CompareTimes(left->lastWriteUtc, right->lastWriteUtc, options);
        return t > 0 ? CompareStatus::LeftNewer : t < 0 ? CompareStatus::RightNewer : fallback;
    };
    bool sizeDiffers = left->size != right->size;

    switch (options.method) {
    case CompareMethod::Name:
        return CompareStatus::Identical;
    case CompareMethod::Size:
        return sizeDiffers ? CompareStatus::Different : CompareStatus::Identical;
    case CompareMethod::Timestamp: {
        CompareStatus byTime = newerOr(CompareStatus::Identical);
        if (byTime != CompareStatus::Identical) return byTime;
        return sizeDiffers ? CompareStatus::Different : CompareStatus::Identical;
    }
    case CompareMethod::Content:
        if (sizeDiffers) return newerOr(CompareStatus::Different);
        if (!contentEqual) return CompareStatus::Unreadable;
        return *contentEqual ? CompareStatus::Identical : newerOr(CompareStatus::Different);
    }
    return CompareStatus::Identical;
}

// Public API, called by DirCompareTests.cpp (not part of the scan).
// cppcheck-suppress unusedFunction
std::optional<bool> FilesHaveEqualContent(const fs::path& a, const fs::path& b, const SearchCancelPoll& cancelPoll,
                                          uint64_t* bytesRead, bool* cancelled) {
    return ContentEqual(a, b, cancelPoll, bytesRead, cancelled, {});
}

CompareOutcome CompareDirectories(const fs::path& left, const fs::path& right, const CompareOptions& options,
                                  const SearchCancelPoll& cancelPoll, const CompareProgressCallback& onProgress) {
    CompareOutcome out;
    Listing leftListing, rightListing;
    out.leftListed = ListDirectory(left, options.includeHiddenLeft, options.fileMaskLeft, leftListing);
    out.rightListed = ListDirectory(right, options.includeHiddenRight, options.fileMaskRight, rightListing);
    if (!out.leftListed || !out.rightListed) return out;
    Context ctx{left, right, options, cancelPoll, onProgress, out};
    CompareLevel(ctx, fs::path(), 0, leftListing, rightListing);
    return out;
}

}  // namespace mc
