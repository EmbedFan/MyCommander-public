#pragma once

#include "FileOps.h"
#include "OperationQueue.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// FOP-014, Phase 2: runs queued jobs on background threads, one job at a time
// per volume (OperationQueue decides what may start). Worker threads call
// FileOps and never touch the console or the panels; they publish plain data
// that the UI thread reads: job snapshots and pending prompts.
//
// A name clash or a non-collision failure needs an answer. The worker turns it
// into a PendingPrompt and waits. The UI thread answers it with AnswerConflict
// or AnswerError; the worker resumes with that answer. The worker also stops
// waiting if the job is cancelled or the JobWorker is destroyed.

enum class PromptKind { Conflict, Error };

struct PendingPrompt {
    uint64_t seq = 0;                // the id to pass back to AnswerConflict/AnswerError
    uint64_t jobId = 0;
    PromptKind kind = PromptKind::Conflict;
    std::filesystem::path path;      // Conflict: the destination; Error: the item's name
    std::wstring text;               // Error: the error message; empty for a conflict
};

class JobWorker {
public:
    JobWorker();
    // Cancels every job and answers outstanding prompts with Cancel, so no
    // worker thread is left waiting; then joins them all. Queued jobs that
    // never started are dropped with the worker (nothing is persisted).
    ~JobWorker();
    JobWorker(const JobWorker&) = delete;
    JobWorker& operator=(const JobWorker&) = delete;

    uint64_t Enqueue(Job job);

    // Removes a queued job, or asks a running one to stop at its next item.
    // False for a finished or unknown job.
    bool Cancel(uint64_t id);

    std::vector<Job> Jobs() const;

    // FOP-014 completion: the first active job using any of `paths`
    // (OperationQueue::ActiveJobTouching, under the lock), so the UI thread
    // can refuse a synchronous operation that would race a background job.
    std::optional<Job> ActiveJobTouching(const std::vector<std::filesystem::path>& paths) const;
    std::vector<PendingPrompt> PendingPrompts() const;
    bool AnswerConflict(uint64_t promptSeq, ConflictAskResult answer);
    bool AnswerError(uint64_t promptSeq, ErrorAskResult answer);

private:
    struct PromptSlot {
        PendingPrompt prompt;
        bool answered = false;
        ConflictAskResult conflict;
        ErrorAskResult error;
    };

    mutable std::mutex mutex_;
    std::condition_variable cv_;
    OperationQueue queue_;
    std::map<uint64_t, std::shared_ptr<std::atomic<bool>>> cancelFlags_;
    std::vector<HANDLE> threads_;  // Win32 threads: see JobWorker.cpp for why not std::thread
    std::vector<PromptSlot> prompts_;
    uint64_t nextPromptSeq_ = 1;
    bool shuttingDown_ = false;

    void StartRunnableLocked();
    static DWORD WINAPI ThreadEntry(LPVOID param);
    void RunJob(Job job);
    void PublishProgress(uint64_t id, int done, int total);
    ConflictAskResult AwaitConflict(uint64_t jobId, const std::filesystem::path& destination,
                                    const std::shared_ptr<std::atomic<bool>>& cancel);
    ErrorAskResult AwaitError(uint64_t jobId, const std::wstring& itemName, const std::wstring& message,
                              const std::shared_ptr<std::atomic<bool>>& cancel);
    PromptSlot* FindPromptLocked(uint64_t seq);
    void RemovePromptLocked(uint64_t seq);
};

} // namespace mc
