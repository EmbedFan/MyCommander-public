#pragma once

// LIST-010: folder sizes in the panel's Size column. `FolderSizeCache` is the
// pure bookkeeping (lexical case-insensitive key, per-folder state, staleness
// by last-write time or an explicit invalidation, least-recently-shown
// eviction); `FolderSizeWorker` measures folders one at a time on a single
// background thread with DiskUsage's SumTree and publishes into a cache the UI
// thread reads without ever waiting. Nothing is persisted: sizes change, and a
// stale number on disk would mislead. No Console/Panel dependency.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mc {

enum class FolderSizeState { Unknown, Pending, Computing, Done };

struct FolderSizeLookup {
    FolderSizeState state = FolderSizeState::Unknown;
    bool hasValue = false;   // a measured total exists (possibly stale)
    bool stale = false;      // it should be measured again
    uint64_t bytes = 0;
    uint64_t unreadable = 0; // folders that could not be listed
};

class FolderSizeCache {
public:
    static constexpr size_t kDefaultCapacity = 2000;
    explicit FolderSizeCache(size_t capacity = kDefaultCapacity) : capacity_(capacity) {}

    // The state for `path`, given the folder's current last-write time (FILETIME
    // ticks, 0 if unknown). Counts as "shown" for the eviction order.
    FolderSizeLookup Get(const std::filesystem::path& path, uint64_t currentWriteTime);

    // True when nothing is known, or the value is stale, and no measurement is
    // already queued or running for it.
    bool NeedsMeasuring(const std::filesystem::path& path, uint64_t currentWriteTime);

    void MarkPending(const std::filesystem::path& path);
    void MarkComputing(const std::filesystem::path& path);
    // A finished measurement taken while the folder's last-write time was `writeTime`.
    void Complete(const std::filesystem::path& path, uint64_t bytes, uint64_t unreadable, uint64_t writeTime);
    // The measurement did not finish (cancelled): back to its previous value, if any.
    void Abandon(const std::filesystem::path& path);
    // A refresh (Ctrl+R, auto-refresh): every finished value becomes stale.
    void InvalidateAll();

    size_t Size() const { return entries_.size(); }
    size_t Capacity() const { return capacity_; }

    // The lexical, case- and separator-insensitive key used for every lookup.
    static std::wstring KeyOf(const std::filesystem::path& path);

private:
    struct Entry {
        FolderSizeState state = FolderSizeState::Unknown;
        bool hasValue = false;
        bool invalidated = false;
        uint64_t bytes = 0;
        uint64_t unreadable = 0;
        uint64_t writeTime = 0;
        uint64_t lastShown = 0;
    };
    Entry& Touch(const std::wstring& key);
    void EvictIfNeeded();

    size_t capacity_;
    uint64_t tick_ = 0;
    std::unordered_map<std::wstring, Entry> entries_;
};

// One background thread, one folder at a time. All public methods are called
// from the UI thread and never block on a measurement.
class FolderSizeWorker {
public:
    FolderSizeWorker();
    ~FolderSizeWorker();
    FolderSizeWorker(const FolderSizeWorker&) = delete;
    FolderSizeWorker& operator=(const FolderSizeWorker&) = delete;

    FolderSizeLookup Lookup(const std::filesystem::path& path, uint64_t currentWriteTime);

    // Replaces the queue with the folders now on screen (missing or stale ones
    // only). A running measurement whose folder is no longer wanted is cancelled.
    void Want(const std::vector<std::pair<std::filesystem::path, uint64_t>>& visible);

    void InvalidateAll();

    // True once if a measurement finished since the last call (a redraw hint).
    bool TakeChanged();

    // Test aid: waits until nothing is queued or running (or the timeout, ms).
    bool WaitIdle(unsigned timeoutMs);

private:
    static unsigned long __stdcall ThreadMain(void* self);
    void Run();

    std::mutex mutex_;
    FolderSizeCache cache_;
    std::deque<std::pair<std::filesystem::path, uint64_t>> queue_;
    std::wstring running_;              // key of the folder being measured, empty if none
    std::atomic<bool> cancelCurrent_{false};
    std::atomic<bool> stop_{false};
    std::atomic<bool> changed_{false};
    void* wake_ = nullptr;   // auto-reset event
    void* thread_ = nullptr;
};

}  // namespace mc
