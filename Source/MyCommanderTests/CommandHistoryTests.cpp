#include "TestFixtures.h"
#include "TestFramework.h"

#include "CommandHistory.h"

using mc::kMaxCommandHistoryEntries;
using mc::LoadCommandHistoryFrom;
using mc::SaveCommandHistoryTo;

TEST_CASE(CommandHistory_NonexistentFile_IsEmpty_CLI004) {
    test::TempDir dir;
    auto history = LoadCommandHistoryFrom(dir.Path() / L"missing.txt");

    CHECK(history.empty());
}

TEST_CASE(CommandHistory_RoundTripsOrderedCommands_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"nested" / L"history.txt";
    std::vector<std::wstring> saved{L"dir", L"cd ..", L"copy a.txt b.txt"};

    CHECK(SaveCommandHistoryTo(path, saved));
    auto loaded = LoadCommandHistoryFrom(path);

    CHECK(loaded.size() == 3);
    if (loaded.size() == 3) {
        CHECK(loaded[0] == L"dir");
        CHECK(loaded[1] == L"cd ..");
        CHECK(loaded[2] == L"copy a.txt b.txt");
    }
}

TEST_CASE(CommandHistory_HandlesCrLfLineEndings_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"history.txt";
    test::WriteFileContent(path, "dir\r\ncd ..\r\n");

    auto loaded = LoadCommandHistoryFrom(path);

    CHECK(loaded.size() == 2);
    if (loaded.size() == 2) {
        CHECK(loaded[0] == L"dir");
        CHECK(loaded[1] == L"cd ..");
    }
}

TEST_CASE(CommandHistory_SkipsBlankLines_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"history.txt";
    test::WriteFileContent(path, "dir\n\ncd ..\n");

    auto loaded = LoadCommandHistoryFrom(path);

    CHECK(loaded.size() == 2);
}

// CLI-004: a file (or an in-memory list) larger than the cap is trimmed
// down to the most recent entries, both on load and on save, so history
// can't grow without bound across repeated sessions.
TEST_CASE(CommandHistory_LoadTrimsToMostRecentEntries_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"history.txt";
    std::wstring content;
    const size_t total = kMaxCommandHistoryEntries + 10;
    for (size_t i = 0; i < total; ++i) {
        content += L"cmd" + std::to_wstring(i) + L"\n";
    }
    std::wofstream out(path);
    out << content;
    out.close();

    auto loaded = LoadCommandHistoryFrom(path);

    CHECK(loaded.size() == kMaxCommandHistoryEntries);
    if (!loaded.empty()) {
        // The oldest 10 ("cmd0".."cmd9") should have been dropped, keeping the tail.
        CHECK(loaded.front() == L"cmd10");
        CHECK(loaded.back() == (L"cmd" + std::to_wstring(total - 1)));
    }
}

TEST_CASE(CommandHistory_SaveTrimsToMostRecentEntries_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"history.txt";
    std::vector<std::wstring> oversized;
    const size_t total = kMaxCommandHistoryEntries + 5;
    for (size_t i = 0; i < total; ++i) {
        oversized.push_back(L"cmd" + std::to_wstring(i));
    }

    CHECK(SaveCommandHistoryTo(path, oversized));
    auto loaded = LoadCommandHistoryFrom(path);

    CHECK(loaded.size() == kMaxCommandHistoryEntries);
    if (!loaded.empty()) {
        CHECK(loaded.front() == L"cmd5");
        CHECK(loaded.back() == (L"cmd" + std::to_wstring(total - 1)));
    }
}

TEST_CASE(CommandHistory_SaveOverwritesPreviousContent_CLI004) {
    test::TempDir dir;
    auto path = dir.Path() / L"history.txt";

    CHECK(SaveCommandHistoryTo(path, {L"first", L"second", L"third"}));
    CHECK(SaveCommandHistoryTo(path, {L"only"}));
    auto loaded = LoadCommandHistoryFrom(path);

    CHECK(loaded.size() == 1);
    if (!loaded.empty()) CHECK(loaded[0] == L"only");
}

TEST_CASE(CommandHistory_EmptyPathFailsSaveWithoutCrashing_CLI004) {
    CHECK(!SaveCommandHistoryTo(std::filesystem::path{}, {L"anything"}));
}
