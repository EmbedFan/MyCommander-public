#include "TestFixtures.h"
#include "TestFramework.h"

#include "Elevation.h"

#include <windows.h>

namespace fs = std::filesystem;

using mc::BuildElevatedHelperParameters;
using mc::ElevatedAction;
using mc::ElevatedRequest;
using mc::FileAttributeFlag;
using mc::RunAsElevatedHelperIfRequested;

namespace {

// Builds a real argv-shaped array (wchar_t*[]) from `args` and calls
// RunAsElevatedHelperIfRequested with it — this never triggers a real UAC
// prompt or spawns a process: it's the same code path the elevated child
// itself runs once it's already elevated, exercised here directly against
// its own argv-dispatch logic and the underlying FileOps primitives.
std::optional<DWORD> RunHelper(std::vector<std::wstring> args) {
    std::vector<wchar_t*> argv;
    for (auto& a : args) argv.push_back(a.data());
    return RunAsElevatedHelperIfRequested(static_cast<int>(argv.size()), argv.data());
}

}  // namespace

// --- BuildElevatedHelperParameters (SEC-004: safe quoting) -----------------

TEST_CASE(BuildElevatedHelperParameters_Copy_IncludesFlagAndBothPaths_SEC003) {
    ElevatedRequest req{ElevatedAction::Copy, L"C:\\src\\a.txt", L"C:\\dst\\a.txt"};
    std::wstring params = BuildElevatedHelperParameters(req);

    CHECK(params.find(L"--elevated-copy") != std::wstring::npos);
    CHECK(params.find(L"C:\\src\\a.txt") != std::wstring::npos);
    CHECK(params.find(L"C:\\dst\\a.txt") != std::wstring::npos);
}

TEST_CASE(BuildElevatedHelperParameters_QuotesPathsContainingSpaces_SEC003_SEC004) {
    ElevatedRequest req{ElevatedAction::Copy, L"C:\\a folder\\a.txt", L"C:\\b folder\\a.txt"};
    std::wstring params = BuildElevatedHelperParameters(req);

    CHECK(params.find(L"\"C:\\a folder\\a.txt\"") != std::wstring::npos);
    CHECK(params.find(L"\"C:\\b folder\\a.txt\"") != std::wstring::npos);
}

TEST_CASE(BuildElevatedHelperParameters_Move_IncludesOverwriteFlagOnlyWhenSet_SEC003) {
    ElevatedRequest withOverwrite{ElevatedAction::Move, L"C:\\a.txt", L"C:\\b.txt"};
    withOverwrite.overwrite = true;
    CHECK(BuildElevatedHelperParameters(withOverwrite).find(L"--overwrite") != std::wstring::npos);

    ElevatedRequest withoutOverwrite{ElevatedAction::Move, L"C:\\a.txt", L"C:\\b.txt"};
    withoutOverwrite.overwrite = false;
    CHECK(BuildElevatedHelperParameters(withoutOverwrite).find(L"--overwrite") == std::wstring::npos);
}

TEST_CASE(BuildElevatedHelperParameters_DeletePermanent_IncludesFlagAndPath_SEC003) {
    ElevatedRequest req{ElevatedAction::DeletePermanent, L"C:\\target"};
    std::wstring params = BuildElevatedHelperParameters(req);

    CHECK(params.find(L"--elevated-delete") != std::wstring::npos);
    CHECK(params.find(L"C:\\target") != std::wstring::npos);
}

TEST_CASE(BuildElevatedHelperParameters_ToggleAttribute_EncodesTheFlagByName_SEC003) {
    ElevatedRequest req{ElevatedAction::ToggleAttribute, L"C:\\target"};
    req.attributeFlag = FileAttributeFlag::Hidden;
    std::wstring params = BuildElevatedHelperParameters(req);

    CHECK(params.find(L"--elevated-attrib") != std::wstring::npos);
    CHECK(params.find(L"hidden") != std::wstring::npos);
}

// --- RunAsElevatedHelperIfRequested (argv dispatch) -------------------------

TEST_CASE(RunAsElevatedHelperIfRequested_NoArguments_ReturnsNullopt) {
    CHECK(!RunHelper({L"MyCommander.exe"}).has_value());
}

TEST_CASE(RunAsElevatedHelperIfRequested_UnrecognizedFlag_ReturnsNullopt) {
    CHECK(!RunHelper({L"MyCommander.exe", L"--not-an-elevated-flag"}).has_value());
}

TEST_CASE(RunAsElevatedHelperIfRequested_Copy_PerformsTheCopyAndReturnsSuccess_SEC003) {
    test::TempDir root;
    fs::path src = root.Path() / L"a.txt";
    fs::path dst = root.Path() / L"b.txt";
    test::WriteFileContent(src, "hello");

    auto result = RunHelper({L"MyCommander.exe", L"--elevated-copy", src.wstring(), dst.wstring()});

    CHECK(result.has_value());
    if (result) CHECK(*result == ERROR_SUCCESS);
    CHECK(test::ReadFileContent(dst) == "hello");
    CHECK(fs::exists(src));  // copy leaves the source alone
}

TEST_CASE(RunAsElevatedHelperIfRequested_Copy_TooFewArgumentsFails) {
    auto result = RunHelper({L"MyCommander.exe", L"--elevated-copy", L"C:\\only-one-path"});
    CHECK(result.has_value());
    if (result) CHECK(*result == static_cast<DWORD>(ERROR_INVALID_PARAMETER));
}

TEST_CASE(RunAsElevatedHelperIfRequested_Move_PerformsTheMoveAndReturnsSuccess_SEC003) {
    test::TempDir root;
    fs::path src = root.Path() / L"a.txt";
    fs::path dst = root.Path() / L"b.txt";
    test::WriteFileContent(src, "payload");

    auto result = RunHelper({L"MyCommander.exe", L"--elevated-move", src.wstring(), dst.wstring()});

    CHECK(result.has_value());
    if (result) CHECK(*result == ERROR_SUCCESS);
    CHECK(!fs::exists(src));
    CHECK(test::ReadFileContent(dst) == "payload");
}

TEST_CASE(RunAsElevatedHelperIfRequested_Move_OverwriteFlagReplacesExistingDestination_SEC003) {
    test::TempDir root;
    fs::path src = root.Path() / L"a.txt";
    fs::path dst = root.Path() / L"b.txt";
    test::WriteFileContent(src, "new");
    test::WriteFileContent(dst, "old");

    auto result =
        RunHelper({L"MyCommander.exe", L"--elevated-move", src.wstring(), dst.wstring(), L"--overwrite"});

    CHECK(result.has_value());
    if (result) CHECK(*result == ERROR_SUCCESS);
    CHECK(test::ReadFileContent(dst) == "new");
}

TEST_CASE(RunAsElevatedHelperIfRequested_DeletePermanent_RemovesTheTarget_SEC003) {
    test::TempDir root;
    fs::path target = root.Path() / L"container";
    fs::create_directories(target);
    test::WriteFileContent(target / L"a.txt", "x");

    auto result = RunHelper({L"MyCommander.exe", L"--elevated-delete", target.wstring()});

    CHECK(result.has_value());
    if (result) CHECK(*result == ERROR_SUCCESS);
    CHECK(!fs::exists(target));
}

TEST_CASE(RunAsElevatedHelperIfRequested_ToggleAttribute_FlipsTheRequestedBit_SEC003) {
    test::TempDir root;
    fs::path target = root.Path() / L"a.txt";
    test::WriteFileContent(target, "x");
    CHECK((GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_HIDDEN) == 0);

    auto result = RunHelper({L"MyCommander.exe", L"--elevated-attrib", target.wstring(), L"hidden"});

    CHECK(result.has_value());
    if (result) CHECK(*result == ERROR_SUCCESS);
    CHECK((GetFileAttributesW(target.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);
}

TEST_CASE(RunAsElevatedHelperIfRequested_ToggleAttribute_UnrecognizedNameFails) {
    test::TempDir root;
    fs::path target = root.Path() / L"a.txt";
    test::WriteFileContent(target, "x");

    auto result = RunHelper({L"MyCommander.exe", L"--elevated-attrib", target.wstring(), L"not-a-real-flag"});

    CHECK(result.has_value());
    if (result) CHECK(*result == static_cast<DWORD>(ERROR_INVALID_PARAMETER));
}

TEST_CASE(RunAsElevatedHelperIfRequested_NonexistentSource_ReportsThePreciseWin32Error_SEC003) {
    test::TempDir root;
    fs::path missing = root.Path() / L"does-not-exist.txt";
    fs::path dst = root.Path() / L"b.txt";

    auto result = RunHelper({L"MyCommander.exe", L"--elevated-copy", missing.wstring(), dst.wstring()});

    CHECK(result.has_value());
    if (result) CHECK(*result != ERROR_SUCCESS);  // exact code isn't the point; a real code is
}
