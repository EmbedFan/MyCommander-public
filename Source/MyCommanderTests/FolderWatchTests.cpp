#include "TestFixtures.h"
#include "TestFramework.h"

#include "FolderWatch.h"

#include <windows.h>

namespace fs = std::filesystem;

using mc::Debouncer;
using mc::FolderWatch;

namespace {

// Polls up to `timeoutMs` for a signal.
bool SignalsWithin(FolderWatch& watch, DWORD timeoutMs) {
    const ULONGLONG deadline = GetTickCount64() + timeoutMs;
    do {
        if (watch.Poll()) return true;
        Sleep(10);
    } while (GetTickCount64() < deadline);
    return false;
}

}  // namespace

// --- Debouncer (pure; the clock is a plain number) ---------------------------

TEST_CASE(Debouncer_SingleSignalFiresAfterQuietPeriod_LIST007) {
    Debouncer d(300, 2000);
    CHECK(!d.Due(0));
    d.Signal(1000);
    CHECK(d.Pending());
    CHECK(!d.Due(1100));
    CHECK(!d.Due(1299));
    CHECK(d.Due(1300));
    d.Clear();
    CHECK(!d.Pending());
    CHECK(!d.Due(5000));
}

TEST_CASE(Debouncer_BurstFiresOnce_LIST007) {
    Debouncer d(300, 2000);
    for (uint64_t t = 0; t < 1000; t += 100) d.Signal(t);  // every 100 ms for 0.9 s
    CHECK(!d.Due(900 + 299));
    CHECK(d.Due(900 + 300));
}

TEST_CASE(Debouncer_ContinuousChangesFireAtTheCap_LIST007) {
    Debouncer d(300, 2000);
    for (uint64_t t = 0; t <= 2000; t += 100) d.Signal(t);  // never quiet for 300 ms
    CHECK(d.Due(2000));                                      // 2 s since the first signal
    d.Clear();
    d.Signal(2100);                                          // the next burst starts its own cap
    CHECK(!d.Due(2200));
}

TEST_CASE(Debouncer_StaysDueWhileDeferredThenFiresOnRelease_LIST007) {
    Debouncer d(300, 2000);
    d.Signal(0);
    CHECK(d.Due(500));   // the caller defers (user busy) and does not Clear
    CHECK(d.Due(4000));  // still due when released
}

// --- FolderWatch (real folder) ------------------------------------------------

TEST_CASE(FolderWatch_SignalsCreateRenameResizeDelete_LIST007) {
    test::TempDir dir;
    FolderWatch watch;
    CHECK(watch.Arm(dir.Path().wstring()));
    CHECK(watch.GetState() == FolderWatch::State::Active);
    CHECK(!watch.Poll());  // nothing yet

    test::WriteFileContent(dir.Path() / L"a.txt", "1");
    CHECK(SignalsWithin(watch, 3000));
    watch.Drain();

    std::error_code ec;
    fs::rename(dir.Path() / L"a.txt", dir.Path() / L"b.txt", ec);
    CHECK(SignalsWithin(watch, 3000));
    watch.Drain();

    test::WriteFileContent(dir.Path() / L"b.txt", "a much longer content than before");
    CHECK(SignalsWithin(watch, 3000));
    watch.Drain();

    fs::remove(dir.Path() / L"b.txt", ec);
    CHECK(SignalsWithin(watch, 3000));
}

TEST_CASE(FolderWatch_IgnoresChangesInSubfolders_LIST007) {
    test::TempDir dir;
    std::error_code ec;
    // Two levels down: NTFS can lazily refresh a *direct* subfolder's own
    // directory entry (which is a change to the watched folder), but never a
    // grandchild's.
    fs::create_directories(dir.Path() / L"sub" / L"inner", ec);
    test::WriteFileContent(dir.Path() / L"sub" / L"inner" / L"deep.txt", "x");
    FolderWatch watch;
    CHECK(watch.Arm(dir.Path().wstring()));
    SignalsWithin(watch, 300);  // let any lazy entry updates from the setup settle
    watch.Drain();

    test::WriteFileContent(dir.Path() / L"sub" / L"inner" / L"deep.txt", "xyz");
    CHECK(!SignalsWithin(watch, 400));
}

TEST_CASE(FolderWatch_ReArmWatchesOnlyTheNewFolder_LIST007) {
    test::TempDir first;
    test::TempDir second;
    FolderWatch watch;
    CHECK(watch.Arm(first.Path().wstring()));
    CHECK(watch.Arm(second.Path().wstring()));
    CHECK(watch.Folder() == second.Path().wstring());

    test::WriteFileContent(first.Path() / L"old.txt", "x");
    CHECK(!SignalsWithin(watch, 400));
    test::WriteFileContent(second.Path() / L"new.txt", "x");
    CHECK(SignalsWithin(watch, 3000));
}

TEST_CASE(FolderWatch_MissingFolderIsUnavailableAndCloseIsSafe_LIST007) {
    FolderWatch watch;
    CHECK(!watch.Arm(L"C:\\this\\folder\\does\\not\\exist\\LIST007"));
    CHECK(watch.GetState() == FolderWatch::State::Unavailable);
    CHECK(!watch.Poll());
    watch.Close();
    CHECK(watch.GetState() == FolderWatch::State::Closed);
    watch.Close();  // twice is fine
}
