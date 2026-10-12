#include "DirSync.h"

#include "PathUtil.h"

#include <map>
#include <utility>

namespace fs = std::filesystem;

namespace mc {

namespace {

bool IsDir(const std::optional<CompareSide>& side) { return side && side->isDirectory; }

void Propose(SyncItem& e, SyncAction action, const wchar_t* reason = L"") {
    e.action = action;
    e.reason = reason;
}

void Conflict(SyncItem& e, const wchar_t* reason) {
    e.action = SyncAction::None;
    e.conflict = true;
    e.reason = reason;
}

}  // namespace

std::vector<SyncItem> BuildSyncPlan(const CompareOutcome& outcome, const SyncOptions& options) {
    const SyncDirection dir = options.direction;
    std::vector<SyncItem> plan;
    plan.reserve(outcome.items.size());
    for (const CompareItem& ci : outcome.items) {
        SyncItem e;
        e.item = ci;
        switch (ci.status) {
            case CompareStatus::Identical:
                break;
            case CompareStatus::Unreadable:
                e.reason = L"couldn't be read";
                break;
            case CompareStatus::TypeMismatch:
                Conflict(e, L"a file on one side, a folder on the other");
                break;
            case CompareStatus::LeftOnly:
                if (dir == SyncDirection::RightToLeft) {
                    if (options.deleteExtras) Propose(e, SyncAction::DeleteLeft, L"only on the target");
                    else e.reason = L"only on the target";
                } else {
                    Propose(e, SyncAction::CopyToRight);
                }
                break;
            case CompareStatus::RightOnly:
                if (dir == SyncDirection::LeftToRight) {
                    if (options.deleteExtras) Propose(e, SyncAction::DeleteRight, L"only on the target");
                    else e.reason = L"only on the target";
                } else {
                    Propose(e, SyncAction::CopyToLeft);
                }
                break;
            case CompareStatus::LeftNewer:
                if (dir == SyncDirection::RightToLeft) Conflict(e, L"target is newer");
                else Propose(e, SyncAction::CopyToRight, L"newer");
                break;
            case CompareStatus::RightNewer:
                if (dir == SyncDirection::LeftToRight) Conflict(e, L"target is newer");
                else Propose(e, SyncAction::CopyToLeft, L"newer");
                break;
            case CompareStatus::Different:
                if (IsDir(ci.left) && IsDir(ci.right)) {
                    e.reason = L"differences inside";  // its children carry the actions
                } else if (dir == SyncDirection::Both) {
                    Conflict(e, L"differs, neither is newer");
                } else {
                    Propose(e, dir == SyncDirection::LeftToRight ? SyncAction::CopyToRight : SyncAction::CopyToLeft,
                            L"differs");
                }
                break;
        }
        e.proposed = e.action;
        plan.push_back(std::move(e));
    }
    return plan;
}

bool CanApply(const SyncItem& entry, SyncAction action, const SyncOptions& options) {
    const auto& l = entry.item.left;
    const auto& r = entry.item.right;
    auto canCopy = [](const std::optional<CompareSide>& from, const std::optional<CompareSide>& to) {
        if (!from) return false;
        if (!to) return true;
        return from->isDirectory == to->isDirectory && !from->isDirectory;  // file over file only
    };
    switch (action) {
        case SyncAction::None:
            return true;
        case SyncAction::CopyToRight:
            return canCopy(l, r);
        case SyncAction::CopyToLeft:
            return canCopy(r, l);
        case SyncAction::DeleteRight:
            return options.direction == SyncDirection::LeftToRight && options.deleteExtras && r && !l;
        case SyncAction::DeleteLeft:
            return options.direction == SyncDirection::RightToLeft && options.deleteExtras && l && !r;
    }
    return false;
}

bool Overwrites(const SyncItem& entry) {
    return (entry.action == SyncAction::CopyToRight && entry.item.right.has_value()) ||
           (entry.action == SyncAction::CopyToLeft && entry.item.left.has_value());
}

SyncSummary Summarize(const std::vector<SyncItem>& plan) {
    SyncSummary s;
    for (const SyncItem& e : plan) {
        switch (e.action) {
            case SyncAction::CopyToRight:
                ++s.copyRight;
                if (!e.item.left->isDirectory) s.bytesRight += e.item.left->size;
                break;
            case SyncAction::CopyToLeft:
                ++s.copyLeft;
                if (!e.item.right->isDirectory) s.bytesLeft += e.item.right->size;
                break;
            case SyncAction::DeleteRight:
                ++s.deleteRight;
                break;
            case SyncAction::DeleteLeft:
                ++s.deleteLeft;
                break;
            case SyncAction::None:
                if (e.conflict) ++s.conflicts;
                break;
        }
        if (Overwrites(e)) ++s.overwrite;
    }
    return s;
}

std::vector<SyncGroup> GroupForExecution(const std::vector<SyncItem>& plan) {
    std::vector<SyncGroup> copies, deletes;
    // (action, folder) -> index into its vector; keeps first-seen order.
    std::map<std::pair<int, std::wstring>, size_t> copyIndex, deleteIndex;
    for (size_t i = 0; i < plan.size(); ++i) {
        SyncAction a = plan[i].action;
        if (a == SyncAction::None) continue;
        bool isDelete = a == SyncAction::DeleteRight || a == SyncAction::DeleteLeft;
        auto& groups = isDelete ? deletes : copies;
        auto& index = isDelete ? deleteIndex : copyIndex;
        fs::path relDir = fs::path(plan[i].item.relativePath).parent_path();
        auto key = std::make_pair(static_cast<int>(a), relDir.wstring());
        auto it = index.find(key);
        if (it == index.end()) {
            index.emplace(key, groups.size());
            groups.push_back(SyncGroup{a, relDir, {i}});
        } else {
            groups[it->second].planIndices.push_back(i);
        }
    }
    copies.insert(copies.end(), deletes.begin(), deletes.end());
    return copies;
}

std::optional<CompareSide> ReadSide(const fs::path& path) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ToLongPath(path).c_str(), GetFileExInfoStandard, &data)) return std::nullopt;
    CompareSide side;
    side.attributes = data.dwFileAttributes;
    side.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    side.isReparsePoint = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    side.size = side.isDirectory ? 0 : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    side.lastWriteUtc =
        (static_cast<uint64_t>(data.ftLastWriteTime.dwHighDateTime) << 32) | data.ftLastWriteTime.dwLowDateTime;
    return side;
}

bool SideChanged(const std::optional<CompareSide>& planned, const std::optional<CompareSide>& now) {
    if (planned.has_value() != now.has_value()) return true;
    if (!planned) return false;
    if (planned->isDirectory != now->isDirectory) return true;
    if (planned->isDirectory) return false;  // a folder's own time changes with its content
    return planned->size != now->size || planned->lastWriteUtc != now->lastWriteUtc;
}

}  // namespace mc
