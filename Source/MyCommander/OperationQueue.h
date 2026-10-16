#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mc {

// FOP-014, Phase 1: the pure job model for queued/background copy, move and
// delete. No threads, no Console, no Panel -- the state machine is tested
// before any worker exists (TST-005 precedent). Thread handoff is Phase 2.

enum class JobKind { Copy, Move, Delete };
// FOP-015: Paused is a Running job the user paused (P in the job list). It stays
// active: it keeps its volume and its paths, so nothing else starts on them.
enum class JobState { Queued, Running, WaitingForAnswer, Done, Cancelled, Failed, Paused };

struct Job {
    uint64_t id = 0;
    JobKind kind = JobKind::Copy;
    std::vector<std::filesystem::path> sources;
    std::filesystem::path destination;  // empty for Delete
    bool permanent = false;             // Delete only: bypass the Recycle Bin
    std::wstring volume;                // VolumeKey of the destination (or the first source for Delete)
    JobState state = JobState::Queued;
    int done = 0;
    int total = 0;
    std::vector<std::wstring> failures;  // one "item: reason" line per failed item, set when the job ends
};

// The filesystem root (drive or UNC share) a path lives on. Jobs on the same
// volume are serialized so two of them never write to or delete the same
// tree at once; jobs on different volumes may run in parallel (FOP-014 D1).
std::wstring VolumeKey(const std::filesystem::path& path);

// FOP-014 completion: which files a job uses, for the overlap safety rules.
// All comparisons are lexical (no filesystem access, so an offline share
// can't stall them), case-insensitive, '/' and '\' alike, a trailing
// separator ignored.

// Every path a job reads or writes: its sources, plus its destination for copy/move.
std::vector<std::filesystem::path> JobPaths(const Job& job);

// True if `a` and `b` are the same path or one is inside the other
// ("C:\a" and "C:\a\b" overlap; "C:\a" and "C:\ab" don't).
bool PathsOverlapLexically(const std::filesystem::path& a, const std::filesystem::path& b);

// True if any path of `a` overlaps any path of `b`.
bool JobsOverlap(const Job& a, const Job& b);

class OperationQueue {
public:
    // Assigns the next id, sets the state to Queued, and fills in `volume`
    // when the caller left it empty. Returns the id.
    uint64_t Enqueue(Job job);

    // Removes a job that has not started yet. Returns false for a running,
    // waiting or finished job, or an unknown id -- those are cancelled
    // through the worker's cancel flag instead (Phase 2).
    bool CancelQueued(uint64_t id);

    // The first Queued job that may start, marked Running; nothing when no
    // job is eligible. A job waits while its volume has a Running or
    // WaitingForAnswer job (D1), and -- wherever the volume -- while it
    // overlaps (JobsOverlap) an active job or an overlapping job queued
    // before it, so jobs on the same files keep their queue order (a delete
    // of C:\a queued after a copy of C:\a to D: never overtakes the copy).
    // Unrelated jobs behind it still start: no head-of-line blocking.
    std::optional<Job> NextToRun();

    // The first active (Running / WaitingForAnswer / Paused) job touching any of
    // `paths` (PathsOverlapLexically against JobPaths), or nothing. Used to
    // refuse a synchronous operation on files a background job is using.
    std::optional<Job> ActiveJobTouching(const std::vector<std::filesystem::path>& paths) const;

    // Sets a job's state (e.g. Running -> WaitingForAnswer). False for an
    // unknown id.
    bool SetState(uint64_t id, JobState state);

    // Records how many items of a job are done out of its total. Unknown ids are ignored.
    void SetProgress(uint64_t id, int done, int total);

    // Records the failed-item lines of a job. Unknown ids are ignored.
    void SetFailures(uint64_t id, std::vector<std::wstring> lines);

    // Records the final state of a job. Unknown ids are ignored.
    void Finish(uint64_t id, JobState finalState);

    const std::vector<Job>& Jobs() const { return jobs_; }
    size_t ActiveCount() const;  // Running + WaitingForAnswer + Paused
    size_t QueuedCount() const;

private:
    std::vector<Job> jobs_;
    uint64_t nextId_ = 1;
};

} // namespace mc
