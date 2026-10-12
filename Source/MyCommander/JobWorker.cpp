#include "JobWorker.h"

#include <algorithm>
#include <iterator>
#include <utility>

namespace mc {

namespace {

// Handed to a new worker thread; the thread takes ownership and frees it.
struct ThreadArgs {
    JobWorker* worker;
    Job job;
};

// FileOps works on one folder's items at a time, so a job's sources are
// grouped by their parent folder, keeping the order they were queued in.
std::vector<std::pair<std::filesystem::path, std::vector<std::wstring>>> GroupByFolder(
    const std::vector<std::filesystem::path>& sources) {
    std::vector<std::pair<std::filesystem::path, std::vector<std::wstring>>> groups;
    for (const auto& source : sources) {
        std::filesystem::path folder = source.parent_path();
        std::wstring name = source.filename().wstring();
        auto it = std::find_if(groups.begin(), groups.end(),
                               [&folder](const auto& group) { return group.first == folder; });
        if (it == groups.end()) {
            groups.push_back({folder, {name}});
        } else {
            it->second.push_back(name);
        }
    }
    return groups;
}

} // namespace

JobWorker::JobWorker() = default;

JobWorker::~JobWorker() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        shuttingDown_ = true;
        for (auto& entry : cancelFlags_) entry.second->store(true);
        cv_.notify_all();
    }
    // Joining outside the lock: a finishing worker takes the lock itself.
    // StartRunnableLocked does nothing once shuttingDown_ is set, so no new
    // thread can appear while we drain the list.
    for (;;) {
        HANDLE finished = nullptr;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (threads_.empty()) break;
            finished = threads_.back();
            threads_.pop_back();
        }
        WaitForSingleObject(finished, INFINITE);
        CloseHandle(finished);
    }
}

uint64_t JobWorker::Enqueue(Job job) {
    std::lock_guard<std::mutex> lock(mutex_);
    uint64_t id = queue_.Enqueue(std::move(job));
    cancelFlags_[id] = std::make_shared<std::atomic<bool>>(false);
    StartRunnableLocked();
    return id;
}

bool JobWorker::Cancel(uint64_t id) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (queue_.CancelQueued(id)) return true;
    for (const Job& job : queue_.Jobs()) {
        if (job.id != id) continue;
        if (job.state != JobState::Running && job.state != JobState::WaitingForAnswer) return false;
        cancelFlags_[id]->store(true);
        cv_.notify_all();
        return true;
    }
    return false;
}

std::vector<Job> JobWorker::Jobs() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.Jobs();
}

std::optional<Job> JobWorker::ActiveJobTouching(const std::vector<std::filesystem::path>& paths) const {
    std::lock_guard<std::mutex> lock(mutex_);
    return queue_.ActiveJobTouching(paths);
}

std::vector<PendingPrompt> JobWorker::PendingPrompts() const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::vector<PendingPrompt> result;
    for (const PromptSlot& slot : prompts_) {
        if (!slot.answered) result.push_back(slot.prompt);
    }
    return result;
}

bool JobWorker::AnswerConflict(uint64_t promptSeq, ConflictAskResult answer) {
    std::lock_guard<std::mutex> lock(mutex_);
    PromptSlot* slot = FindPromptLocked(promptSeq);
    if (!slot || slot->answered || slot->prompt.kind != PromptKind::Conflict) return false;
    slot->conflict = std::move(answer);
    slot->answered = true;
    cv_.notify_all();
    return true;
}

bool JobWorker::AnswerError(uint64_t promptSeq, ErrorAskResult answer) {
    std::lock_guard<std::mutex> lock(mutex_);
    PromptSlot* slot = FindPromptLocked(promptSeq);
    if (!slot || slot->answered || slot->prompt.kind != PromptKind::Error) return false;
    slot->error = answer;
    slot->answered = true;
    cv_.notify_all();
    return true;
}

void JobWorker::StartRunnableLocked() {
    if (shuttingDown_) return;
    while (auto next = queue_.NextToRun()) {
        const uint64_t id = next->id;
        auto* args = new ThreadArgs{this, std::move(*next)};
        HANDLE thread = CreateThread(nullptr, 0, &JobWorker::ThreadEntry, args, 0, nullptr);
        if (!thread) {
            delete args;
            queue_.Finish(id, JobState::Failed);
            continue;
        }
        threads_.push_back(thread);
    }
}

// std::thread is not used: on this project's include path `<process.h>`
// resolves to our own Process.h, which hides the C runtime's _beginthreadex,
// so the standard library's thread header does not compile in the test
// project. Win32 threads avoid that and match the rest of the console code.
DWORD WINAPI JobWorker::ThreadEntry(LPVOID param) {
    std::unique_ptr<ThreadArgs> args(static_cast<ThreadArgs*>(param));
    args->worker->RunJob(std::move(args->job));
    return 0;
}

void JobWorker::PublishProgress(uint64_t id, int done, int total) {
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.SetProgress(id, done, total);
}

void JobWorker::RunJob(Job job) {
    const uint64_t id = job.id;
    std::shared_ptr<std::atomic<bool>> cancel;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        cancel = cancelFlags_[id];
    }
    const int total = static_cast<int>(job.sources.size());

    ConflictHandler onConflict = [this, id, cancel](const std::filesystem::path& destination) {
        return AwaitConflict(id, destination, cancel);
    };
    ErrorHandler onError = [this, id, cancel](const std::wstring& itemName, const std::wstring& message, DWORD) {
        return AwaitError(id, itemName, message, cancel);
    };
    CancelPoll cancelPoll = [cancel]() { return cancel->load(); };

    bool cancelled = false;
    bool anyFailure = false;
    std::vector<std::wstring> failureLines;
    int doneSoFar = 0;
    for (const auto& group : GroupByFolder(job.sources)) {
        const std::filesystem::path& folder = group.first;
        const std::vector<std::wstring>& names = group.second;
        const int groupBase = doneSoFar;
        ItemStartHandler onItemStart = [this, id, cancel, groupBase, total](const std::wstring&, int doneCount, int) {
            PublishProgress(id, groupBase + doneCount, total);
            return !cancel->load();
        };

        OperationOutcome outcome;
        switch (job.kind) {
            case JobKind::Copy:
                outcome = CopyItems(folder, names, job.destination, onConflict, onItemStart, cancelPoll, onError);
                break;
            case JobKind::Move:
                outcome = MoveItems(folder, names, job.destination, onConflict, onItemStart, cancelPoll, onError);
                break;
            case JobKind::Delete:
                outcome = DeleteItems(folder, names, job.permanent, onItemStart, cancelPoll, onError);
                break;
        }
        if (outcome.cancelled) {
            cancelled = true;
            break;
        }
        if (!outcome.failed.empty()) anyFailure = true;
        std::transform(outcome.failed.begin(), outcome.failed.end(), std::back_inserter(failureLines),
                       [](const auto& failed) { return failed.first + L": " + failed.second; });
        doneSoFar += static_cast<int>(names.size());
        PublishProgress(id, doneSoFar, total);
    }

    JobState finalState = cancelled ? JobState::Cancelled : (anyFailure ? JobState::Failed : JobState::Done);
    std::lock_guard<std::mutex> lock(mutex_);
    queue_.SetFailures(id, std::move(failureLines));
    queue_.Finish(id, finalState);
    StartRunnableLocked();
    cv_.notify_all();
}

ConflictAskResult JobWorker::AwaitConflict(uint64_t jobId, const std::filesystem::path& destination,
                                           const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::unique_lock<std::mutex> lock(mutex_);
    uint64_t seq = nextPromptSeq_++;
    PromptSlot slot;
    slot.prompt.seq = seq;
    slot.prompt.jobId = jobId;
    slot.prompt.kind = PromptKind::Conflict;
    slot.prompt.path = destination;
    prompts_.push_back(std::move(slot));
    queue_.SetState(jobId, JobState::WaitingForAnswer);
    cv_.notify_all();

    cv_.wait(lock, [this, seq, &cancel] {
        PromptSlot* s = FindPromptLocked(seq);
        return shuttingDown_ || cancel->load() || (s && s->answered);
    });

    ConflictAskResult result{ConflictChoice::Cancel};
    PromptSlot* s = FindPromptLocked(seq);
    if (s && s->answered) result = s->conflict;
    RemovePromptLocked(seq);
    queue_.SetState(jobId, JobState::Running);
    return result;
}

ErrorAskResult JobWorker::AwaitError(uint64_t jobId, const std::wstring& itemName, const std::wstring& message,
                                     const std::shared_ptr<std::atomic<bool>>& cancel) {
    std::unique_lock<std::mutex> lock(mutex_);
    uint64_t seq = nextPromptSeq_++;
    PromptSlot slot;
    slot.prompt.seq = seq;
    slot.prompt.jobId = jobId;
    slot.prompt.kind = PromptKind::Error;
    slot.prompt.path = itemName;
    slot.prompt.text = message;
    prompts_.push_back(std::move(slot));
    queue_.SetState(jobId, JobState::WaitingForAnswer);
    cv_.notify_all();

    cv_.wait(lock, [this, seq, &cancel] {
        PromptSlot* s = FindPromptLocked(seq);
        return shuttingDown_ || cancel->load() || (s && s->answered);
    });

    ErrorAskResult result{ErrorChoice::Cancel};
    PromptSlot* s = FindPromptLocked(seq);
    if (s && s->answered) result = s->error;
    RemovePromptLocked(seq);
    queue_.SetState(jobId, JobState::Running);
    return result;
}

JobWorker::PromptSlot* JobWorker::FindPromptLocked(uint64_t seq) {
    auto it = std::find_if(prompts_.begin(), prompts_.end(),
                           [seq](const PromptSlot& slot) { return slot.prompt.seq == seq; });
    return it == prompts_.end() ? nullptr : &*it;
}

void JobWorker::RemovePromptLocked(uint64_t seq) {
    prompts_.erase(std::remove_if(prompts_.begin(), prompts_.end(),
                                  [seq](const PromptSlot& slot) { return slot.prompt.seq == seq; }),
                   prompts_.end());
}

} // namespace mc
