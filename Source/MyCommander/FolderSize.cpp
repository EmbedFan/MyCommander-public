#include "FolderSize.h"

#include "DiskUsage.h"
#include "TextCase.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace mc {

std::wstring FolderSizeCache::KeyOf(const fs::path& path) {
    std::wstring s = ToLowerInvariant(path.lexically_normal().wstring());
    for (wchar_t& c : s)
        if (c == L'/') c = L'\\';
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    return s;
}

FolderSizeCache::Entry& FolderSizeCache::Touch(const std::wstring& key) {
    Entry& e = entries_[key];
    e.lastShown = ++tick_;
    return e;
}

void FolderSizeCache::EvictIfNeeded() {
    while (entries_.size() > capacity_ && !entries_.empty()) {
        auto oldest = entries_.begin();
        for (auto it = entries_.begin(); it != entries_.end(); ++it) {
            // never evict something being measured
            const bool busy = it->second.state == FolderSizeState::Computing || it->second.state == FolderSizeState::Pending;
            const bool oldestBusy = oldest->second.state == FolderSizeState::Computing || oldest->second.state == FolderSizeState::Pending;
            if ((oldestBusy && !busy) || (busy == oldestBusy && it->second.lastShown < oldest->second.lastShown)) oldest = it;
        }
        entries_.erase(oldest);
    }
}

FolderSizeLookup FolderSizeCache::Get(const fs::path& path, uint64_t currentWriteTime) {
    FolderSizeLookup r;
    auto it = entries_.find(KeyOf(path));
    if (it == entries_.end()) return r;
    Entry& e = it->second;
    e.lastShown = ++tick_;
    r.state = e.state;
    r.hasValue = e.hasValue;
    r.bytes = e.bytes;
    r.unreadable = e.unreadable;
    r.stale = e.hasValue && (e.invalidated || (currentWriteTime != 0 && e.writeTime != currentWriteTime));
    return r;
}

bool FolderSizeCache::NeedsMeasuring(const fs::path& path, uint64_t currentWriteTime) {
    FolderSizeLookup l = Get(path, currentWriteTime);
    if (l.state == FolderSizeState::Pending || l.state == FolderSizeState::Computing) return false;
    return !l.hasValue || l.stale;
}

void FolderSizeCache::MarkPending(const fs::path& path) {
    Entry& e = Touch(KeyOf(path));
    e.state = FolderSizeState::Pending;
    EvictIfNeeded();
}

void FolderSizeCache::MarkComputing(const fs::path& path) {
    Touch(KeyOf(path)).state = FolderSizeState::Computing;
}

void FolderSizeCache::Complete(const fs::path& path, uint64_t bytes, uint64_t unreadable, uint64_t writeTime) {
    Entry& e = Touch(KeyOf(path));
    e.state = FolderSizeState::Done;
    e.hasValue = true;
    e.invalidated = false;
    e.bytes = bytes;
    e.unreadable = unreadable;
    e.writeTime = writeTime;
    EvictIfNeeded();
}

void FolderSizeCache::Abandon(const fs::path& path) {
    auto it = entries_.find(KeyOf(path));
    if (it == entries_.end()) return;
    it->second.state = it->second.hasValue ? FolderSizeState::Done : FolderSizeState::Unknown;
}

void FolderSizeCache::InvalidateAll() {
    for (auto& kv : entries_)
        if (kv.second.hasValue) kv.second.invalidated = true;
}

// --- worker ----------------------------------------------------------------

FolderSizeWorker::FolderSizeWorker() {
    wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    thread_ = CreateThread(nullptr, 0, &FolderSizeWorker::ThreadMain, this, 0, nullptr);
}

FolderSizeWorker::~FolderSizeWorker() {
    stop_ = true;
    cancelCurrent_ = true;
    if (wake_) SetEvent(wake_);
    if (thread_) {
        WaitForSingleObject(thread_, 10000);
        CloseHandle(thread_);
    }
    if (wake_) CloseHandle(wake_);
}

unsigned long __stdcall FolderSizeWorker::ThreadMain(void* self) {
    static_cast<FolderSizeWorker*>(self)->Run();
    return 0;
}

void FolderSizeWorker::Run() {
    while (!stop_) {
        std::pair<fs::path, uint64_t> job;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty()) {
                running_.clear();
            } else {
                job = queue_.front();
                queue_.pop_front();
                running_ = FolderSizeCache::KeyOf(job.first);
                cancelCurrent_ = false;
                cache_.MarkComputing(job.first);
            }
        }
        if (job.first.empty()) {
            WaitForSingleObject(wake_, 200);
            continue;
        }
        DiskUsageResult r = SumTree(job.first, [&]() { return stop_.load() || cancelCurrent_.load(); });
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (r.cancelled) {
                cache_.Abandon(job.first);
            } else {
                cache_.Complete(job.first, r.bytes, r.unreadableFolders, job.second);
                changed_ = true;
            }
            running_.clear();
        }
    }
}

FolderSizeLookup FolderSizeWorker::Lookup(const fs::path& path, uint64_t currentWriteTime) {
    std::lock_guard<std::mutex> lock(mutex_);
    return cache_.Get(path, currentWriteTime);
}

void FolderSizeWorker::Want(const std::vector<std::pair<fs::path, uint64_t>>& visible) {
    bool wake = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        // Anything queued that is no longer on screen goes back to unknown.
        for (const auto& q : queue_) {
            bool stillWanted = std::any_of(visible.begin(), visible.end(), [&](const auto& v) {
                return FolderSizeCache::KeyOf(v.first) == FolderSizeCache::KeyOf(q.first);
            });
            if (!stillWanted) cache_.Abandon(q.first);
        }
        queue_.clear();
        bool runningWanted = running_.empty();
        for (const auto& v : visible) {
            if (!running_.empty() && FolderSizeCache::KeyOf(v.first) == running_) runningWanted = true;
            if (!cache_.NeedsMeasuring(v.first, v.second)) continue;
            cache_.MarkPending(v.first);
            queue_.push_back(v);
            wake = true;
        }
        if (!runningWanted) cancelCurrent_ = true;  // navigated away from it
    }
    if (wake && wake_) SetEvent(wake_);
}

void FolderSizeWorker::InvalidateAll() {
    std::lock_guard<std::mutex> lock(mutex_);
    cache_.InvalidateAll();
}

// cppcheck-suppress unusedFunction -- used by FolderSizeTests (not scanned)
bool FolderSizeWorker::TakeChanged() { return changed_.exchange(false); }

// cppcheck-suppress unusedFunction -- used by FolderSizeTests (not scanned)
bool FolderSizeWorker::WaitIdle(unsigned timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    while (GetTickCount64() < deadline) {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (queue_.empty() && running_.empty()) return true;
        }
        if (wake_) SetEvent(wake_);
        Sleep(10);
    }
    return false;
}

}  // namespace mc
