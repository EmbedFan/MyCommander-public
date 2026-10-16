#include "PauseToken.h"

#include <chrono>

namespace mc {

void PauseToken::Pause() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = true;
    changed_.notify_all();
}

void PauseToken::Resume() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = false;
    changed_.notify_all();
}

bool PauseToken::Toggle() {
    std::lock_guard<std::mutex> lock(mutex_);
    paused_ = !paused_;
    changed_.notify_all();
    return paused_;
}

bool PauseToken::Paused() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return paused_;
}

bool PauseToken::WaitWhilePaused(const std::function<bool()>& cancelled, const std::function<void()>& tick,
                                 int sliceMs) {
    if (sliceMs < 1) sliceMs = 1;
    for (;;) {
        if (cancelled && cancelled()) return false;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            if (!paused_) return true;
            changed_.wait_for(lock, std::chrono::milliseconds(sliceMs), [this] { return !paused_; });
            // paused_ is changed by Resume() on another thread while wait_for has released the lock.
            // cppcheck-suppress knownConditionTrueFalse
            if (!paused_) {
                lock.unlock();
                // Resumed meanwhile; cancel still wins if it arrived too.
                return !(cancelled && cancelled());
            }
        }
        if (tick) tick();
    }
}

}  // namespace mc
