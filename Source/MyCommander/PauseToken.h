// FOP-015: pause and resume for long-running operations.
//
// Every engine already polls a cancel callback often (per item, and per progress
// tick inside a file). A pause is "that poll waits while paused": the engines
// need no new hook points and no signature change. Cancel always wins over a
// pause. Pure (mutex + condition variable), no Console.
#pragma once

#include <condition_variable>
#include <functional>
#include <mutex>

namespace mc {

class PauseToken {
public:
    void Pause();
    void Resume();
    // Flips the state; returns the new Paused().
    bool Toggle();
    bool Paused() const;

    // Blocks while paused. `cancelled` is polled about every `sliceMs` while
    // waiting and returns true to give up; `tick` (optional) runs on the same
    // cadence so a UI can stay alive. Returns false at once, without waiting,
    // if `cancelled` is true, and true when the wait ended because the token
    // was resumed (or was never paused). Neither callback runs under the
    // token's lock, so either may call Resume().
    bool WaitWhilePaused(const std::function<bool()>& cancelled, const std::function<void()>& tick = {},
                         int sliceMs = 50);

private:
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    bool paused_ = false;
};

}  // namespace mc
