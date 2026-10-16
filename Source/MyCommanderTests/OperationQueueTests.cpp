#include "TestFramework.h"

#include "OperationQueue.h"

#include <string>

using mc::Job;
using mc::JobKind;
using mc::JobState;
using mc::OperationQueue;
using mc::VolumeKey;

namespace {

Job MakeCopy(const std::wstring& source, const std::wstring& destination) {
    Job job;
    job.kind = JobKind::Copy;
    job.sources = {source};
    job.destination = destination;
    return job;
}

} // namespace

// Paths on drives that don't need to exist: VolumeKey is pure string work.

TEST_CASE(OperationQueue_Enqueue_AssignsIncreasingIdsAndQueuedState_FOP014) {
    OperationQueue queue;
    uint64_t first = queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    uint64_t second = queue.Enqueue(MakeCopy(L"C:\\a\\y", L"C:\\b"));
    CHECK(second > first);
    CHECK(queue.Jobs().size() == 2);
    CHECK(queue.Jobs()[0].state == JobState::Queued);
    CHECK(queue.QueuedCount() == 2);
    CHECK(queue.ActiveCount() == 0);
}

TEST_CASE(OperationQueue_NextToRun_IsFifoAndMarksRunning_FOP014) {
    OperationQueue queue;
    uint64_t first = queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    queue.Enqueue(MakeCopy(L"C:\\a\\y", L"C:\\c"));

    auto job = queue.NextToRun();
    CHECK(job.has_value());
    if (job) {
        CHECK(job->id == first);
        CHECK(job->state == JobState::Running);
    }
    CHECK(queue.ActiveCount() == 1);
}

TEST_CASE(OperationQueue_NextToRun_SerializesJobsOnTheSameVolume_FOP014) {
    OperationQueue queue;
    queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    queue.Enqueue(MakeCopy(L"C:\\a\\y", L"C:\\c"));

    CHECK(queue.NextToRun().has_value());
    CHECK(!queue.NextToRun().has_value());  // the second C: job waits behind the first
}

TEST_CASE(OperationQueue_NextToRun_RunsDifferentVolumesInParallel_FOP014) {
    OperationQueue queue;
    queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    queue.Enqueue(MakeCopy(L"D:\\a\\y", L"D:\\b"));

    auto first = queue.NextToRun();
    auto second = queue.NextToRun();
    CHECK(first.has_value());
    CHECK(second.has_value());
    CHECK(queue.ActiveCount() == 2);
}

TEST_CASE(OperationQueue_NextToRun_SkipsBlockedVolumeToStartLaterJob_FOP014) {
    OperationQueue queue;
    queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    queue.Enqueue(MakeCopy(L"C:\\a\\y", L"C:\\c"));  // blocked behind the first on C:
    queue.Enqueue(MakeCopy(L"D:\\a\\z", L"D:\\b"));  // free volume, starts before the blocked C: job

    CHECK(queue.NextToRun().has_value());
    auto next = queue.NextToRun();
    CHECK(next.has_value());
    if (next) CHECK(next->volume == L"D:\\");
}

TEST_CASE(OperationQueue_WaitingForAnswer_KeepsVolumeBusy_FOP014) {
    OperationQueue queue;
    uint64_t first = queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    queue.Enqueue(MakeCopy(L"C:\\a\\y", L"C:\\c"));

    CHECK(queue.NextToRun().has_value());
    CHECK(queue.SetState(first, JobState::WaitingForAnswer));
    CHECK(!queue.NextToRun().has_value());  // a pending prompt blocks the volume's next job
    CHECK(queue.ActiveCount() == 1);
}

TEST_CASE(OperationQueue_CancelQueued_RemovesOnlyQueuedJobs_FOP014) {
    OperationQueue queue;
    uint64_t running = queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    uint64_t waiting = queue.Enqueue(MakeCopy(L"C:\\a\\y", L"C:\\c"));

    CHECK(queue.NextToRun().has_value());        // `running` starts
    CHECK(!queue.CancelQueued(running));         // a running job is not removed here
    CHECK(queue.CancelQueued(waiting));          // a queued job is removed
    CHECK(queue.Jobs().size() == 1);
    CHECK(!queue.CancelQueued(waiting));         // already gone
}

TEST_CASE(OperationQueue_Finish_RecordsFinalStateAndFreesVolume_FOP014) {
    OperationQueue queue;
    uint64_t first = queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    uint64_t second = queue.Enqueue(MakeCopy(L"C:\\a\\y", L"C:\\c"));

    CHECK(queue.NextToRun().has_value());
    queue.Finish(first, JobState::Done);
    CHECK(queue.Jobs()[0].state == JobState::Done);

    auto next = queue.NextToRun();
    CHECK(next.has_value());
    if (next) CHECK(next->id == second);
}

TEST_CASE(OperationQueue_UnknownIdsAreIgnored_FOP014) {
    OperationQueue queue;
    CHECK(!queue.SetState(999, JobState::Running));
    queue.Finish(999, JobState::Failed);  // must not crash or change anything
    CHECK(queue.Jobs().empty());
}

TEST_CASE(OperationQueue_EmptyQueueHasNothingToRun_FOP014) {
    OperationQueue queue;
    CHECK(!queue.NextToRun().has_value());
    CHECK(queue.ActiveCount() == 0);
    CHECK(queue.QueuedCount() == 0);
}

TEST_CASE(OperationQueue_DeleteUsesFirstSourceVolume_FOP014) {
    OperationQueue queue;
    Job job;
    job.kind = JobKind::Delete;
    job.sources = {L"E:\\trash\\x.txt"};
    uint64_t id = queue.Enqueue(job);
    CHECK(queue.Jobs()[0].id == id);
    CHECK(queue.Jobs()[0].volume == L"E:\\");
}

TEST_CASE(OperationQueue_FailureLinesSurviveFinish_FOP014) {
    OperationQueue queue;
    uint64_t id = queue.Enqueue(MakeCopy(L"C:\\a\\x", L"C:\\b"));
    queue.SetFailures(id, {L"x: access denied"});
    queue.Finish(id, JobState::Failed);
    CHECK(queue.Jobs()[0].state == JobState::Failed);
    CHECK(queue.Jobs()[0].failures.size() == 1);
}

// --- FOP-014 completion: overlap safety ----------------------------------------

namespace {
Job MakeDelete(const std::wstring& source) {
    Job job;
    job.kind = JobKind::Delete;
    job.sources = {source};
    return job;
}
}  // namespace

TEST_CASE(PathsOverlapLexically_SameInsideAndBoundaries_FOP014) {
    using mc::PathsOverlapLexically;
    CHECK(PathsOverlapLexically(L"C:\\a", L"c:/A/"));
    CHECK(PathsOverlapLexically(L"C:\\a", L"C:\\a\\b"));
    CHECK(PathsOverlapLexically(L"C:\\a\\b", L"C:\\a"));
    CHECK(!PathsOverlapLexically(L"C:\\a", L"C:\\ab"));  // a prefix that isn't a path boundary
    CHECK(!PathsOverlapLexically(L"C:\\a", L"D:\\a"));
    CHECK(PathsOverlapLexically(L"C:\\", L"C:\\anything\\deep"));  // a drive root contains everything on it
    CHECK(PathsOverlapLexically(L"\\\\srv\\share\\x", L"\\\\SRV\\share\\x\\y"));
    CHECK(!PathsOverlapLexically(L"\\\\srv\\share\\x", L"\\\\srv\\share\\xy"));
    CHECK(PathsOverlapLexically(L"C:\\a\\sub\\..", L"C:\\a\\file"));  // normalized lexically
}

TEST_CASE(JobPaths_CopyHasDestinationDeleteDoesNot_FOP014) {
    auto copy = mc::JobPaths(MakeCopy(L"C:\\a\\x", L"D:\\b"));
    CHECK(copy.size() == 2);
    auto del = mc::JobPaths(MakeDelete(L"C:\\a\\x"));
    CHECK(del.size() == 1);
}

TEST_CASE(JobsOverlap_SharedSourceOrDestinationTree_FOP014) {
    CHECK(mc::JobsOverlap(MakeCopy(L"C:\\a", L"D:\\x"), MakeDelete(L"C:\\a")));          // delete the copy's source
    CHECK(mc::JobsOverlap(MakeCopy(L"C:\\a", L"D:\\x"), MakeDelete(L"D:\\x\\old")));     // inside the copy's destination
    CHECK(!mc::JobsOverlap(MakeCopy(L"C:\\a", L"D:\\x"), MakeDelete(L"E:\\z")));         // unrelated
}

// The scenario the volume rule alone missed: the copy runs on D:, the
// delete of its source would run on C: -- in parallel.
TEST_CASE(OperationQueue_NextToRun_OverlappingJobWaitsAcrossVolumes_NoHeadOfLineBlocking_FOP014) {
    OperationQueue queue;
    uint64_t copy = queue.Enqueue(MakeCopy(L"C:\\a", L"D:\\x"));
    uint64_t del = queue.Enqueue(MakeDelete(L"C:\\a"));
    uint64_t other = queue.Enqueue(MakeDelete(L"E:\\z"));

    auto first = queue.NextToRun();
    CHECK(first.has_value() && first->id == copy);
    auto second = queue.NextToRun();
    CHECK(second.has_value() && second->id == other);  // the overlapping delete waits; the unrelated one starts
    CHECK(!queue.NextToRun().has_value());

    queue.Finish(copy, JobState::Done);
    auto third = queue.NextToRun();
    CHECK(third.has_value() && third->id == del);  // now the delete may run
}

// Queue order is kept among jobs on the same files even before the first one
// starts: the delete must not overtake a copy that is still waiting for D:.
TEST_CASE(OperationQueue_NextToRun_OverlappingJobNeverOvertakesEarlierQueuedOne_FOP014) {
    OperationQueue queue;
    uint64_t busyD = queue.Enqueue(MakeCopy(L"D:\\p", L"D:\\q"));  // occupies volume D:
    uint64_t copy = queue.Enqueue(MakeCopy(L"C:\\a", L"D:\\x"));   // waits for D:
    uint64_t del = queue.Enqueue(MakeDelete(L"C:\\a"));            // C: is free, but it overlaps `copy`

    auto first = queue.NextToRun();
    CHECK(first.has_value() && first->id == busyD);
    CHECK(!queue.NextToRun().has_value());  // copy waits for D:, delete waits behind copy

    queue.Finish(busyD, JobState::Done);
    auto second = queue.NextToRun();
    CHECK(second.has_value() && second->id == copy);
    CHECK(!queue.NextToRun().has_value());
    queue.Finish(copy, JobState::Done);
    auto third = queue.NextToRun();
    CHECK(third.has_value() && third->id == del);
}

TEST_CASE(OperationQueue_ActiveJobTouching_FindsOnlyActiveOverlappingJob_FOP014) {
    OperationQueue queue;
    uint64_t copy = queue.Enqueue(MakeCopy(L"C:\\a", L"D:\\x"));
    CHECK(!queue.ActiveJobTouching({L"C:\\a\\file.txt"}).has_value());  // still only queued
    CHECK(queue.NextToRun().has_value());
    auto touching = queue.ActiveJobTouching({L"C:\\a\\file.txt"});
    CHECK(touching.has_value() && touching->id == copy);
    CHECK(queue.ActiveJobTouching({L"D:\\x"}).has_value());
    CHECK(!queue.ActiveJobTouching({L"C:\\b", L"E:\\z"}).has_value());
    queue.Finish(copy, JobState::Done);
    CHECK(!queue.ActiveJobTouching({L"C:\\a"}).has_value());
}

TEST_CASE(VolumeKey_DriveAndUncShareRoots_FOP014) {
    CHECK(VolumeKey(L"C:\\Users\\bob\\file.txt") == L"C:\\");
    CHECK(VolumeKey(L"\\\\srv\\share\\dir\\file.txt") == L"\\\\srv\\share");
}

// --- FOP-015: a Paused job stays active -----------------------------------------

TEST_CASE(OperationQueue_PausedJobIsActiveAndKeepsItsVolumeAndPaths_FOP015) {
    OperationQueue queue;
    Job first;
    first.kind = JobKind::Copy;
    first.sources = {L"C:\\data\\a.txt"};
    first.destination = L"D:\\out";
    uint64_t firstId = queue.Enqueue(first);
    Job same;
    same.kind = JobKind::Delete;
    same.sources = {L"C:\\data\\a.txt"};
    uint64_t sameId = queue.Enqueue(same);
    (void)sameId;

    auto running = queue.NextToRun();
    CHECK(running.has_value() && running->id == firstId);
    CHECK(queue.SetState(firstId, JobState::Paused));
    CHECK(queue.ActiveCount() == 1);
    CHECK(!queue.NextToRun().has_value());  // the delete still overlaps the paused copy
    auto touching = queue.ActiveJobTouching({L"C:\\data\\a.txt"});
    CHECK(touching.has_value() && touching->state == JobState::Paused);

    queue.Finish(firstId, JobState::Done);
    CHECK(queue.NextToRun().has_value());   // released only when it ends
}
