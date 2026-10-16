#include "TestFixtures.h"
#include "TestFramework.h"

#include "FileOps.h"
#include "PauseToken.h"

#include <atomic>
#include <chrono>
#include <functional>

namespace fs = std::filesystem;

using mc::PauseToken;

namespace {

// std::thread isn't usable in this project (see JobWorker.cpp), so a tiny
// Win32 helper runs `fn` on another thread after `delayMs`.
struct DelayedCall {
    std::function<void()> fn;
    DWORD delayMs;
    HANDLE thread = nullptr;

    DelayedCall(DWORD delay, std::function<void()> f) : fn(std::move(f)), delayMs(delay) {
        thread = CreateThread(nullptr, 0, &DelayedCall::Entry, this, 0, nullptr);
    }
    ~DelayedCall() {
        if (thread) {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
        }
    }
    DelayedCall(const DelayedCall&) = delete;
    DelayedCall& operator=(const DelayedCall&) = delete;

    static DWORD WINAPI Entry(LPVOID param) {
        auto* self = static_cast<DelayedCall*>(param);
        Sleep(self->delayMs);
        self->fn();
        return 0;
    }
};

long long ElapsedMs(std::chrono::steady_clock::time_point since) {
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - since).count();
}

}  // namespace

TEST_CASE(PauseToken_NotPausedNeverWaits_FOP015) {
    PauseToken token;
    CHECK(!token.Paused());
    auto start = std::chrono::steady_clock::now();
    CHECK(token.WaitWhilePaused([] { return false; }));
    CHECK(ElapsedMs(start) < 200);
}

TEST_CASE(PauseToken_WaitsWhilePausedAndResumes_FOP015) {
    PauseToken token;
    token.Pause();
    CHECK(token.Paused());
    auto start = std::chrono::steady_clock::now();
    bool result = false;
    {
        DelayedCall resumer(250, [&] { token.Resume(); });
        result = token.WaitWhilePaused([] { return false; });
    }
    CHECK(result);                    // resumed, not cancelled
    CHECK(ElapsedMs(start) >= 200);   // really waited
    CHECK(!token.Paused());
}

TEST_CASE(PauseToken_CancelWhilePausedReturnsAtOnce_FOP015) {
    PauseToken token;
    token.Pause();
    std::atomic<bool> cancel{false};
    auto start = std::chrono::steady_clock::now();
    bool result = true;
    {
        DelayedCall canceller(150, [&] { cancel.store(true); });
        result = token.WaitWhilePaused([&] { return cancel.load(); });
    }
    CHECK(!result);                   // cancel wins
    CHECK(ElapsedMs(start) < 3000);
    CHECK(token.Paused());            // the token itself is untouched
}

TEST_CASE(PauseToken_AlreadyCancelledDoesNotWait_FOP015) {
    PauseToken token;
    token.Pause();
    auto start = std::chrono::steady_clock::now();
    CHECK(!token.WaitWhilePaused([] { return true; }));
    CHECK(ElapsedMs(start) < 200);
}

TEST_CASE(PauseToken_TickRunsWhilePausedAndCanResume_FOP015) {
    PauseToken token;
    token.Pause();
    int ticks = 0;
    bool result = token.WaitWhilePaused([] { return false; },
                                        [&] {
                                            if (++ticks == 3) token.Resume();  // a UI tick handling the P key
                                        },
                                        10);
    CHECK(result);
    CHECK(ticks == 3);
}

TEST_CASE(PauseToken_ToggleFlipsAndReports_FOP015) {
    PauseToken token;
    CHECK(token.Toggle());
    CHECK(token.Paused());
    CHECK(!token.Toggle());
    CHECK(!token.Paused());
}

// --- FileOps with a pausable cancel poll (the engine itself is unchanged) -----

namespace {

void MakeFiles(const fs::path& dir, int count, std::vector<std::wstring>& names) {
    for (int i = 0; i < count; ++i) {
        wchar_t name[32];
        swprintf_s(name, L"f%02d.txt", i);
        test::WriteFileContent(dir / name, std::string(1000 + static_cast<size_t>(i), static_cast<char>('a' + i)));
        names.push_back(name);
    }
}

mc::OperationOutcome RunCopy(const fs::path& src, const std::vector<std::wstring>& names, const fs::path& dst,
                             const mc::CancelPoll& cancelPoll) {
    return mc::CopyItems(
        src, names, dst, [](const fs::path&) { return mc::ConflictAskResult{mc::ConflictChoice::Overwrite}; },
        [](const std::wstring&, int, int) { return true; }, cancelPoll,
        [](const std::wstring&, const std::wstring&, DWORD) { return mc::ErrorAskResult{mc::ErrorChoice::Cancel}; });
}

}  // namespace

TEST_CASE(FileOps_PausedCopyDelaysButCompletesIntact_FOP015) {
    test::TempDir src;
    test::TempDir dst;
    std::vector<std::wstring> names;
    MakeFiles(src.Path(), 10, names);

    PauseToken token;
    int polls = 0;
    auto cancelPoll = [&]() {
        if (++polls == 3) token.Pause();
        return !token.WaitWhilePaused([] { return false; });
    };
    auto start = std::chrono::steady_clock::now();
    mc::OperationOutcome outcome;
    {
        DelayedCall resumer(300, [&] { token.Resume(); });
        outcome = RunCopy(src.Path(), names, dst.Path(), cancelPoll);
    }
    CHECK(!outcome.cancelled);
    CHECK(outcome.failed.empty());
    CHECK(ElapsedMs(start) >= 250);  // it really waited for the resume
    for (const auto& name : names) {
        CHECK(test::ReadFileContent(dst.Path() / name) == test::ReadFileContent(src.Path() / name));
    }
}

TEST_CASE(FileOps_CancelWhilePausedStopsTheCopy_FOP015) {
    test::TempDir src;
    test::TempDir dst;
    std::vector<std::wstring> names;
    MakeFiles(src.Path(), 10, names);

    PauseToken token;
    std::atomic<bool> cancel{false};
    int polls = 0;
    auto cancelPoll = [&]() {
        if (++polls == 3) token.Pause();
        return !token.WaitWhilePaused([&] { return cancel.load(); });
    };
    mc::OperationOutcome outcome;
    {
        DelayedCall canceller(200, [&] { cancel.store(true); });
        outcome = RunCopy(src.Path(), names, dst.Path(), cancelPoll);
    }
    CHECK(outcome.cancelled);
    int copied = 0;
    for (const auto& entry : fs::directory_iterator(dst.Path())) {
        (void)entry;
        ++copied;
    }
    CHECK(copied < 10);
}
