#include "TestFixtures.h"
#include "TestFramework.h"

#include "Panel.h"
#include "PathUtil.h"

#include <windows.h>
#include <psapi.h>

#include <algorithm>
#include <cstdio>

#pragma comment(lib, "psapi.lib")

namespace fs = std::filesystem;

using mc::Entry;
using mc::GetLastWriteTimeOrZero;
using mc::IsFilesystemRoot;
using mc::Panel;
using mc::SortKey;

namespace {

const Entry* FindEntry(const Panel& panel, const std::wstring& name) {
    const auto& entries = panel.Entries();
    auto it = std::find_if(entries.begin(), entries.end(), [&](const Entry& e) { return e.name == name; });
    return it == entries.end() ? nullptr : &*it;
}

// Names in listed order, excluding "..", so a sort test can assert an exact
// sequence without depending on directory-enumeration order.
std::vector<std::wstring> NamesExcludingParent(const Panel& panel) {
    std::vector<std::wstring> names;
    for (const auto& e : panel.Entries()) {
        if (e.name != L"..") names.push_back(e.name);
    }
    return names;
}

// PER-003: this process' current working-set size, to check that loading a
// huge directory grows memory by something proportional to its entry count
// rather than by something unbounded (a leak, or accidental duplication
// across incremental batches).
size_t CurrentWorkingSetBytes() {
    PROCESS_MEMORY_COUNTERS pmc{};
    GetProcessMemoryInfo(GetCurrentProcess(), &pmc, sizeof(pmc));
    return pmc.WorkingSetSize;
}

}  // namespace

TEST_CASE(Panel_ReadsHiddenAttributeIntoEntry_CON005) {
    test::TempDir root;
    fs::path file = root.Path() / L"secret.txt";
    test::WriteFileContent(file, "x");
    CHECK(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0);

    Panel panel(root.Path());
    panel.ToggleHiddenAndSystem();

    const Entry* entry = FindEntry(panel, L"secret.txt");
    CHECK(entry != nullptr);
    if (entry) {
        CHECK((entry->attributes & FILE_ATTRIBUTE_HIDDEN) != 0);
    }
}

TEST_CASE(Panel_HiddenAndSystemEntries_AreOptional_NAV010) {
    test::TempDir root;
    fs::path hidden = root.Path() / L"hidden.txt";
    fs::path system = root.Path() / L"system.txt";
    test::WriteFileContent(hidden, "hidden");
    test::WriteFileContent(system, "system");
    CHECK(SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0);
    CHECK(SetFileAttributesW(system.c_str(), FILE_ATTRIBUTE_SYSTEM) != 0);

    Panel panel(root.Path());
    CHECK(!panel.ShowingHiddenAndSystem());
    CHECK(FindEntry(panel, L"hidden.txt") == nullptr);
    CHECK(FindEntry(panel, L"system.txt") == nullptr);

    panel.ToggleHiddenAndSystem();
    CHECK(panel.ShowingHiddenAndSystem());
    const Entry* hiddenEntry = FindEntry(panel, L"hidden.txt");
    const Entry* systemEntry = FindEntry(panel, L"system.txt");
    CHECK(hiddenEntry != nullptr);
    CHECK(systemEntry != nullptr);
    if (hiddenEntry) CHECK((hiddenEntry->attributes & FILE_ATTRIBUTE_HIDDEN) != 0);
    if (systemEntry) CHECK((systemEntry->attributes & FILE_ATTRIBUTE_SYSTEM) != 0);

    panel.ToggleHiddenAndSystem();
    CHECK(!panel.ShowingHiddenAndSystem());
    CHECK(FindEntry(panel, L"hidden.txt") == nullptr);

    SetFileAttributesW(hidden.c_str(), FILE_ATTRIBUTE_NORMAL);
    SetFileAttributesW(system.c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST_CASE(Panel_ReportsCurrentVolumeInfo_UI003) {
    test::TempDir root;
    Panel panel(root.Path());

    const auto& volume = panel.Volume();
    CHECK(volume.available);
    CHECK(!volume.root.empty());
    CHECK(volume.driveType != DRIVE_UNKNOWN);
    CHECK(volume.spaceKnown);
    if (volume.spaceKnown) {
        CHECK(volume.totalBytes > 0);
        CHECK(volume.freeBytes <= volume.totalBytes);
    }
}

TEST_CASE(Panel_ReadsReadOnlyAndArchiveAttributesIntoEntry_CON005) {
    test::TempDir root;
    fs::path file = root.Path() / L"ro.txt";
    test::WriteFileContent(file, "x");
    // New files get FILE_ATTRIBUTE_ARCHIVE by default; add READONLY on top.
    DWORD current = GetFileAttributesW(file.c_str());
    CHECK(SetFileAttributesW(file.c_str(), current | FILE_ATTRIBUTE_READONLY) != 0);

    Panel panel(root.Path());

    const Entry* entry = FindEntry(panel, L"ro.txt");
    CHECK(entry != nullptr);
    if (entry) {
        CHECK((entry->attributes & FILE_ATTRIBUTE_READONLY) != 0);
        CHECK((entry->attributes & FILE_ATTRIBUTE_ARCHIVE) != 0);
        CHECK((entry->attributes & FILE_ATTRIBUTE_HIDDEN) == 0);
    }

    // Leave the temp file writable so TempDir's destructor can remove it.
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
}

// CON-005 "drive letters ... shall be supported": a drive root's own
// parent_path() is itself (per std::filesystem's root-path convention), so
// Panel's ".."-suppression guard (`has_parent_path() && parent_path() !=
// path_`) must correctly recognize a drive root as having no parent —
// otherwise the panel would offer a ".." that leads nowhere new.
TEST_CASE(Panel_DriveRootHasNoParentEntry_CON005) {
    Panel panel(L"C:\\");

    CHECK(FindEntry(panel, L"..") == nullptr);
}

// CON-005 "UNC paths ... shall be supported": exercised at the
// IsFilesystemRoot/std::filesystem::path level rather than through a live
// Panel pointed at a network share, since a real Panel constructed on an
// unreachable "\\server\share" would have to actually attempt SMB name
// resolution first, which can hang for tens of seconds with no network.
//
// std::filesystem's own root_name() for a UNC path covers only the server
// ("\\server"), NOT the share — root_name()+root_directory() alone lands on
// a bare "\\server\" that isn't a real, listable directory. IsFilesystemRoot
// exists specifically to recognize the share itself ("\\server\share") as
// the correct stopping point instead.
TEST_CASE(IsFilesystemRoot_RecognizesUncShareRootButNotOneLevelDeeper) {
    fs::path uncShareRoot(L"\\\\server\\share");
    fs::path uncSubdir(L"\\\\server\\share\\sub");

    CHECK(IsFilesystemRoot(uncShareRoot));
    CHECK(!IsFilesystemRoot(uncSubdir));
}

TEST_CASE(IsFilesystemRoot_RecognizesDriveRootButNotOneLevelDeeper) {
    CHECK(IsFilesystemRoot(fs::path(L"C:\\")));
    CHECK(!IsFilesystemRoot(fs::path(L"C:\\Users")));
}

TEST_CASE(Panel_ReadsLastWriteTimeIntoEntry_UI004) {
    test::TempDir root;
    fs::path file = root.Path() / L"stamped.txt";
    test::WriteFileContent(file, "x");

    SYSTEMTIME st{};
    st.wYear = 2020;
    st.wMonth = 6;
    st.wDay = 15;
    st.wHour = 12;
    st.wMinute = 30;
    FILETIME localFt{};
    FILETIME utcFt{};
    CHECK(SystemTimeToFileTime(&st, &localFt) != 0);
    CHECK(LocalFileTimeToFileTime(&localFt, &utcFt) != 0);

    HANDLE h = CreateFileW(file.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                          OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    if (h != INVALID_HANDLE_VALUE) {
        CHECK(SetFileTime(h, nullptr, nullptr, &utcFt) != 0);
        CloseHandle(h);
    }

    Panel panel(root.Path());

    const Entry* entry = FindEntry(panel, L"stamped.txt");
    CHECK(entry != nullptr);
    if (entry) {
        ULARGE_INTEGER expected;
        expected.LowPart = utcFt.dwLowDateTime;
        expected.HighPart = utcFt.dwHighDateTime;
        CHECK(entry->lastWriteTimeUtc == expected.QuadPart);
    }
}

TEST_CASE(GetLastWriteTimeOrZero_NonexistentPathReturnsZero_UI004) {
    test::TempDir root;

    CHECK(GetLastWriteTimeOrZero(root.Path() / L"missing.txt") == 0);
}

TEST_CASE(Panel_DefaultSortIsNameAscendingWithDirectoriesFirst_NAV005) {
    test::TempDir root;
    fs::create_directory(root.Path() / L"zdir");
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());

    CHECK(panel.Sort() == SortKey::Name);
    CHECK(!panel.SortDescending());
    auto names = NamesExcludingParent(panel);
    CHECK(names.size() == 3);
    if (names.size() == 3) {
        CHECK(names[0] == L"zdir");  // directories always sort before files
        CHECK(names[1] == L"afile.txt");
        CHECK(names[2] == L"bfile.txt");
    }
}

TEST_CASE(Panel_SetSort_NameDescending_ReversesOrder_NAV005) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    panel.SetSort(SortKey::Name, /*descending=*/true);

    CHECK(panel.Sort() == SortKey::Name);
    CHECK(panel.SortDescending());
    auto names = NamesExcludingParent(panel);
    CHECK(names.size() == 2);
    if (names.size() == 2) {
        CHECK(names[0] == L"bfile.txt");
        CHECK(names[1] == L"afile.txt");
    }
}

TEST_CASE(Panel_SetSort_BySize_SmallestFirstAscending_LIST002) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"big.txt", "xxxxxxxxxx");
    test::WriteFileContent(root.Path() / L"small.txt", "x");

    Panel panel(root.Path());
    panel.SetSort(SortKey::Size, /*descending=*/false);

    auto names = NamesExcludingParent(panel);
    CHECK(names.size() == 2);
    if (names.size() == 2) {
        CHECK(names[0] == L"small.txt");
        CHECK(names[1] == L"big.txt");
    }
}

TEST_CASE(Panel_SetSort_ByExtension_GroupsByExtensionThenName_LIST002) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"b.log", "x");
    test::WriteFileContent(root.Path() / L"a.txt", "x");
    test::WriteFileContent(root.Path() / L"c.log", "x");

    Panel panel(root.Path());
    panel.SetSort(SortKey::Extension, /*descending=*/false);

    auto names = NamesExcludingParent(panel);
    CHECK(names.size() == 3);
    if (names.size() == 3) {
        CHECK(names[0] == L"b.log");  // .log before .txt; alphabetical by name within .log
        CHECK(names[1] == L"c.log");
        CHECK(names[2] == L"a.txt");
    }
}

TEST_CASE(Panel_SetSort_ByModifiedTime_OldestFirstAscending_LIST002) {
    test::TempDir root;
    fs::path older = root.Path() / L"older.txt";
    fs::path newer = root.Path() / L"newer.txt";
    test::WriteFileContent(older, "x");
    test::WriteFileContent(newer, "x");

    auto setYear = [](const fs::path& p, int year) {
        SYSTEMTIME st{};
        st.wYear = static_cast<WORD>(year);
        st.wMonth = 1;
        st.wDay = 1;
        st.wHour = 12;
        FILETIME localFt{}, utcFt{};
        SystemTimeToFileTime(&st, &localFt);
        LocalFileTimeToFileTime(&localFt, &utcFt);
        HANDLE h = CreateFileW(p.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                              OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            SetFileTime(h, nullptr, nullptr, &utcFt);
            CloseHandle(h);
        }
    };
    setYear(older, 2019);
    setYear(newer, 2021);

    Panel panel(root.Path());
    panel.SetSort(SortKey::ModifiedTime, /*descending=*/false);

    auto names = NamesExcludingParent(panel);
    CHECK(names.size() == 2);
    if (names.size() == 2) {
        CHECK(names[0] == L"older.txt");
        CHECK(names[1] == L"newer.txt");
    }
}

TEST_CASE(Panel_SetSort_PreservesCursorOnSameEntry_LIST005) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    // Entries are [.., afile.txt, bfile.txt] under default name-ascending
    // order (TempDir sits under %TEMP%, so it has a real parent and gets a
    // ".." entry) — move two steps to land on "bfile.txt".
    panel.MoveCursor(2, /*visibleRows=*/10);
    CHECK(panel.Entries()[panel.Cursor()].name == L"bfile.txt");

    panel.SetSort(SortKey::Name, /*descending=*/true);  // reverses order

    CHECK(panel.Entries()[panel.Cursor()].name == L"bfile.txt");
}

// NAV-005: two Panel instances never share sort state — each simply owns
// its own sortKey_/sortDescending_ members, the same way it already owns
// its own path/cursor/selection.
TEST_CASE(Panel_SortStateIsIndependentPerPanelInstance_NAV005) {
    test::TempDir rootA;
    test::TempDir rootB;
    test::WriteFileContent(rootA.Path() / L"a.txt", "x");
    test::WriteFileContent(rootB.Path() / L"b.txt", "x");

    Panel panelA(rootA.Path());
    Panel panelB(rootB.Path());

    panelA.SetSort(SortKey::Size, /*descending=*/true);

    CHECK(panelA.Sort() == SortKey::Size);
    CHECK(panelA.SortDescending());
    CHECK(panelB.Sort() == SortKey::Name);
    CHECK(!panelB.SortDescending());
}

TEST_CASE(Panel_LocationInaccessible_WhenDirectoryDoesNotExist_LIST006) {
    test::TempDir root;
    fs::path missing = root.Path() / L"does-not-exist";

    Panel panel(missing);

    CHECK(panel.LocationInaccessible());
    CHECK(!panel.StatusMessage().empty());
}

TEST_CASE(Panel_LocationInaccessible_ClearsAfterNavigatingToValidDirectory_LIST006) {
    test::TempDir root;
    fs::path missing = root.Path() / L"does-not-exist";

    Panel panel(missing);
    CHECK(panel.LocationInaccessible());

    panel.NavigateTo(root.Path());

    CHECK(!panel.LocationInaccessible());
    CHECK(panel.StatusMessage().empty());
}

TEST_CASE(Panel_DirectoryGroupingCanBeDisabled_LIST004) {
    test::TempDir root;
    fs::create_directory(root.Path() / L"zdir");
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path(), /*groupDirectoriesFirst=*/false);
    auto ungrouped = NamesExcludingParent(panel);
    CHECK(!panel.GroupDirectoriesFirst());
    CHECK(ungrouped.size() == 3);
    if (ungrouped.size() == 3) {
        CHECK(ungrouped[0] == L"afile.txt");
        CHECK(ungrouped[1] == L"bfile.txt");
        CHECK(ungrouped[2] == L"zdir");
    }

    panel.SetGroupDirectoriesFirst(true);
    auto grouped = NamesExcludingParent(panel);
    CHECK(panel.GroupDirectoriesFirst());
    CHECK(grouped.size() == 3);
    if (grouped.size() == 3) {
        CHECK(grouped[0] == L"zdir");
        CHECK(grouped[1] == L"afile.txt");
        CHECK(grouped[2] == L"bfile.txt");
    }
}

TEST_CASE(Panel_IncrementalRefresh_LoadsLargeDirectoryInBatches_LIST008) {
    test::TempDir root;
    for (int i = 0; i < 300; ++i) {
        test::WriteFileContent(root.Path() / (L"item" + std::to_wstring(i) + L".txt"), "x");
    }

    Panel panel(root.Path(), /*groupDirectoriesFirst=*/true, /*incrementalLoading=*/true);
    CHECK(panel.IsLoading());
    CHECK(NamesExcludingParent(panel).empty());

    panel.PumpRefresh(16);
    CHECK(panel.IsLoading());
    CHECK(NamesExcludingParent(panel).size() > 0);
    CHECK(NamesExcludingParent(panel).size() < 300);

    while (panel.IsLoading()) panel.PumpRefresh(16);
    auto names = NamesExcludingParent(panel);
    CHECK(names.size() == 300);
    CHECK(names.front() == L"item0.txt");
}

// IS-0001: NavigateTo(dir, selectName)'s selection used to be dead code for
// an incremental panel (LIST-008) — SetLocation searched entries_ for
// selectName immediately after Refresh() started an async load, before any
// entries existed. Every existing NavigateTo test above uses the default
// (non-incremental) Panel constructor, where that search happened to run
// after the listing was already complete — masking the bug. These
// explicitly use incrementalLoading=true, the mode both of the app's real
// panels always run in, to actually exercise the path that was broken.

TEST_CASE(Panel_NavigateTo_IncrementalRefresh_SelectsNewlyCreatedName_IS0001) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"existing.txt", "x");

    Panel panel(root.Path(), /*groupDirectoriesFirst=*/true, /*incrementalLoading=*/true);
    while (panel.IsLoading()) panel.PumpRefresh(16);
    CHECK(NamesExcludingParent(panel).size() == 1);

    // Simulates DoCreateNewFile/DoMakeDirectory: the item is created on disk
    // first, then the panel is told to refresh and select it — exactly the
    // sequence main.cpp now uses.
    test::WriteFileContent(root.Path() / L"newfile.txt", "y");
    panel.NavigateTo(panel.Path(), L"newfile.txt");
    CHECK(panel.IsLoading());  // refresh started but not finished yet

    while (panel.IsLoading()) panel.PumpRefresh(16);
    CHECK(NamesExcludingParent(panel).size() == 2);
    CHECK(panel.Cursor() >= 0 && panel.Cursor() < static_cast<int>(panel.Entries().size()));
    if (panel.Cursor() >= 0 && panel.Cursor() < static_cast<int>(panel.Entries().size())) {
        CHECK(panel.Entries()[panel.Cursor()].name == L"newfile.txt");
    }
}

TEST_CASE(Panel_NavigateTo_IncrementalRefresh_SelectsExistingName_IS0001) {
    // Same mechanism DoSearch relies on to land the cursor on a chosen
    // search result — the item already exists before NavigateTo is called.
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"alpha.txt", "a");
    test::WriteFileContent(root.Path() / L"beta.txt", "b");
    test::WriteFileContent(root.Path() / L"gamma.txt", "c");

    Panel panel(root.Path(), /*groupDirectoriesFirst=*/true, /*incrementalLoading=*/true);
    while (panel.IsLoading()) panel.PumpRefresh(16);
    CHECK(panel.Entries()[panel.Cursor()].name != L"beta.txt");  // starts elsewhere (cursor defaults to index 0)

    panel.NavigateTo(panel.Path(), L"beta.txt");
    while (panel.IsLoading()) panel.PumpRefresh(16);

    CHECK(panel.Cursor() >= 0 && panel.Cursor() < static_cast<int>(panel.Entries().size()));
    if (panel.Cursor() >= 0 && panel.Cursor() < static_cast<int>(panel.Entries().size())) {
        CHECK(panel.Entries()[panel.Cursor()].name == L"beta.txt");
    }
}

TEST_CASE(Panel_NavigateTo_IncrementalRefresh_SelectionSurvivesMultipleBatches_IS0001) {
    // Enough entries that the refresh needs several small PumpRefresh
    // batches to complete, confirming the selection is applied once by
    // FinishRefresh() at the end, not lost partway through.
    test::TempDir root;
    for (int i = 0; i < 50; ++i) {
        test::WriteFileContent(root.Path() / (L"item" + std::to_wstring(i) + L".txt"), "x");
    }

    Panel panel(root.Path(), /*groupDirectoriesFirst=*/true, /*incrementalLoading=*/true);
    while (panel.IsLoading()) panel.PumpRefresh(8);

    panel.NavigateTo(panel.Path(), L"item25.txt");
    CHECK(panel.IsLoading());
    int batches = 0;
    while (panel.IsLoading()) {
        panel.PumpRefresh(8);  // small batches -> several PumpRefresh calls before FinishRefresh runs
        ++batches;
        CHECK(batches < 1000);
    }
    CHECK(batches > 1);

    CHECK(panel.Cursor() >= 0 && panel.Cursor() < static_cast<int>(panel.Entries().size()));
    if (panel.Cursor() >= 0 && panel.Cursor() < static_cast<int>(panel.Entries().size())) {
        CHECK(panel.Entries()[panel.Cursor()].name == L"item25.txt");
    }
}

TEST_CASE(Panel_NavigateTo_IncrementalRefresh_NoSelectNamePreservesCursor_IS0001) {
    // Regression check: the ordinary "preserve wherever the cursor already
    // was" behavior (LIST-005) must still work once pendingSelectName_ is
    // introduced as a separate, higher-priority mechanism.
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"alpha.txt", "a");
    test::WriteFileContent(root.Path() / L"beta.txt", "b");

    Panel panel(root.Path(), /*groupDirectoriesFirst=*/true, /*incrementalLoading=*/true);
    while (panel.IsLoading()) panel.PumpRefresh(16);
    panel.MoveCursor(1, 10);
    std::wstring cursorNameBefore = panel.Entries()[panel.Cursor()].name;

    panel.NavigateTo(panel.Path());  // no selectName
    while (panel.IsLoading()) panel.PumpRefresh(16);

    CHECK(panel.Entries()[panel.Cursor()].name == cursorNameBefore);
}

// PER-003: a directory of at least 100,000 entries must load without an
// application failure (crash, hang) or unbounded memory growth. The
// literal 100,000-entry scale is validated live against the real running
// app (see the 2026-09-22 changelog entry) rather than in this automated
// suite, which every future test run pays for -- creating (and TempDir
// cleaning up) 100,000 files takes tens of seconds on its own, wildly out
// of proportion with the rest of this suite's runtime, for a code path
// that has no N-dependent behavior change between 10,000 and 100,000
// entries (the same fixed-size incremental batch loop either way). This
// still exercises 60+ real PumpRefresh batches (at the default batch
// size) and directly checks for the two failure modes PER-003 actually
// cares about: entry loss/duplication and unbounded memory growth.
// Created via raw CreateFileW/CloseHandle rather than
// test::WriteFileContent's ofstream per file, since this still needs to
// create thousands of them without the test itself becoming the
// bottleneck.
TEST_CASE(Panel_HugeDirectory_LoadsAllEntriesWithBoundedMemoryGrowth_PER003) {
    test::TempDir root;
    constexpr int kEntryCount = 10000;
    for (int i = 0; i < kEntryCount; ++i) {
        std::wstring name = root.Path().wstring() + L"\\f" + std::to_wstring(i) + L".txt";
        HANDLE h = CreateFileW(name.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
        CHECK(h != INVALID_HANDLE_VALUE);
        if (h != INVALID_HANDLE_VALUE) CloseHandle(h);
    }

    size_t beforeBytes = CurrentWorkingSetBytes();

    Panel panel(root.Path(), /*groupDirectoriesFirst=*/true, /*incrementalLoading=*/true);
    CHECK(panel.IsLoading());
    int batches = 0;
    while (panel.IsLoading()) {
        panel.PumpRefresh(256);
        ++batches;
        CHECK(batches < 10000);  // a runaway loop (never finishing) fails loudly rather than hanging forever
    }

    auto names = NamesExcludingParent(panel);
    CHECK(names.size() == static_cast<size_t>(kEntryCount));  // every entry present exactly once — no loss, no duplication

    size_t afterBytes = CurrentWorkingSetBytes();
    // A generous bound: ~20 MB for 10,000 entries is roughly 2 KB/entry, an
    // order of magnitude above Entry's actual footprint — comfortable
    // headroom for allocator overhead while still catching a genuine
    // unbounded-growth bug (which would blow well past this, and scale with
    // however many PumpRefresh batches ran rather than with entry count).
    uint64_t grownBytes = afterBytes > beforeBytes ? static_cast<uint64_t>(afterBytes - beforeBytes) : 0;
    CHECK(grownBytes < 20ull * 1024 * 1024);
}

TEST_CASE(Panel_NavigationHistory_SupportsIndependentBackAndForward_NAV006) {
    test::TempDir root;
    fs::path first = root.Path() / L"first";
    fs::path second = root.Path() / L"second";
    fs::path replacement = root.Path() / L"replacement";
    fs::create_directories(first);
    fs::create_directories(second);
    fs::create_directories(replacement);

    Panel panel(root.Path());
    panel.NavigateTo(first);
    panel.NavigateTo(second);
    CHECK(panel.CanNavigateBack());
    CHECK(!panel.CanNavigateForward());

    CHECK(panel.NavigateBack());
    CHECK(panel.Path() == first);
    CHECK(panel.CanNavigateBack());
    CHECK(panel.CanNavigateForward());

    CHECK(panel.NavigateBack());
    CHECK(panel.Path() == root.Path());
    CHECK(!panel.CanNavigateBack());
    CHECK(panel.NavigateForward());
    CHECK(panel.Path() == first);

    panel.NavigateTo(replacement);
    CHECK(panel.Path() == replacement);
    CHECK(!panel.CanNavigateForward());
    CHECK(!panel.NavigateForward());
}

TEST_CASE(Panel_ValidDirectory_IsNotMarkedInaccessible_LIST006) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"file.txt", "x");

    Panel panel(root.Path());

    CHECK(!panel.LocationInaccessible());
}

// LIST-006: a dangling junction (its target removed after creation) must
// fail is_directory()/file_size() the same way a broken symlink would on
// any platform — Panel::Refresh should mark it Entry::inaccessible rather
// than silently listing it as an ordinary 0-byte file.
TEST_CASE(Panel_BrokenJunction_IsMarkedInaccessibleNotAsAnEmptyFile_LIST006) {
    test::TempDir root;
    fs::path targetDir = root.Path() / L"target";
    fs::path linkDir = root.Path() / L"link";
    fs::create_directory(targetDir);

    if (!test::CreateJunction(linkDir, targetDir)) {
        wprintf(L"  (skipped: could not create an NTFS junction on this system)\n");
        return;
    }

    std::error_code ec;
    fs::remove_all(targetDir, ec);
    CHECK(!ec);

    Panel panel(root.Path());

    const Entry* entry = FindEntry(panel, L"link");
    CHECK(entry != nullptr);
    if (entry) {
        CHECK(entry->inaccessible);
        CHECK(entry->sizeBytes == 0);
    }
}

TEST_CASE(Panel_ReparsePoint_IsIdentifiedAndCannotBeEntered_NAV009) {
    test::TempDir root;
    fs::path targetDir = root.Path() / L"target";
    fs::path linkDir = root.Path() / L"link";
    fs::create_directory(targetDir);

    if (!test::CreateJunction(linkDir, targetDir)) {
        wprintf(L"  (skipped: could not create an NTFS junction on this system)\n");
        return;
    }

    Panel panel(root.Path());
    const Entry* entry = FindEntry(panel, L"link");
    CHECK(entry != nullptr);
    if (!entry) return;
    CHECK(entry->isReparsePoint);
    CHECK(entry->isDirectory);

    int linkIndex = static_cast<int>(entry - panel.Entries().data());
    panel.MoveCursor(linkIndex, 20);
    panel.EnterSelected();

    CHECK(panel.Path() == root.Path());
    CHECK(panel.StatusMessage().find(L"reparse point") != std::wstring::npos);

    panel.NavigateTo(linkDir);  // Search/picker navigation uses this direct path too.
    CHECK(panel.Path() == root.Path());
}

// --- Selection (SEL-*, TST-001) ---------------------------------------------
// Panel's selection state had no dedicated unit tests at all before this —
// TST-001's remaining gap, now that sorting (NAV-005/LIST-002/LIST-003) has
// its own tests above. All of these use two known files under a fresh
// TempDir, so the default name-ascending sort gives a deterministic
// [.., afile.txt, bfile.txt] order and the cursor can be positioned by a
// fixed MoveCursor delta from 0.

TEST_CASE(Panel_ToggleCursorSelection_SelectsEntryAndAdvancesCursor_SEL001) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    panel.MoveCursor(1, /*visibleRows=*/10);  // cursor -> afile.txt (index 1)
    CHECK(panel.Entries()[panel.Cursor()].name == L"afile.txt");

    panel.ToggleCursorSelection(/*visibleRows=*/10);

    CHECK(panel.Entries()[1].selected);
    CHECK(panel.Cursor() == 2);  // advanced onto bfile.txt
    CHECK(panel.SelectedCount() == 1);
}

TEST_CASE(Panel_ToggleCursorSelection_TogglingTwiceDeselectsAgain_SEL001) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    panel.MoveCursor(1, 10);  // cursor -> afile.txt (the last entry: only ".." and this file exist)
    panel.ToggleCursorSelection(10);
    CHECK(panel.SelectedCount() == 1);
    // ToggleCursorSelection's own advance-by-one clamps back onto afile.txt,
    // since it's the last entry — no need to move the cursor back manually.
    CHECK(panel.Entries()[panel.Cursor()].name == L"afile.txt");

    panel.ToggleCursorSelection(10);

    CHECK(panel.SelectedCount() == 0);
}

TEST_CASE(Panel_ToggleCursorSelection_NeverSelectsParentEntry_SEL001) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    CHECK(panel.Entries()[panel.Cursor()].name == L"..");  // cursor starts on ".."

    panel.ToggleCursorSelection(10);

    CHECK(panel.SelectedCount() == 0);
    CHECK(panel.Cursor() == 1);  // cursor still advances past it
}

TEST_CASE(Panel_SelectAll_SelectsEveryEntryExceptParent_SEL002) {
    test::TempDir root;
    fs::create_directory(root.Path() / L"subdir");
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    panel.SelectAll();

    CHECK(panel.SelectedCount() == 2);  // subdir + afile.txt, not ".."
    const Entry* parent = FindEntry(panel, L"..");
    CHECK(parent != nullptr);
    if (parent) CHECK(!parent->selected);
}

TEST_CASE(Panel_ClearSelection_DeselectsEverything_SEL002) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    panel.SelectAll();
    CHECK(panel.SelectedCount() == 2);

    panel.ClearSelection();

    CHECK(panel.SelectedCount() == 0);
}

TEST_CASE(Panel_InvertSelection_FlipsEveryEntryExceptParent_SEL002) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    panel.MoveCursor(1, 10);          // cursor -> afile.txt
    panel.ToggleCursorSelection(10);  // selects afile.txt, cursor -> bfile.txt

    panel.InvertSelection();

    const Entry* a = FindEntry(panel, L"afile.txt");
    const Entry* b = FindEntry(panel, L"bfile.txt");
    const Entry* dotdot = FindEntry(panel, L"..");
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(dotdot != nullptr);
    if (a) CHECK(!a->selected);
    if (b) CHECK(b->selected);
    if (dotdot) CHECK(!dotdot->selected);
}

TEST_CASE(Panel_SelectByMask_GlobPattern_SelectsOnlyMatchingEntries_SEL003) {
    test::TempDir root;
    fs::create_directory(root.Path() / L"subdir");
    test::WriteFileContent(root.Path() / L"a.txt", "x");
    test::WriteFileContent(root.Path() / L"b.txt", "x");
    test::WriteFileContent(root.Path() / L"c.log", "x");

    Panel panel(root.Path());
    panel.SelectByMask(L"*.txt", /*select=*/true);

    const Entry* a = FindEntry(panel, L"a.txt");
    const Entry* b = FindEntry(panel, L"b.txt");
    const Entry* c = FindEntry(panel, L"c.log");
    const Entry* dir = FindEntry(panel, L"subdir");
    CHECK(a != nullptr);
    CHECK(b != nullptr);
    CHECK(c != nullptr);
    CHECK(dir != nullptr);
    if (a) CHECK(a->selected);
    if (b) CHECK(b->selected);
    if (c) CHECK(!c->selected);
    if (dir) CHECK(!dir->selected);
    CHECK(panel.SelectedCount() == 2);
}

TEST_CASE(Panel_SelectByMask_Deselect_ClearsOnlyMatchingEntries_SEL003) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"a.txt", "x");
    test::WriteFileContent(root.Path() / L"b.txt", "x");
    test::WriteFileContent(root.Path() / L"c.log", "x");

    Panel panel(root.Path());
    panel.SelectAll();
    CHECK(panel.SelectedCount() == 3);

    panel.SelectByMask(L"*.txt", /*select=*/false);

    const Entry* a = FindEntry(panel, L"a.txt");
    const Entry* c = FindEntry(panel, L"c.log");
    CHECK(a != nullptr);
    CHECK(c != nullptr);
    if (a) CHECK(!a->selected);
    if (c) CHECK(c->selected);  // untouched by a mask that doesn't match it
    CHECK(panel.SelectedCount() == 1);
}

TEST_CASE(Panel_SelectByMask_StarMatchesEverythingIncludingDirectories_SEL003) {
    test::TempDir root;
    fs::create_directory(root.Path() / L"subdir");
    test::WriteFileContent(root.Path() / L"a.txt", "x");

    Panel panel(root.Path());
    panel.SelectByMask(L"*", /*select=*/true);

    CHECK(panel.SelectedCount() == 2);  // subdir + a.txt, not ".."
    const Entry* dotdot = FindEntry(panel, L"..");
    CHECK(dotdot != nullptr);
    if (dotdot) CHECK(!dotdot->selected);
}

TEST_CASE(Panel_SelectByMask_SubstringPattern_IsCaseInsensitive_SEL003) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"Report.TXT", "x");
    test::WriteFileContent(root.Path() / L"other.log", "x");

    Panel panel(root.Path());
    panel.SelectByMask(L"report", /*select=*/true);  // no '*'/'?' -> substring match

    const Entry* report = FindEntry(panel, L"Report.TXT");
    const Entry* other = FindEntry(panel, L"other.log");
    CHECK(report != nullptr);
    CHECK(other != nullptr);
    if (report) CHECK(report->selected);
    if (other) CHECK(!other->selected);
}

// --- ToggleEntrySelected / SetEntrySelected / SelectRange (IS-0004) ---------

TEST_CASE(Panel_ToggleEntrySelected_TogglesOnThenOff_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    const Entry* a = FindEntry(panel, L"afile.txt");
    CHECK(a != nullptr);
    int index = a ? static_cast<int>(a - &panel.Entries()[0]) : -1;

    bool nowSelected = panel.ToggleEntrySelected(index);
    CHECK(nowSelected);
    CHECK(panel.Entries()[index].selected);

    bool nowSelectedAgain = panel.ToggleEntrySelected(index);
    CHECK(!nowSelectedAgain);
    CHECK(!panel.Entries()[index].selected);
}

TEST_CASE(Panel_ToggleEntrySelected_NeverSelectsParentEntry_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    CHECK(panel.Entries()[0].name == L"..");

    bool result = panel.ToggleEntrySelected(0);

    CHECK(!result);
    CHECK(!panel.Entries()[0].selected);
}

TEST_CASE(Panel_ToggleEntrySelected_OutOfRangeIndexIsANoOp_IS0004) {
    test::TempDir root;
    Panel panel(root.Path());

    bool result = panel.ToggleEntrySelected(99);

    CHECK(!result);
}

TEST_CASE(Panel_SetEntrySelected_ForcesStateRegardlessOfCurrent_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    const Entry* a = FindEntry(panel, L"afile.txt");
    int index = a ? static_cast<int>(a - &panel.Entries()[0]) : -1;

    panel.SetEntrySelected(index, true);
    CHECK(panel.Entries()[index].selected);
    panel.SetEntrySelected(index, true);  // already true -- stays true, no toggle
    CHECK(panel.Entries()[index].selected);
    panel.SetEntrySelected(index, false);
    CHECK(!panel.Entries()[index].selected);
}

TEST_CASE(Panel_SetEntrySelected_NeverSelectsParentEntry_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    panel.SetEntrySelected(0, true);  // index 0 is ".."

    CHECK(!panel.Entries()[0].selected);
}

TEST_CASE(Panel_SelectRange_SelectsEveryEntryBetweenBothIndicesInclusive_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");
    test::WriteFileContent(root.Path() / L"cfile.txt", "x");
    test::WriteFileContent(root.Path() / L"dfile.txt", "x");

    Panel panel(root.Path());  // entries: "..", a, b, c, d (indices 0-4)
    panel.SelectRange(1, 3);

    CHECK(!panel.Entries()[0].selected);  // ".."
    CHECK(panel.Entries()[1].selected);
    CHECK(panel.Entries()[2].selected);
    CHECK(panel.Entries()[3].selected);
    CHECK(!panel.Entries()[4].selected);  // outside the range
}

TEST_CASE(Panel_SelectRange_ReversedOrder_SelectsTheSameInclusiveRange_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");
    test::WriteFileContent(root.Path() / L"cfile.txt", "x");

    Panel panel(root.Path());  // entries: "..", a, b, c (indices 0-3)
    panel.SelectRange(3, 1);   // higher index first

    CHECK(!panel.Entries()[0].selected);
    CHECK(panel.Entries()[1].selected);
    CHECK(panel.Entries()[2].selected);
    CHECK(panel.Entries()[3].selected);
}

TEST_CASE(Panel_SelectRange_SameIndexTwice_SelectsJustThatOneEntry_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    panel.SelectRange(1, 1);

    CHECK(panel.Entries()[1].selected);
    CHECK(!panel.Entries()[2].selected);
    CHECK(panel.SelectedCount() == 1);
}

TEST_CASE(Panel_SelectRange_LeavesEntriesOutsideTheRangeUntouched_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");
    test::WriteFileContent(root.Path() / L"cfile.txt", "x");

    Panel panel(root.Path());  // entries: "..", a, b, c (indices 0-3)
    panel.SetEntrySelected(3, true);  // pre-select something outside the upcoming range

    panel.SelectRange(1, 1);

    CHECK(panel.Entries()[1].selected);   // newly selected by the range
    CHECK(!panel.Entries()[2].selected);  // untouched, never was selected
    CHECK(panel.Entries()[3].selected);   // untouched, stays selected from before
}

TEST_CASE(Panel_SelectRange_NeverSelectsParentEntry_IS0004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());  // entries: "..", a (indices 0-1)
    panel.SelectRange(0, 1);

    CHECK(!panel.Entries()[0].selected);
    CHECK(panel.Entries()[1].selected);
}

TEST_CASE(Panel_SelectedCount_CountsOnlySelectedEntries_SEL005) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    CHECK(panel.SelectedCount() == 0);

    panel.MoveCursor(1, 10);
    panel.ToggleCursorSelection(10);

    CHECK(panel.SelectedCount() == 1);
}

TEST_CASE(Panel_SelectedSizeBytes_SumsOnlySelectedEntries_SEL005) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "12345");       // 5 bytes
    test::WriteFileContent(root.Path() / L"bfile.txt", "1234567890");  // 10 bytes

    Panel panel(root.Path());
    panel.SelectAll();

    CHECK(panel.SelectedSizeBytes() == 15);
}

TEST_CASE(Panel_SelectionOrCursor_ReturnsSelectedNamesWhenAnyAreSelected_SEL004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    panel.MoveCursor(1, 10);
    panel.ToggleCursorSelection(10);  // selects afile.txt only; cursor now on bfile.txt

    auto names = panel.SelectionOrCursor();

    CHECK(names.size() == 1);
    if (!names.empty()) CHECK(names[0] == L"afile.txt");
}

TEST_CASE(Panel_SelectionOrCursor_FallsBackToCursorEntryWhenNothingSelected_SEL004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");
    test::WriteFileContent(root.Path() / L"bfile.txt", "x");

    Panel panel(root.Path());
    panel.MoveCursor(2, 10);  // cursor -> bfile.txt, nothing selected

    auto names = panel.SelectionOrCursor();

    CHECK(names.size() == 1);
    if (!names.empty()) CHECK(names[0] == L"bfile.txt");
}

TEST_CASE(Panel_SelectionOrCursor_EmptyWhenCursorIsOnParentAndNothingSelected_SEL004) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "x");

    Panel panel(root.Path());
    CHECK(panel.Entries()[panel.Cursor()].name == L"..");  // cursor starts on ".."

    auto names = panel.SelectionOrCursor();

    CHECK(names.empty());
}
