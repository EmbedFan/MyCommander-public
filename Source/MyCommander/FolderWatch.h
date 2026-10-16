// LIST-007: automatic panel refresh on external file-system changes.
//
//   * Debouncer -- pure timing: a burst of change signals becomes one refresh,
//     fired after a quiet period and never later than a cap after the first
//     signal of the burst. The clock is a plain millisecond value passed in, so
//     it is unit-tested without sleeping.
//   * FolderWatch -- one Windows change-notification handle on one folder
//     (FindFirstChangeNotificationW, not recursive). Polled from the main loop;
//     no thread. Console-free, so MyCommanderTests can use it directly.
#pragma once

#include <windows.h>

#include <cstdint>
#include <string>

namespace mc {

class Debouncer {
public:
    explicit Debouncer(uint64_t quietMs = 300, uint64_t capMs = 2000) : quietMs_(quietMs), capMs_(capMs) {}

    // A change was seen at `nowMs`.
    void Signal(uint64_t nowMs) {
        if (!pending_) {
            pending_ = true;
            firstMs_ = nowMs;
        }
        lastMs_ = nowMs;
    }
    bool Pending() const { return pending_; }
    // True once the burst has been quiet for quietMs, or has lasted capMs.
    bool Due(uint64_t nowMs) const {
        if (!pending_) return false;
        return nowMs - lastMs_ >= quietMs_ || nowMs - firstMs_ >= capMs_;
    }
    // The refresh happened (or became unnecessary): forget the burst.
    void Clear() { pending_ = false; }

private:
    uint64_t quietMs_;
    uint64_t capMs_;
    bool pending_ = false;
    uint64_t firstMs_ = 0;
    uint64_t lastMs_ = 0;
};

class FolderWatch {
public:
    enum class State { Closed, Active, Unavailable };

    FolderWatch() = default;
    ~FolderWatch() { Close(); }
    FolderWatch(const FolderWatch&) = delete;
    FolderWatch& operator=(const FolderWatch&) = delete;

    // Watches `folder` (replacing any previous watch). Returns false and
    // becomes Unavailable when the folder can't be watched.
    bool Arm(const std::wstring& folder);
    void Close();

    // True when something in the folder changed since the last call. A handle
    // that stops working (drive removed) becomes Unavailable and reports one
    // final true, so the panel re-lists and shows its banner.
    bool Poll();
    // Discards anything signalled so far (a refresh just re-listed the folder).
    void Drain();

    State GetState() const { return state_; }
    const std::wstring& Folder() const { return folder_; }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    State state_ = State::Closed;
    std::wstring folder_;
};

}  // namespace mc
