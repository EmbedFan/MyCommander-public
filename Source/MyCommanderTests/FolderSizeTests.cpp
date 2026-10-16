#include "TestFixtures.h"
#include "TestFramework.h"

#include "DiskUsage.h"
#include "FolderSize.h"

#include <windows.h>

namespace fs = std::filesystem;
using mc::FolderSizeCache;
using mc::FolderSizeState;

TEST_CASE(FolderSizeCache_StateTransitions_LIST010) {
    FolderSizeCache c;
    fs::path p = L"C:\\data\\folder";
    auto l = c.Get(p, 100);
    CHECK(l.state == FolderSizeState::Unknown);
    CHECK(!l.hasValue);
    CHECK(c.NeedsMeasuring(p, 100));

    c.MarkPending(p);
    CHECK(c.Get(p, 100).state == FolderSizeState::Pending);
    CHECK(!c.NeedsMeasuring(p, 100));  // already queued

    c.MarkComputing(p);
    CHECK(c.Get(p, 100).state == FolderSizeState::Computing);
    CHECK(!c.NeedsMeasuring(p, 100));

    c.Complete(p, 4096, 2, 100);
    l = c.Get(p, 100);
    CHECK(l.state == FolderSizeState::Done);
    CHECK(l.hasValue);
    CHECK(l.bytes == 4096);
    CHECK(l.unreadable == 2);
    CHECK(!l.stale);
    CHECK(!c.NeedsMeasuring(p, 100));
}

TEST_CASE(FolderSizeCache_StaleOnWriteTimeChangeOrInvalidate_LIST010) {
    FolderSizeCache c;
    fs::path p = L"C:\\d\\f";
    c.Complete(p, 10, 0, 500);
    CHECK(!c.Get(p, 500).stale);
    auto l = c.Get(p, 900);  // the folder changed
    CHECK(l.stale);
    CHECK(l.hasValue);       // the old total stays visible
    CHECK(l.bytes == 10);
    CHECK(c.NeedsMeasuring(p, 900));

    c.Complete(p, 20, 0, 900);
    CHECK(!c.Get(p, 900).stale);
    c.InvalidateAll();  // Ctrl+R / auto-refresh
    CHECK(c.Get(p, 900).stale);
    CHECK(c.NeedsMeasuring(p, 900));
    c.Complete(p, 20, 0, 900);
    CHECK(!c.Get(p, 900).stale);
    CHECK(!c.Get(p, 0).stale);  // unknown current time never makes it stale by itself
}

TEST_CASE(FolderSizeCache_AbandonKeepsPreviousValue_LIST010) {
    FolderSizeCache c;
    fs::path p = L"C:\\x";
    c.MarkPending(p);
    c.MarkComputing(p);
    c.Abandon(p);
    CHECK(c.Get(p, 1).state == FolderSizeState::Unknown);
    CHECK(c.NeedsMeasuring(p, 1));
    c.Complete(p, 7, 0, 1);
    c.InvalidateAll();
    c.MarkPending(p);
    c.Abandon(p);
    auto l = c.Get(p, 1);
    CHECK(l.state == FolderSizeState::Done);
    CHECK(l.bytes == 7);
}

TEST_CASE(FolderSizeCache_KeyIsLexical_LIST010) {
    FolderSizeCache c;
    c.Complete(L"C:\\Data\\Folder", 5, 0, 1);
    CHECK(c.Get(L"c:\\data\\folder", 1).hasValue);
    CHECK(c.Get(L"C:/Data/Folder/", 1).hasValue);
    CHECK(c.Size() == 1);
}

TEST_CASE(FolderSizeCache_CapEvictsLeastRecentlyShown_LIST010) {
    FolderSizeCache c(3);
    c.Complete(L"C:\\a", 1, 0, 1);
    c.Complete(L"C:\\b", 2, 0, 1);
    c.Complete(L"C:\\c", 3, 0, 1);
    c.Get(L"C:\\a", 1);  // a is now the most recently shown
    c.Complete(L"C:\\d", 4, 0, 1);
    CHECK(c.Size() == 3);
    CHECK(c.Get(L"C:\\a", 1).hasValue);
    CHECK(!c.Get(L"C:\\b", 1).hasValue);  // the least recently shown went
    CHECK(c.Get(L"C:\\c", 1).hasValue);
    CHECK(c.Get(L"C:\\d", 1).hasValue);
}

TEST_CASE(FolderSizeWorker_TotalEqualsSumTree_LIST010) {
    test::TempDir dir;
    fs::path root = dir.Path() / L"tree";
    fs::create_directories(root / L"sub" / L"deep");
    test::WriteFileContent(root / L"a.bin", std::string(1000, 'a'));
    test::WriteFileContent(root / L"sub" / L"b.bin", std::string(2500, 'b'));
    test::WriteFileContent(root / L"sub" / L"deep" / L"c.bin", std::string(300, 'c'));

    mc::DiskUsageResult expected = mc::SumTree(root);
    mc::FolderSizeWorker w;
    w.Want({{root, 1}});
    CHECK(w.WaitIdle(10000));
    auto l = w.Lookup(root, 1);
    CHECK(l.state == FolderSizeState::Done);
    CHECK(l.bytes == expected.bytes);
    CHECK(l.bytes == 3800);
    CHECK(l.unreadable == 0);
    CHECK(w.TakeChanged());
    CHECK(!w.TakeChanged());
}

TEST_CASE(FolderSizeWorker_JunctionNotEntered_LIST010) {
    test::TempDir dir;
    fs::path target = dir.Path() / L"target";
    fs::create_directories(target);
    test::WriteFileContent(target / L"big.bin", std::string(5000, 'x'));
    fs::path root = dir.Path() / L"root";
    fs::create_directories(root);
    test::WriteFileContent(root / L"small.bin", std::string(10, 's'));
    if (!test::CreateJunction(root / L"link", target)) return;  // junctions unavailable: skip
    mc::FolderSizeWorker w;
    w.Want({{root, 1}});
    CHECK(w.WaitIdle(10000));
    CHECK(w.Lookup(root, 1).bytes == 10);
}

TEST_CASE(FolderSizeWorker_InvalidateRecomputes_LIST010) {
    test::TempDir dir;
    fs::path root = dir.Path() / L"r";
    fs::create_directories(root);
    test::WriteFileContent(root / L"a.bin", std::string(100, 'a'));
    mc::FolderSizeWorker w;
    w.Want({{root, 1}});
    CHECK(w.WaitIdle(10000));
    CHECK(w.Lookup(root, 1).bytes == 100);

    test::WriteFileContent(root / L"b.bin", std::string(50, 'b'));
    w.InvalidateAll();
    CHECK(w.Lookup(root, 1).stale);
    CHECK(w.Lookup(root, 1).bytes == 100);  // old total visible meanwhile
    w.Want({{root, 1}});
    CHECK(w.WaitIdle(10000));
    CHECK(w.Lookup(root, 1).bytes == 150);
    CHECK(!w.Lookup(root, 1).stale);
}

TEST_CASE(FolderSizeWorker_WantReplacesQueueAndKeepsFinished_LIST010) {
    test::TempDir dir;
    fs::path a = dir.Path() / L"a";
    fs::path b = dir.Path() / L"b";
    fs::create_directories(a);
    fs::create_directories(b);
    test::WriteFileContent(a / L"f.bin", std::string(10, 'a'));
    test::WriteFileContent(b / L"f.bin", std::string(20, 'b'));
    mc::FolderSizeWorker w;
    w.Want({{a, 1}});
    CHECK(w.WaitIdle(10000));
    w.Want({{b, 1}});  // navigated: a is no longer shown
    CHECK(w.WaitIdle(10000));
    CHECK(w.Lookup(b, 1).bytes == 20);
    CHECK(w.Lookup(a, 1).hasValue);  // finished results are kept
    CHECK(w.Lookup(a, 1).bytes == 10);
}
