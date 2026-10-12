#include "OperationQueue.h"

#include "DirectoryTree.h"

#include <algorithm>
#include <cwctype>

namespace mc {

std::wstring VolumeKey(const std::filesystem::path& path) { return FindFilesystemRoot(path).wstring(); }

namespace {

// Lexical comparison key: normalized, '\' separators, lower case, no
// trailing separator except the one a drive root ("c:\") keeps.
std::wstring OverlapKey(const std::filesystem::path& p) {
    std::wstring s = p.lexically_normal().wstring();
    std::replace(s.begin(), s.end(), L'/', L'\\');
    while (s.size() > 3 && s.back() == L'\\') s.pop_back();
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return static_cast<wchar_t>(std::towlower(c)); });
    return s;
}

bool IsSameOrInside(const std::wstring& base, const std::wstring& p) {
    if (base.empty() || p.size() < base.size() || p.compare(0, base.size(), base) != 0) return false;
    return p.size() == base.size() || base.back() == L'\\' || p[base.size()] == L'\\';
}

bool IsActive(JobState state) { return state == JobState::Running || state == JobState::WaitingForAnswer; }

}  // namespace

std::vector<std::filesystem::path> JobPaths(const Job& job) {
    std::vector<std::filesystem::path> paths = job.sources;
    if (job.kind != JobKind::Delete && !job.destination.empty()) paths.push_back(job.destination);
    return paths;
}

bool PathsOverlapLexically(const std::filesystem::path& a, const std::filesystem::path& b) {
    std::wstring ka = OverlapKey(a), kb = OverlapKey(b);
    return IsSameOrInside(ka, kb) || IsSameOrInside(kb, ka);
}

bool JobsOverlap(const Job& a, const Job& b) {
    const auto pathsA = JobPaths(a), pathsB = JobPaths(b);
    return std::any_of(pathsA.begin(), pathsA.end(), [&](const auto& pa) {
        return std::any_of(pathsB.begin(), pathsB.end(), [&](const auto& pb) { return PathsOverlapLexically(pa, pb); });
    });
}

std::optional<Job> OperationQueue::ActiveJobTouching(const std::vector<std::filesystem::path>& paths) const {
    for (const Job& job : jobs_) {
        if (!IsActive(job.state)) continue;
        const auto jobPaths = JobPaths(job);
        if (std::any_of(jobPaths.begin(), jobPaths.end(), [&](const auto& jp) {
                return std::any_of(paths.begin(), paths.end(), [&](const auto& p) { return PathsOverlapLexically(jp, p); });
            }))
            return job;
    }
    return std::nullopt;
}

uint64_t OperationQueue::Enqueue(Job job) {
    job.id = nextId_++;
    job.state = JobState::Queued;
    if (job.volume.empty()) {
        // Copy/move: the destination's volume. Delete (no destination): the first source's.
        std::filesystem::path where = job.destination;
        if (where.empty() && !job.sources.empty()) where = job.sources.front();
        job.volume = VolumeKey(where);
    }
    uint64_t id = job.id;
    jobs_.push_back(std::move(job));
    return id;
}

bool OperationQueue::CancelQueued(uint64_t id) {
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& j) { return j.id == id; });
    if (it == jobs_.end() || it->state != JobState::Queued) return false;
    jobs_.erase(it);
    return true;
}

std::optional<Job> OperationQueue::NextToRun() {
    for (size_t i = 0; i < jobs_.size(); ++i) {
        Job& candidate = jobs_[i];
        if (candidate.state != JobState::Queued) continue;
        bool blocked = false;
        for (size_t j = 0; j < jobs_.size() && !blocked; ++j) {
            if (j == i) continue;
            const Job& other = jobs_[j];
            bool active = IsActive(other.state);
            bool queuedEarlier = other.state == JobState::Queued && j < i;
            if (active && other.volume == candidate.volume) blocked = true;  // D1: one job per volume
            else if ((active || queuedEarlier) && JobsOverlap(candidate, other)) blocked = true;  // same files
        }
        if (blocked) continue;
        candidate.state = JobState::Running;
        return candidate;
    }
    return std::nullopt;
}

bool OperationQueue::SetState(uint64_t id, JobState state) {
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& j) { return j.id == id; });
    if (it == jobs_.end()) return false;
    it->state = state;
    return true;
}

void OperationQueue::SetProgress(uint64_t id, int done, int total) {
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& j) { return j.id == id; });
    if (it == jobs_.end()) return;
    it->done = done;
    it->total = total;
}

void OperationQueue::SetFailures(uint64_t id, std::vector<std::wstring> lines) {
    auto it = std::find_if(jobs_.begin(), jobs_.end(), [id](const Job& j) { return j.id == id; });
    if (it == jobs_.end()) return;
    it->failures = std::move(lines);
}

void OperationQueue::Finish(uint64_t id, JobState finalState) { SetState(id, finalState); }

// Public API, called by OperationQueueTests.cpp (not part of the scan).
// cppcheck-suppress unusedFunction
size_t OperationQueue::ActiveCount() const {
    return static_cast<size_t>(std::count_if(jobs_.begin(), jobs_.end(), [](const Job& j) {
        return j.state == JobState::Running || j.state == JobState::WaitingForAnswer;
    }));
}

// Public API, called by OperationQueueTests.cpp (not part of the scan).
// cppcheck-suppress unusedFunction
size_t OperationQueue::QueuedCount() const {
    return static_cast<size_t>(
        std::count_if(jobs_.begin(), jobs_.end(), [](const Job& j) { return j.state == JobState::Queued; }));
}

} // namespace mc
