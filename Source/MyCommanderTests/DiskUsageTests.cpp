#include "TestFramework.h"
#include "TestFixtures.h"

#include "DiskUsage.h"

using mc::DiskUsageResult;
using mc::SumTree;

TEST_CASE(DiskUsage_SingleFile_CountsItsOwnBytes_FOP020) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "0123456789");  // 10 bytes
    DiskUsageResult result = SumTree(dir.Path() / L"a.txt");
    CHECK(result.bytes == 10);
    CHECK(result.files == 1);
    CHECK(result.folders == 0);
    CHECK(result.allocated >= 10);  // cluster-rounded, never less than the logical size
}

TEST_CASE(DiskUsage_FlatDirectory_SumsEveryFileAndCountsNoSubfolders_FOP020) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "01234");       // 5
    test::WriteFileContent(dir.Path() / L"b.txt", "0123456789");  // 10
    DiskUsageResult result = SumTree(dir.Path());
    CHECK(result.bytes == 15);
    CHECK(result.files == 2);
    CHECK(result.folders == 0);
    CHECK(result.unreadableFolders == 0);
}

TEST_CASE(DiskUsage_NestedDirectory_RecursesAndCountsSubfolders_FOP020) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.Path() / L"sub" / L"deeper");
    test::WriteFileContent(dir.Path() / L"top.txt", "abc");               // 3
    test::WriteFileContent(dir.Path() / L"sub" / L"mid.txt", "abcde");    // 5
    test::WriteFileContent(dir.Path() / L"sub" / L"deeper" / L"d.txt", "a");  // 1
    DiskUsageResult result = SumTree(dir.Path());
    CHECK(result.bytes == 9);
    CHECK(result.files == 3);
    CHECK(result.folders == 2);  // "sub" and "sub\\deeper"
}

TEST_CASE(DiskUsage_EmptyDirectory_IsZeroWithNoFilesOrFolders_FOP020) {
    test::TempDir dir;
    DiskUsageResult result = SumTree(dir.Path());
    CHECK(result.bytes == 0);
    CHECK(result.files == 0);
    CHECK(result.folders == 0);
    CHECK(!result.cancelled);
}

TEST_CASE(DiskUsage_ReparsePoint_IsCountedButNeverDescendedInto_FOP020_SEC006) {
    test::TempDir dir;
    std::filesystem::create_directories(dir.Path() / L"real");
    test::WriteFileContent(dir.Path() / L"real" / L"inside.txt", "0123456789");  // 10
    if (!test::CreateJunction(dir.Path() / L"link", dir.Path() / L"real")) return;  // skip gracefully (TST-004 note)
    DiskUsageResult result = SumTree(dir.Path());
    // "real" is walked (1 folder, 10 bytes); "link" is counted as a folder entry
    // but never descended into, so its target's bytes are not double-counted.
    CHECK(result.bytes == 10);
    CHECK(result.folders == 2);  // "real" and "link" itself
}

TEST_CASE(DiskUsage_CancelPoll_StopsEarlyAndMarksCancelled_FOP020_PER002) {
    test::TempDir dir;
    for (int i = 0; i < 20; ++i) {
        test::WriteFileContent(dir.Path() / (L"f" + std::to_wstring(i) + L".txt"), "x");
    }
    int seen = 0;
    auto cancelPoll = [&]() { return ++seen > 3; };
    DiskUsageResult result = SumTree(dir.Path(), cancelPoll);
    CHECK(result.cancelled);
    CHECK(result.files < 20);
}

TEST_CASE(DiskUsage_NonexistentPath_ReturnsZeroWithoutCrashing_FOP020) {
    test::TempDir dir;
    DiskUsageResult result = SumTree(dir.Path() / L"does-not-exist");
    CHECK(result.bytes == 0);
    CHECK(result.files == 0);
    CHECK(!result.cancelled);
}
