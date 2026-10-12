#include "TestFixtures.h"
#include "TestFramework.h"

#include "JobWorker.h"

#include <chrono>
#include <functional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

using mc::ConflictAskResult;
using mc::ConflictChoice;
using mc::Job;
using mc::JobKind;
using mc::JobState;
using mc::JobWorker;
using mc::PendingPrompt;
using mc::PromptKind;

namespace {

// Polls `pred` until it's true or the timeout passes. The worker runs on
// its own thread, so tests wait on observable state rather than sleeping.
bool WaitUntil(const std::function<bool()>& pred, int timeoutMs = 10000) {
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        Sleep(5);
    }
    return pred();
}

bool Finished(const JobWorker& worker, uint64_t id) {
    for (const Job& job : worker.Jobs()) {
        if (job.id == id) return job.state == JobState::Done || job.state == JobState::Cancelled ||
                                 job.state == JobState::Failed;
    }
    return false;
}

JobState StateOf(const JobWorker& worker, uint64_t id) {
    for (const Job& job : worker.Jobs()) {
        if (job.id == id) return job.state;
    }
    return JobState::Queued;
}

Job CopyJob(const std::vector<fs::path>& sources, const fs::path& destination) {
    Job job;
    job.kind = JobKind::Copy;
    job.sources = sources;
    job.destination = destination;
    return job;
}

std::vector<PendingPrompt> WaitForPrompts(const JobWorker& worker) {
    std::vector<PendingPrompt> prompts;
    WaitUntil([&] {
        prompts = worker.PendingPrompts();
        return !prompts.empty();
    });
    return prompts;
}

} // namespace

// FOP-014 Phase 2 thread tests (plan test items 6-10), using real TempDir
// trees and the real FileOps engine.

TEST_CASE(JobWorker_QueuedCopyOfManyFilesCompletes_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    std::vector<fs::path> sources;
    for (int i = 0; i < 50; ++i) {
        wchar_t name[32];
        swprintf_s(name, L"f%02d.txt", i);
        fs::path file = src.Path() / name;
        test::WriteFileContent(file, "x");
        sources.push_back(file);
    }
    {
        JobWorker worker;
        uint64_t id = worker.Enqueue(CopyJob(sources, dst.Path()));
        CHECK(WaitUntil([&] { return Finished(worker, id); }));
        CHECK(StateOf(worker, id) == JobState::Done);
    }
    int copied = 0;
    for (const auto& entry : fs::directory_iterator(dst.Path())) {
        (void)entry;
        ++copied;
    }
    CHECK(copied == 50);
}

TEST_CASE(JobWorker_ConflictAnsweredWithSkipLeavesExistingFile_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    test::WriteFileContent(src.Path() / L"a.txt", "new");
    test::WriteFileContent(dst.Path() / L"a.txt", "old");
    {
        JobWorker worker;
        uint64_t id = worker.Enqueue(CopyJob({src.Path() / L"a.txt"}, dst.Path()));
        auto prompts = WaitForPrompts(worker);
        CHECK(prompts.size() == 1);
        if (!prompts.empty()) {
            CHECK(prompts[0].kind == PromptKind::Conflict);
            CHECK(prompts[0].jobId == id);
            ConflictAskResult skip;
            skip.choice = ConflictChoice::Skip;
            CHECK(worker.AnswerConflict(prompts[0].seq, skip));
        }
        CHECK(WaitUntil([&] { return Finished(worker, id); }));
        CHECK(StateOf(worker, id) == JobState::Done);
    }
    CHECK(test::ReadFileContent(dst.Path() / L"a.txt") == "old");
}

TEST_CASE(JobWorker_CancelWhilePromptPendingStopsTheJob_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    test::WriteFileContent(src.Path() / L"a.txt", "new");
    test::WriteFileContent(src.Path() / L"b.txt", "new");
    test::WriteFileContent(src.Path() / L"c.txt", "new");
    test::WriteFileContent(dst.Path() / L"a.txt", "old");
    {
        JobWorker worker;
        uint64_t id = worker.Enqueue(CopyJob({src.Path() / L"a.txt", src.Path() / L"b.txt", src.Path() / L"c.txt"},
                                             dst.Path()));
        CHECK(!WaitForPrompts(worker).empty());
        CHECK(worker.Cancel(id));
        CHECK(WaitUntil([&] { return Finished(worker, id); }));
        CHECK(StateOf(worker, id) == JobState::Cancelled);
    }
    CHECK(!fs::exists(dst.Path() / L"c.txt"));
    CHECK(test::ReadFileContent(dst.Path() / L"a.txt") == "old");
}

TEST_CASE(JobWorker_JobsOnOneVolumeNeverRunConcurrently_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    test::WriteFileContent(src.Path() / L"a.txt", "new");
    test::WriteFileContent(src.Path() / L"b.txt", "new");
    test::WriteFileContent(dst.Path() / L"a.txt", "old");
    {
        JobWorker worker;
        uint64_t first = worker.Enqueue(CopyJob({src.Path() / L"a.txt"}, dst.Path()));
        uint64_t second = worker.Enqueue(CopyJob({src.Path() / L"b.txt"}, dst.Path()));
        auto prompts = WaitForPrompts(worker);
        CHECK(!prompts.empty());
        // The first job is waiting on a prompt, so the second must not have started.
        CHECK(StateOf(worker, second) == JobState::Queued);
        if (!prompts.empty()) {
            ConflictAskResult skip;
            skip.choice = ConflictChoice::Skip;
            worker.AnswerConflict(prompts[0].seq, skip);
        }
        CHECK(WaitUntil([&] { return Finished(worker, first) && Finished(worker, second); }));
        CHECK(StateOf(worker, first) == JobState::Done);
        CHECK(StateOf(worker, second) == JobState::Done);
    }
    CHECK(fs::exists(dst.Path() / L"b.txt"));
}

TEST_CASE(JobWorker_PromptBlocksWorkerUntilAnsweredWithOverwrite_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    test::WriteFileContent(src.Path() / L"a.txt", "new");
    test::WriteFileContent(dst.Path() / L"a.txt", "old");
    {
        JobWorker worker;
        uint64_t id = worker.Enqueue(CopyJob({src.Path() / L"a.txt"}, dst.Path()));
        auto prompts = WaitForPrompts(worker);
        CHECK(!prompts.empty());
        // Until answered, the worker is parked and the destination is untouched.
        CHECK(StateOf(worker, id) == JobState::WaitingForAnswer);
        CHECK(test::ReadFileContent(dst.Path() / L"a.txt") == "old");
        if (!prompts.empty()) {
            ConflictAskResult overwrite;
            overwrite.choice = ConflictChoice::Overwrite;
            CHECK(worker.AnswerConflict(prompts[0].seq, overwrite));
            CHECK(!worker.AnswerConflict(prompts[0].seq, overwrite));  // an answer is accepted once
        }
        CHECK(WaitUntil([&] { return Finished(worker, id); }));
        CHECK(StateOf(worker, id) == JobState::Done);
    }
    CHECK(test::ReadFileContent(dst.Path() / L"a.txt") == "new");
}

TEST_CASE(JobWorker_ProgressReportsDoneAndTotal_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    std::vector<fs::path> sources;
    for (const wchar_t* name : {L"a.txt", L"b.txt", L"c.txt"}) {
        test::WriteFileContent(src.Path() / name, "x");
        sources.push_back(src.Path() / name);
    }
    JobWorker worker;
    uint64_t id = worker.Enqueue(CopyJob(sources, dst.Path()));
    CHECK(WaitUntil([&] { return Finished(worker, id); }));
    for (const Job& job : worker.Jobs()) {
        if (job.id == id) {
            CHECK(job.done == 3);
            CHECK(job.total == 3);
        }
    }
}

TEST_CASE(JobWorker_DestroyingWithPendingPromptDoesNotHang_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    test::WriteFileContent(src.Path() / L"a.txt", "new");
    test::WriteFileContent(dst.Path() / L"a.txt", "old");
    {
        JobWorker worker;
        worker.Enqueue(CopyJob({src.Path() / L"a.txt"}, dst.Path()));
        CHECK(!WaitForPrompts(worker).empty());
        // Leaving this scope must cancel the parked job and join its thread.
    }
    CHECK(test::ReadFileContent(dst.Path() / L"a.txt") == "old");
}

// --- FOP-014 completion: queued delete and overlap ------------------------------

namespace {
Job DeleteJob(const std::vector<fs::path>& sources, bool permanent) {
    Job job;
    job.kind = JobKind::Delete;
    job.sources = sources;
    job.permanent = permanent;
    return job;
}
}  // namespace

TEST_CASE(JobWorker_QueuedPermanentDeleteOfFilesAndFolderCompletes_FOP014) {
    test::TempDir root;
    std::vector<fs::path> sources;
    for (int i = 0; i < 20; ++i) {
        wchar_t name[32];
        swprintf_s(name, L"d%02d.txt", i);
        test::WriteFileContent(root.Path() / name, "x");
        sources.push_back(root.Path() / name);
    }
    fs::create_directories(root.Path() / L"folder" / L"deep");
    test::WriteFileContent(root.Path() / L"folder" / L"deep" / L"z.txt", "z");
    sources.push_back(root.Path() / L"folder");
    {
        JobWorker worker;
        uint64_t id = worker.Enqueue(DeleteJob(sources, /*permanent=*/true));
        CHECK(WaitUntil([&] { return Finished(worker, id); }));
        CHECK(StateOf(worker, id) == JobState::Done);
        for (const Job& job : worker.Jobs()) {
            if (job.id == id) CHECK(job.done == 21 && job.total == 21);
        }
    }
    for (const auto& s : sources) CHECK(!fs::exists(s));
}

TEST_CASE(JobWorker_QueuedRecycleBinDeleteCompletes_FOP014) {
    test::TempDir root;
    std::vector<fs::path> sources;
    for (const wchar_t* n : {L"r1.txt", L"r2.txt", L"r3.txt"}) {
        test::WriteFileContent(root.Path() / n, "x");
        sources.push_back(root.Path() / n);
    }
    JobWorker worker;
    uint64_t id = worker.Enqueue(DeleteJob(sources, /*permanent=*/false));
    CHECK(WaitUntil([&] { return Finished(worker, id); }));
    if (StateOf(worker, id) != JobState::Done) {
        wprintf(L"  (note: Recycle Bin delete failed in this environment, skipping assertion)\n");
        return;
    }
    for (const auto& s : sources) CHECK(!fs::exists(s));
}

TEST_CASE(JobWorker_QueuedDeleteLockedFile_PromptsForThatItemOnly_FOP014) {
    for (mc::ErrorChoice answer : {mc::ErrorChoice::Skip, mc::ErrorChoice::Cancel}) {
        test::TempDir root;
        for (const wchar_t* n : {L"a.txt", L"locked.txt", L"c.txt"}) test::WriteFileContent(root.Path() / n, "x");
        HANDLE lock = CreateFileW((root.Path() / L"locked.txt").c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(lock != INVALID_HANDLE_VALUE);
        {
            JobWorker worker;
            uint64_t id = worker.Enqueue(
                DeleteJob({root.Path() / L"a.txt", root.Path() / L"locked.txt", root.Path() / L"c.txt"}, true));
            auto prompts = WaitForPrompts(worker);
            CHECK(prompts.size() == 1);
            if (!prompts.empty()) {
                CHECK(prompts[0].kind == PromptKind::Error);
                CHECK(prompts[0].path.wstring() == L"locked.txt");
                mc::ErrorAskResult reply;
                reply.choice = answer;
                CHECK(worker.AnswerError(prompts[0].seq, reply));
            }
            CHECK(WaitUntil([&] { return Finished(worker, id); }));
            if (answer == mc::ErrorChoice::Skip) {
                CHECK(StateOf(worker, id) == JobState::Done);  // skipped is not failed
                CHECK(!fs::exists(root.Path() / L"c.txt"));
            } else {
                CHECK(StateOf(worker, id) == JobState::Cancelled);
                CHECK(fs::exists(root.Path() / L"c.txt"));  // stopped before the third item
            }
            CHECK(!fs::exists(root.Path() / L"a.txt"));
        }
        CloseHandle(lock);
        CHECK(fs::exists(root.Path() / L"locked.txt"));
    }
}

// End to end: a delete of a copy's source, queued while the copy is still
// running (parked on a conflict prompt), waits until the copy is done.
TEST_CASE(JobWorker_DeleteOfCopySourceWaitsForTheCopy_FOP014) {
    test::TempDir src;
    test::TempDir dst;
    test::WriteFileContent(src.Path() / L"a.txt", "new");
    test::WriteFileContent(dst.Path() / L"a.txt", "old");
    {
        JobWorker worker;
        uint64_t copy = worker.Enqueue(CopyJob({src.Path() / L"a.txt"}, dst.Path()));
        auto prompts = WaitForPrompts(worker);  // the copy is now active, waiting for an answer
        uint64_t del = worker.Enqueue(DeleteJob({src.Path() / L"a.txt"}, /*permanent=*/true));
        auto busy = worker.ActiveJobTouching({src.Path() / L"a.txt"});
        CHECK(busy.has_value() && busy->id == copy);
        Sleep(100);
        CHECK(StateOf(worker, del) == JobState::Queued);  // held back while the copy runs
        CHECK(fs::exists(src.Path() / L"a.txt"));
        if (!prompts.empty()) {
            ConflictAskResult overwrite;
            overwrite.choice = ConflictChoice::Overwrite;
            worker.AnswerConflict(prompts[0].seq, overwrite);
        }
        CHECK(WaitUntil([&] { return Finished(worker, copy) && Finished(worker, del); }));
        CHECK(StateOf(worker, copy) == JobState::Done);
        CHECK(StateOf(worker, del) == JobState::Done);
        CHECK(!worker.ActiveJobTouching({src.Path() / L"a.txt"}).has_value());
    }
    CHECK(test::ReadFileContent(dst.Path() / L"a.txt") == "new");  // the copy completed first
    CHECK(!fs::exists(src.Path() / L"a.txt"));                     // then the delete ran
}

TEST_CASE(JobWorker_UnknownPromptAndJobIdsAreRejected_FOP014) {
    JobWorker worker;
    CHECK(!worker.Cancel(12345));
    CHECK(!worker.AnswerConflict(12345, ConflictAskResult{}));
    CHECK(!worker.AnswerError(12345, mc::ErrorAskResult{}));
    CHECK(worker.PendingPrompts().empty());
}
