#include "TestFixtures.h"
#include "TestFramework.h"

#include "DirSync.h"

#include <algorithm>

namespace fs = std::filesystem;

using mc::BuildSyncPlan;
using mc::CanApply;
using mc::CompareItem;
using mc::CompareOutcome;
using mc::CompareSide;
using mc::CompareStatus;
using mc::SyncAction;
using mc::SyncDirection;
using mc::SyncItem;
using mc::SyncOptions;

namespace {

constexpr uint64_t kBase = 133'000'000'000'000'000ULL;

CompareSide File(uint64_t size) {
    CompareSide s;
    s.size = size;
    s.lastWriteUtc = kBase;
    return s;
}

CompareSide Folder() {
    CompareSide s;
    s.isDirectory = true;
    return s;
}

CompareItem Item(const std::wstring& rel, std::optional<CompareSide> l, std::optional<CompareSide> r,
                 CompareStatus status) {
    CompareItem i;
    i.relativePath = rel;
    i.name = fs::path(rel).filename().wstring();
    i.depth = static_cast<int>(std::count(rel.begin(), rel.end(), L'\\'));
    i.left = l;
    i.right = r;
    i.status = status;
    return i;
}

// One item of every status, all at the top level.
CompareOutcome EveryStatus() {
    CompareOutcome o;
    o.items = {
        Item(L"same", File(1), File(1), CompareStatus::Identical),
        Item(L"lonly", File(10), std::nullopt, CompareStatus::LeftOnly),
        Item(L"ronly", std::nullopt, File(20), CompareStatus::RightOnly),
        Item(L"lnewer", File(30), File(31), CompareStatus::LeftNewer),
        Item(L"rnewer", File(40), File(41), CompareStatus::RightNewer),
        Item(L"diff", File(50), File(51), CompareStatus::Different),
        Item(L"mixed", Folder(), File(1), CompareStatus::TypeMismatch),
        Item(L"locked", File(5), File(5), CompareStatus::Unreadable),
    };
    return o;
}

const SyncItem& Get(const std::vector<SyncItem>& plan, const std::wstring& rel) {
    return *std::find_if(plan.begin(), plan.end(), [&](const SyncItem& e) { return e.item.relativePath == rel; });
}

SyncOptions Options(SyncDirection d, bool deleteExtras = false) {
    SyncOptions o;
    o.direction = d;
    o.deleteExtras = deleteExtras;
    return o;
}

}  // namespace

TEST_CASE(DirSync_Plan_LeftToRight_RuleTable_SRC007) {
    auto plan = BuildSyncPlan(EveryStatus(), Options(SyncDirection::LeftToRight));
    CHECK(Get(plan, L"same").action == SyncAction::None);
    CHECK(Get(plan, L"lonly").action == SyncAction::CopyToRight);
    CHECK(Get(plan, L"ronly").action == SyncAction::None);  // delete extras off
    CHECK(!Get(plan, L"ronly").conflict);
    CHECK(Get(plan, L"lnewer").action == SyncAction::CopyToRight);
    CHECK(Get(plan, L"rnewer").action == SyncAction::None && Get(plan, L"rnewer").conflict);
    CHECK(Get(plan, L"diff").action == SyncAction::CopyToRight);  // mirror semantics
    CHECK(Get(plan, L"mixed").action == SyncAction::None && Get(plan, L"mixed").conflict);
    CHECK(Get(plan, L"locked").action == SyncAction::None && !Get(plan, L"locked").conflict);
    for (const auto& e : plan) CHECK(e.proposed == e.action);
}

TEST_CASE(DirSync_Plan_RightToLeft_IsTheMirrorImage_SRC007) {
    auto plan = BuildSyncPlan(EveryStatus(), Options(SyncDirection::RightToLeft));
    CHECK(Get(plan, L"lonly").action == SyncAction::None);
    CHECK(Get(plan, L"ronly").action == SyncAction::CopyToLeft);
    CHECK(Get(plan, L"lnewer").conflict && Get(plan, L"lnewer").action == SyncAction::None);
    CHECK(Get(plan, L"rnewer").action == SyncAction::CopyToLeft);
    CHECK(Get(plan, L"diff").action == SyncAction::CopyToLeft);
}

TEST_CASE(DirSync_Plan_BothWays_SRC007) {
    auto plan = BuildSyncPlan(EveryStatus(), Options(SyncDirection::Both, /*deleteExtras=*/true));
    CHECK(Get(plan, L"lonly").action == SyncAction::CopyToRight);
    CHECK(Get(plan, L"ronly").action == SyncAction::CopyToLeft);  // never a delete both ways
    CHECK(Get(plan, L"lnewer").action == SyncAction::CopyToRight);
    CHECK(Get(plan, L"rnewer").action == SyncAction::CopyToLeft);
    CHECK(Get(plan, L"diff").conflict && Get(plan, L"diff").action == SyncAction::None);
}

TEST_CASE(DirSync_Plan_DeleteExtrasOneWayOnly_SRC007) {
    auto plan = BuildSyncPlan(EveryStatus(), Options(SyncDirection::LeftToRight, true));
    CHECK(Get(plan, L"ronly").action == SyncAction::DeleteRight);
    plan = BuildSyncPlan(EveryStatus(), Options(SyncDirection::RightToLeft, true));
    CHECK(Get(plan, L"lonly").action == SyncAction::DeleteLeft);
}

TEST_CASE(DirSync_Plan_Folders_SRC007) {
    CompareOutcome o;
    o.items = {
        Item(L"both", Folder(), Folder(), CompareStatus::Different),
        Item(L"both\\f.txt", File(2), File(1), CompareStatus::Different),
        Item(L"newdir", Folder(), std::nullopt, CompareStatus::LeftOnly),
    };
    auto plan = BuildSyncPlan(o, Options(SyncDirection::LeftToRight));
    CHECK(Get(plan, L"both").action == SyncAction::None && !Get(plan, L"both").conflict);
    CHECK(Get(plan, L"both\\f.txt").action == SyncAction::CopyToRight);
    CHECK(Get(plan, L"newdir").action == SyncAction::CopyToRight);  // whole folder, one item
    CHECK(!CanApply(Get(plan, L"both"), SyncAction::CopyToRight, Options(SyncDirection::LeftToRight)));
}

TEST_CASE(DirSync_CanApply_SRC007) {
    auto oneWay = Options(SyncDirection::LeftToRight, true);
    auto plan = BuildSyncPlan(EveryStatus(), oneWay);
    CHECK(CanApply(Get(plan, L"rnewer"), SyncAction::CopyToRight, oneWay));  // resolve a conflict
    CHECK(CanApply(Get(plan, L"rnewer"), SyncAction::CopyToLeft, oneWay));
    CHECK(!CanApply(Get(plan, L"lonly"), SyncAction::CopyToLeft, oneWay));   // right lacks it
    CHECK(!CanApply(Get(plan, L"mixed"), SyncAction::CopyToRight, oneWay));  // file onto folder
    CHECK(!CanApply(Get(plan, L"mixed"), SyncAction::CopyToLeft, oneWay));
    CHECK(CanApply(Get(plan, L"ronly"), SyncAction::DeleteRight, oneWay));
    CHECK(!CanApply(Get(plan, L"lnewer"), SyncAction::DeleteRight, oneWay));  // exists on both sides
    CHECK(!CanApply(Get(plan, L"ronly"), SyncAction::DeleteRight, Options(SyncDirection::LeftToRight, false)));
    CHECK(!CanApply(Get(plan, L"ronly"), SyncAction::DeleteRight, Options(SyncDirection::Both, true)));
    CHECK(!CanApply(Get(plan, L"lonly"), SyncAction::DeleteLeft, oneWay));
    CHECK(CanApply(Get(plan, L"same"), SyncAction::None, oneWay));
}

TEST_CASE(DirSync_Summarize_SRC007) {
    auto plan = BuildSyncPlan(EveryStatus(), Options(SyncDirection::LeftToRight, true));
    auto s = mc::Summarize(plan);
    CHECK(s.copyRight == 3);  // lonly, lnewer, diff
    CHECK(s.overwrite == 2);  // lnewer, diff
    CHECK(s.bytesRight == 10 + 30 + 50);
    CHECK(s.deleteRight == 1);
    CHECK(s.copyLeft == 0 && s.deleteLeft == 0);
    CHECK(s.conflicts == 2);  // rnewer, mixed
    CHECK(!s.Empty());

    CompareOutcome same;
    same.items = {Item(L"a", File(1), File(1), CompareStatus::Identical)};
    CHECK(mc::Summarize(BuildSyncPlan(same, Options(SyncDirection::Both))).Empty());
}

TEST_CASE(DirSync_GroupForExecution_CopiesFirstDeletesLast_SRC007) {
    CompareOutcome o;
    o.items = {
        Item(L"gone", std::nullopt, File(1), CompareStatus::RightOnly),
        Item(L"a.txt", File(1), std::nullopt, CompareStatus::LeftOnly),
        Item(L"sub", Folder(), Folder(), CompareStatus::Different),
        Item(L"sub\\b.txt", File(1), std::nullopt, CompareStatus::LeftOnly),
        Item(L"sub\\old.txt", std::nullopt, File(1), CompareStatus::RightOnly),
        Item(L"c.txt", File(2), File(1), CompareStatus::LeftNewer),
    };
    auto plan = BuildSyncPlan(o, Options(SyncDirection::LeftToRight, true));
    auto groups = mc::GroupForExecution(plan);
    CHECK(groups.size() == 4);
    CHECK(groups[0].action == SyncAction::CopyToRight && groups[0].relativeDir.empty());
    CHECK(groups[0].planIndices.size() == 2);  // a.txt, c.txt
    CHECK(groups[1].action == SyncAction::CopyToRight && groups[1].relativeDir == L"sub");
    CHECK(groups[2].action == SyncAction::DeleteRight && groups[2].relativeDir.empty());
    CHECK(groups[3].action == SyncAction::DeleteRight && groups[3].relativeDir == L"sub");
}

TEST_CASE(DirSync_Invariant_EveryCopyTargetFolderExists_SRC007) {
    // A real compare: a copy's destination folder is always a both-sides folder or a root.
    test::TempDir l, r;
    fs::create_directories(l.Path() / L"a" / L"b");
    fs::create_directories(r.Path() / L"a");
    test::WriteFileContent(l.Path() / L"a" / L"b" / L"deep.txt", "x");
    test::WriteFileContent(l.Path() / L"a" / L"top.txt", "x");
    mc::CompareOptions co;
    co.recursive = true;
    auto outcome = mc::CompareDirectories(l.Path(), r.Path(), co);
    auto plan = BuildSyncPlan(outcome, Options(SyncDirection::LeftToRight));
    for (const auto& g : mc::GroupForExecution(plan)) {
        std::error_code ec;
        CHECK(fs::is_directory(r.Path() / g.relativeDir, ec));
    }
    CHECK(mc::Summarize(plan).copyRight == 2);  // a\b (whole folder) and a\top.txt
}

TEST_CASE(DirSync_StaleCheck_SRC007) {
    test::TempDir d;
    fs::path f = d.Path() / L"f.txt";
    test::WriteFileContent(f, "abc");
    auto planned = mc::ReadSide(f);
    CHECK(planned.has_value() && planned->size == 3 && !planned->isDirectory);
    CHECK(!mc::SideChanged(planned, mc::ReadSide(f)));

    test::WriteFileContent(f, "abcd");
    CHECK(mc::SideChanged(planned, mc::ReadSide(f)));

    CHECK(!mc::ReadSide(d.Path() / L"missing").has_value());
    CHECK(!mc::SideChanged(std::nullopt, mc::ReadSide(d.Path() / L"missing")));
    CHECK(mc::SideChanged(std::nullopt, mc::ReadSide(f)));  // appeared since the preview
    CHECK(mc::SideChanged(planned, std::nullopt));          // vanished

    fs::create_directory(d.Path() / L"dir");
    auto dir = mc::ReadSide(d.Path() / L"dir");
    CHECK(dir && dir->isDirectory);
    test::WriteFileContent(d.Path() / L"dir" / L"x", "1");
    CHECK(!mc::SideChanged(dir, mc::ReadSide(d.Path() / L"dir")));  // folder time changes don't count
}
