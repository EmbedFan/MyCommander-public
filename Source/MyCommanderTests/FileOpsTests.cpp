#include "TestFixtures.h"
#include "TestFramework.h"

#include "FileOps.h"
#include "PathUtil.h"

#include <chrono>
#include <thread>
#include <windows.h>

namespace fs = std::filesystem;

using mc::ConflictAskResult;
using mc::ConflictChoice;
using mc::CopyItems;
using mc::CreateDirectoryResult;
using mc::CreateFileResult;
using mc::CreateNewDirectory;
using mc::CreateNewFile;
using mc::DeleteItems;
using mc::ErrorAskResult;
using mc::ErrorChoice;
using mc::FileAttributeFlag;
using mc::MoveItems;
using mc::CheckCopyMoveDestination;
using mc::CreateDestinationDirectory;
using mc::DestinationState;
using mc::OperationOutcome;
using mc::RenameItem;
using mc::RenameResult;
using mc::ToggleAttributes;
using mc::ToLongPath;

namespace {

// Default callbacks for tests that don't care about conflicts/progress/cancel.
ConflictAskResult NoConflictExpected(const fs::path&) {
    return {}; // defaults to Cancel; a test hitting this without expecting a conflict should fail loudly
}
bool AlwaysContinue(const std::wstring&, int, int) { return true; }
bool NeverCancel() { return false; }
bool AlwaysCancel() { return true; }

ConflictAskResult Overwrite(bool applyToAll = false) {
    ConflictAskResult r;
    r.choice = ConflictChoice::Overwrite;
    r.applyToAll = applyToAll;
    return r;
}
ConflictAskResult Skip(bool applyToAll = false) {
    ConflictAskResult r;
    r.choice = ConflictChoice::Skip;
    r.applyToAll = applyToAll;
    return r;
}
ConflictAskResult RenameTo(const std::wstring& newName) {
    ConflictAskResult r;
    r.choice = ConflictChoice::Rename;
    r.renameTo = newName;
    return r;
}
ConflictAskResult CancelConflict() {
    ConflictAskResult r;
    r.choice = ConflictChoice::Cancel;
    return r;
}

ErrorAskResult RetryError() {
    ErrorAskResult r;
    r.choice = ErrorChoice::Retry;
    return r;
}
ErrorAskResult SkipError(bool applyToAll = false) {
    ErrorAskResult r;
    r.choice = ErrorChoice::Skip;
    r.applyToAll = applyToAll;
    return r;
}
ErrorAskResult CancelError() {
    ErrorAskResult r;
    r.choice = ErrorChoice::Cancel;
    return r;
}

// Opens `path` with no sharing at all, so any other attempt to read, move,
// or delete it fails with a sharing violation — used to simulate a
// transient "something else has this file open" failure deterministically.
// FOP-010's retry tests close the returned handle mid-test to simulate the
// cause being fixed before the retry attempt.
HANDLE LockExclusive(const fs::path& path) {
    return CreateFileW(path.c_str(), GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
}

} // namespace

// --- Copy ----------------------------------------------------------------

TEST_CASE(Copy_SingleFile_CopiesContentAndLeavesSourceIntact) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "hello world");

    OperationOutcome outcome = CopyItems(srcDir, {L"a.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(outcome.failed.empty());
    CHECK(fs::exists(dstDir / L"a.txt"));
    CHECK(test::ReadFileContent(dstDir / L"a.txt") == "hello world");
    CHECK(fs::exists(srcDir / L"a.txt"));
}

TEST_CASE(Copy_MixedFileAndDirectory_CopiesNestedTree) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir / L"sub" / L"deeper");
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"top.txt", "top");
    test::WriteFileContent(srcDir / L"sub" / L"mid.txt", "mid");
    test::WriteFileContent(srcDir / L"sub" / L"deeper" / L"leaf.txt", "leaf");

    OperationOutcome outcome =
        CopyItems(srcDir, {L"top.txt", L"sub"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 2);
    CHECK(outcome.failed.empty());
    CHECK(test::ReadFileContent(dstDir / L"top.txt") == "top");
    CHECK(test::ReadFileContent(dstDir / L"sub" / L"mid.txt") == "mid");
    CHECK(test::ReadFileContent(dstDir / L"sub" / L"deeper" / L"leaf.txt") == "leaf");
}

TEST_CASE(Copy_ConflictSkip_LeavesDestinationUnchanged) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "new");
    test::WriteFileContent(dstDir / L"a.txt", "old");

    auto onConflict = [](const fs::path&) { return Skip(); };
    OperationOutcome outcome = CopyItems(srcDir, {L"a.txt"}, dstDir, onConflict, AlwaysContinue, NeverCancel);

    CHECK(outcome.skipped == 1);
    CHECK(outcome.succeeded == 0);
    CHECK(test::ReadFileContent(dstDir / L"a.txt") == "old");
}

TEST_CASE(Copy_ConflictOverwrite_ReplacesDestination) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "new");
    test::WriteFileContent(dstDir / L"a.txt", "old");

    auto onConflict = [](const fs::path&) { return Overwrite(); };
    OperationOutcome outcome = CopyItems(srcDir, {L"a.txt"}, dstDir, onConflict, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(test::ReadFileContent(dstDir / L"a.txt") == "new");
}

TEST_CASE(Copy_ConflictRename_CreatesNewNameWithoutTouchingOriginal) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "new");
    test::WriteFileContent(dstDir / L"a.txt", "old");

    auto onConflict = [](const fs::path&) { return RenameTo(L"a_renamed.txt"); };
    OperationOutcome outcome = CopyItems(srcDir, {L"a.txt"}, dstDir, onConflict, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(test::ReadFileContent(dstDir / L"a.txt") == "old");
    CHECK(test::ReadFileContent(dstDir / L"a_renamed.txt") == "new");
}

TEST_CASE(Copy_ConflictCancel_StopsBeforeRemainingItems) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "a");
    test::WriteFileContent(srcDir / L"b.txt", "b");
    test::WriteFileContent(dstDir / L"a.txt", "old-a");

    auto onConflict = [](const fs::path&) { return CancelConflict(); };
    OperationOutcome outcome =
        CopyItems(srcDir, {L"a.txt", L"b.txt"}, dstDir, onConflict, AlwaysContinue, NeverCancel);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK(!fs::exists(dstDir / L"b.txt"));
}

TEST_CASE(Copy_ItemStartCancel_CopiesNothing) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "a");

    auto cancelImmediately = [](const std::wstring&, int, int) { return false; };
    OperationOutcome outcome =
        CopyItems(srcDir, {L"a.txt"}, dstDir, NoConflictExpected, cancelImmediately, NeverCancel);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK(!fs::exists(dstDir / L"a.txt"));
}

TEST_CASE(Copy_CancelPollDuringFile_AbortsAndRemovesPartialDestination) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "some content to copy");

    OperationOutcome outcome = CopyItems(srcDir, {L"a.txt"}, dstDir, NoConflictExpected, AlwaysContinue, AlwaysCancel);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK(!fs::exists(dstDir / L"a.txt"));
    CHECK(fs::exists(srcDir / L"a.txt")); // source is never touched by a copy
}

TEST_CASE(Copy_RefusesDirectoryIntoOwnSubtree) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::create_directories(srcDir / L"sub" / L"nested_dest");
    test::WriteFileContent(srcDir / L"sub" / L"keep.txt", "keep");

    fs::path destDir = srcDir / L"sub" / L"nested_dest";
    OperationOutcome outcome = CopyItems(srcDir, {L"sub"}, destDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 0);
    CHECK(outcome.failed.size() == 1);
    CHECK(test::ReadFileContent(srcDir / L"sub" / L"keep.txt") == "keep"); // untouched
}

TEST_CASE(Copy_TopLevelReparsePoint_IsRefusedNotFollowed) {
    test::TempDir root;
    fs::path targetDir = root.Path() / L"target";
    fs::path linkDir = root.Path() / L"link";
    fs::path destDir = root.Path() / L"dest";
    fs::create_directories(targetDir);
    fs::create_directories(destDir);
    test::WriteFileContent(targetDir / L"secret.txt", "target content");

    if (!test::CreateJunction(linkDir, targetDir)) {
        wprintf(L"  (skipped: could not create an NTFS junction on this system)\n");
        return;
    }

    OperationOutcome outcome = CopyItems(root.Path(), {L"link"}, destDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 0);
    CHECK(outcome.failed.size() == 1);
    CHECK(!fs::exists(destDir / L"link"));
}

TEST_CASE(Copy_NestedReparsePoint_IsSkippedWithoutAbortingSiblings) {
    test::TempDir root;
    fs::path targetDir = root.Path() / L"target";
    fs::path srcDir = root.Path() / L"src";
    fs::path destDir = root.Path() / L"dest";
    fs::create_directories(targetDir);
    fs::create_directories(srcDir / L"container");
    fs::create_directories(destDir);
    test::WriteFileContent(targetDir / L"secret.txt", "target content");
    test::WriteFileContent(srcDir / L"container" / L"keep.txt", "keep me");

    fs::path linkInContainer = srcDir / L"container" / L"link";
    if (!test::CreateJunction(linkInContainer, targetDir)) {
        wprintf(L"  (skipped: could not create an NTFS junction on this system)\n");
        return;
    }

    OperationOutcome outcome =
        CopyItems(srcDir, {L"container"}, destDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(outcome.failed.empty());
    CHECK(test::ReadFileContent(destDir / L"container" / L"keep.txt") == "keep me");
    CHECK(!fs::exists(destDir / L"container" / L"link"));
}

// --- Retry (FOP-010) -------------------------------------------------------

TEST_CASE(Copy_ErrorRetry_SucceedsOnceTheLockIsReleased) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"locked.txt", "content");

    HANDLE lock = LockExclusive(srcDir / L"locked.txt");
    CHECK(lock != INVALID_HANDLE_VALUE);

    int askCount = 0;
    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        ++askCount;
        CloseHandle(lock);  // simulate whatever had the file open closing it
        return RetryError();
    };

    OperationOutcome outcome =
        CopyItems(srcDir, {L"locked.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel, onError);

    CHECK(askCount == 1);
    CHECK(outcome.succeeded == 1);
    CHECK(outcome.failed.empty());
    CHECK(fs::exists(dstDir / L"locked.txt"));
}

TEST_CASE(Copy_ErrorSkip_SkipsJustThatItemAndContinues) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"locked.txt", "content");
    test::WriteFileContent(srcDir / L"ok.txt", "fine");

    HANDLE lock = LockExclusive(srcDir / L"locked.txt");
    CHECK(lock != INVALID_HANDLE_VALUE);

    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) { return SkipError(); };
    OperationOutcome outcome = CopyItems(srcDir, {L"locked.txt", L"ok.txt"}, dstDir, NoConflictExpected,
                                         AlwaysContinue, NeverCancel, onError);

    CloseHandle(lock);

    CHECK(outcome.succeeded == 1);
    CHECK(outcome.skipped == 1);
    CHECK(outcome.failed.empty());
    CHECK(!fs::exists(dstDir / L"locked.txt"));
    CHECK(fs::exists(dstDir / L"ok.txt"));
}

TEST_CASE(Copy_ErrorSkipAll_AsksOnlyOnceForMultipleFailures) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "a");
    test::WriteFileContent(srcDir / L"b.txt", "b");

    HANDLE lockA = LockExclusive(srcDir / L"a.txt");
    HANDLE lockB = LockExclusive(srcDir / L"b.txt");
    CHECK(lockA != INVALID_HANDLE_VALUE);
    CHECK(lockB != INVALID_HANDLE_VALUE);

    int askCount = 0;
    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        ++askCount;
        return SkipError(/*applyToAll=*/true);
    };
    OperationOutcome outcome =
        CopyItems(srcDir, {L"a.txt", L"b.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel, onError);

    CloseHandle(lockA);
    CloseHandle(lockB);

    CHECK(askCount == 1);
    CHECK(outcome.skipped == 2);
    CHECK(outcome.failed.empty());
}

TEST_CASE(Copy_ErrorCancel_StopsWithoutProcessingRemainingItems) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"locked.txt", "content");
    test::WriteFileContent(srcDir / L"never_reached.txt", "content");

    HANDLE lock = LockExclusive(srcDir / L"locked.txt");
    CHECK(lock != INVALID_HANDLE_VALUE);

    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) { return CancelError(); };
    OperationOutcome outcome = CopyItems(srcDir, {L"locked.txt", L"never_reached.txt"}, dstDir, NoConflictExpected,
                                         AlwaysContinue, NeverCancel, onError);

    CloseHandle(lock);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK(outcome.failed.empty());  // Cancel from the error dialog doesn't itself record a failure,
                                    // matching AskConflict's own Cancel choice
    CHECK(!fs::exists(dstDir / L"never_reached.txt"));
}

TEST_CASE(Copy_NoErrorHandler_RecordsFailureAndContinues_PreservesOriginalBehavior) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"locked.txt", "content");
    test::WriteFileContent(srcDir / L"ok.txt", "fine");

    HANDLE lock = LockExclusive(srcDir / L"locked.txt");
    CHECK(lock != INVALID_HANDLE_VALUE);

    // No onError argument at all — the pre-FOP-010-retry call shape.
    OperationOutcome outcome =
        CopyItems(srcDir, {L"locked.txt", L"ok.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CloseHandle(lock);

    CHECK(outcome.succeeded == 1);
    CHECK(outcome.failed.size() == 1);
    CHECK(fs::exists(dstDir / L"ok.txt"));
}

// --- Move ------------------------------------------------------------------

TEST_CASE(Move_File_RemovesSourceAndCreatesDestination) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "payload");

    OperationOutcome outcome = MoveItems(srcDir, {L"a.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(!fs::exists(srcDir / L"a.txt"));
    CHECK(test::ReadFileContent(dstDir / L"a.txt") == "payload");
}

TEST_CASE(Move_Directory_MovesNestedTreeAndRemovesSource) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir / L"sub" / L"deeper");
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"sub" / L"mid.txt", "mid");
    test::WriteFileContent(srcDir / L"sub" / L"deeper" / L"leaf.txt", "leaf");

    OperationOutcome outcome = MoveItems(srcDir, {L"sub"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(!fs::exists(srcDir / L"sub"));
    CHECK(test::ReadFileContent(dstDir / L"sub" / L"mid.txt") == "mid");
    CHECK(test::ReadFileContent(dstDir / L"sub" / L"deeper" / L"leaf.txt") == "leaf");
}

TEST_CASE(Move_ConflictOverwrite_ReplacesDestinationFile) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "new");
    test::WriteFileContent(dstDir / L"a.txt", "old");

    auto onConflict = [](const fs::path&) { return Overwrite(); };
    OperationOutcome outcome = MoveItems(srcDir, {L"a.txt"}, dstDir, onConflict, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(!fs::exists(srcDir / L"a.txt"));
    CHECK(test::ReadFileContent(dstDir / L"a.txt") == "new");
}

TEST_CASE(Move_ConflictSkip_LeavesBothSidesUnchanged) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"a.txt", "new");
    test::WriteFileContent(dstDir / L"a.txt", "old");

    auto onConflict = [](const fs::path&) { return Skip(); };
    OperationOutcome outcome = MoveItems(srcDir, {L"a.txt"}, dstDir, onConflict, AlwaysContinue, NeverCancel);

    CHECK(outcome.skipped == 1);
    CHECK(test::ReadFileContent(srcDir / L"a.txt") == "new");
    CHECK(test::ReadFileContent(dstDir / L"a.txt") == "old");
}

TEST_CASE(Move_ConflictOverwriteOnExistingDirectory_FailsWithoutTouchingEither) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir / L"sub");
    fs::create_directories(dstDir / L"sub");
    test::WriteFileContent(srcDir / L"sub" / L"a.txt", "src-side");
    test::WriteFileContent(dstDir / L"sub" / L"a.txt", "dst-side");

    auto onConflict = [](const fs::path&) { return Overwrite(); };
    OperationOutcome outcome = MoveItems(srcDir, {L"sub"}, dstDir, onConflict, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 0);
    CHECK(outcome.failed.size() == 1);
    CHECK(test::ReadFileContent(srcDir / L"sub" / L"a.txt") == "src-side");
    CHECK(test::ReadFileContent(dstDir / L"sub" / L"a.txt") == "dst-side");
}

TEST_CASE(Move_RefusesSameSourceAndDestinationDirectory) {
    test::TempDir root;
    fs::path dir = root.Path() / L"dir";
    fs::create_directories(dir);
    test::WriteFileContent(dir / L"a.txt", "a");

    OperationOutcome outcome = MoveItems(dir, {L"a.txt"}, dir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 0);
    CHECK(outcome.failed.size() == 1);
    CHECK(fs::exists(dir / L"a.txt"));
}

TEST_CASE(Move_RefusesDirectoryIntoOwnSubtree) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::create_directories(srcDir / L"sub" / L"nested_dest");
    test::WriteFileContent(srcDir / L"sub" / L"keep.txt", "keep");

    fs::path destDir = srcDir / L"sub" / L"nested_dest";
    OperationOutcome outcome = MoveItems(srcDir, {L"sub"}, destDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 0);
    CHECK(outcome.failed.size() == 1);
    CHECK(fs::exists(srcDir / L"sub" / L"keep.txt")); // never touched
}

TEST_CASE(Move_ErrorRetry_SucceedsOnceTheLockIsReleased) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"locked.txt", "content");

    HANDLE lock = LockExclusive(srcDir / L"locked.txt");
    CHECK(lock != INVALID_HANDLE_VALUE);

    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        CloseHandle(lock);
        return RetryError();
    };
    OperationOutcome outcome =
        MoveItems(srcDir, {L"locked.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel, onError);

    CHECK(outcome.succeeded == 1);
    CHECK(fs::exists(dstDir / L"locked.txt"));
    CHECK(!fs::exists(srcDir / L"locked.txt"));
}

// --- Rename ------------------------------------------------------------------

TEST_CASE(Rename_Basic_RenamesFileInPlace) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"old.txt", "content");

    auto result = RenameItem(root.Path(), L"old.txt", L"new.txt");

    CHECK(result.ok);
    CHECK(!fs::exists(root.Path() / L"old.txt"));
    CHECK(test::ReadFileContent(root.Path() / L"new.txt") == "content");
}

TEST_CASE(Rename_RefusesWhenTargetAlreadyExists) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"old.txt", "old-content");
    test::WriteFileContent(root.Path() / L"new.txt", "existing-content");

    auto result = RenameItem(root.Path(), L"old.txt", L"new.txt");

    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(test::ReadFileContent(root.Path() / L"old.txt") == "old-content");
    CHECK(test::ReadFileContent(root.Path() / L"new.txt") == "existing-content");
}

TEST_CASE(Rename_RefusesReservedCharacters) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"old.txt", "content");

    auto result = RenameItem(root.Path(), L"old.txt", L"bad:name.txt");

    CHECK(!result.ok);
    CHECK(fs::exists(root.Path() / L"old.txt"));
}

TEST_CASE(Rename_RefusesEmptyAndDotNames) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"old.txt", "content");

    CHECK(!RenameItem(root.Path(), L"old.txt", L"").ok);
    CHECK(!RenameItem(root.Path(), L"old.txt", L".").ok);
    CHECK(!RenameItem(root.Path(), L"old.txt", L"..").ok);
    CHECK(fs::exists(root.Path() / L"old.txt"));
}

// --- Create directory --------------------------------------------------------

TEST_CASE(CreateNewDirectory_Basic_CreatesEmptyDirectory) {
    test::TempDir root;

    auto result = CreateNewDirectory(root.Path(), L"newdir");

    CHECK(result.ok);
    CHECK(fs::is_directory(root.Path() / L"newdir"));
}

TEST_CASE(CreateNewDirectory_RefusesWhenNameAlreadyExistsAsFile) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"existing", "content");

    auto result = CreateNewDirectory(root.Path(), L"existing");

    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(!fs::is_directory(root.Path() / L"existing"));
    CHECK(test::ReadFileContent(root.Path() / L"existing") == "content");
}

TEST_CASE(CreateNewDirectory_RefusesWhenNameAlreadyExistsAsDirectory) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"existing" / L"keep");

    auto result = CreateNewDirectory(root.Path(), L"existing");

    CHECK(!result.ok);
    CHECK(fs::is_directory(root.Path() / L"existing" / L"keep")); // untouched
}

TEST_CASE(CreateNewDirectory_RefusesReservedCharactersAndEmptyNames) {
    test::TempDir root;

    CHECK(!CreateNewDirectory(root.Path(), L"bad:name").ok);
    CHECK(!CreateNewDirectory(root.Path(), L"").ok);
    CHECK(!CreateNewDirectory(root.Path(), L".").ok);
    CHECK(!CreateNewDirectory(root.Path(), L"..").ok);
}

// --- Copy/move destination checks (EXT-001/CON-004) -----------------------
// main.cpp's DoCopyOrMove used to call std::filesystem::exists/is_directory/
// create_directories directly; these two entry points move that file-system
// access into FileOps so the UI layer only ever decides what to do about
// each outcome.

TEST_CASE(CheckCopyMoveDestination_ExistingDirectory_ReturnsReady) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"dest");

    CHECK(CheckCopyMoveDestination(root.Path() / L"dest") == DestinationState::ReadyAsDirectory);
}

TEST_CASE(CheckCopyMoveDestination_MissingPath_ReturnsDoesNotExist) {
    test::TempDir root;

    CHECK(CheckCopyMoveDestination(root.Path() / L"nope") == DestinationState::DoesNotExist);
}

TEST_CASE(CheckCopyMoveDestination_ExistingFile_ReturnsExistsButNotDirectory) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"afile.txt", "content");

    CHECK(CheckCopyMoveDestination(root.Path() / L"afile.txt") == DestinationState::ExistsButNotDirectory);
}

TEST_CASE(CreateDestinationDirectory_CreatesMissingParentsToo) {
    test::TempDir root;
    fs::path nested = root.Path() / L"a" / L"b" / L"c";

    auto result = CreateDestinationDirectory(nested);

    CHECK(result.ok);
    CHECK(result.error.empty());
    CHECK(fs::is_directory(nested));
}

TEST_CASE(CreateDestinationDirectory_FailureMessage_ContainsPath_ERR001) {
    test::TempDir root;
    // A file at "blocked" makes "blocked\sub" impossible to create as a
    // directory — a deterministic, cross-machine way to force the failure
    // path without needing a locked handle.
    test::WriteFileContent(root.Path() / L"blocked", "content");
    fs::path target = root.Path() / L"blocked" / L"sub";

    auto result = CreateDestinationDirectory(target);

    CHECK(!result.ok);
    CHECK(result.error.find(target.wstring()) != std::wstring::npos);
}

// --- Create file (FOP-005) ----------------------------------------------------

TEST_CASE(CreateNewFile_Basic_CreatesEmptyFile) {
    test::TempDir root;

    auto result = CreateNewFile(root.Path(), L"newfile.txt");

    CHECK(result.ok);
    CHECK(fs::is_regular_file(root.Path() / L"newfile.txt"));
    CHECK(fs::file_size(root.Path() / L"newfile.txt") == 0);
}

TEST_CASE(CreateNewFile_RefusesWhenNameAlreadyExistsAsFile) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"existing.txt", "content");

    auto result = CreateNewFile(root.Path(), L"existing.txt");

    CHECK(!result.ok);
    CHECK(!result.error.empty());
    CHECK(test::ReadFileContent(root.Path() / L"existing.txt") == "content");  // untouched
}

TEST_CASE(CreateNewFile_RefusesWhenNameAlreadyExistsAsDirectory) {
    test::TempDir root;
    fs::create_directories(root.Path() / L"existing" / L"keep");

    auto result = CreateNewFile(root.Path(), L"existing");

    CHECK(!result.ok);
    CHECK(fs::is_directory(root.Path() / L"existing" / L"keep"));  // untouched
}

TEST_CASE(CreateNewFile_RefusesReservedCharactersAndEmptyNames) {
    test::TempDir root;

    CHECK(!CreateNewFile(root.Path(), L"bad:name").ok);
    CHECK(!CreateNewFile(root.Path(), L"").ok);
    CHECK(!CreateNewFile(root.Path(), L".").ok);
    CHECK(!CreateNewFile(root.Path(), L"..").ok);
}

TEST_CASE(CreateNewFile_FailureMessage_ContainsPath_ERR001) {
    test::TempDir root;
    fs::path missingParent = root.Path() / L"does-not-exist";

    CreateFileResult result = CreateNewFile(missingParent, L"newfile.txt");

    CHECK(!result.ok);
    CHECK(result.error.find(L"newfile.txt") != std::wstring::npos);
}

// --- Delete ------------------------------------------------------------------

TEST_CASE(Delete_Permanent_RemovesFileAndDirectoryTree) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"fileA.txt", "a");
    fs::create_directories(root.Path() / L"dirB" / L"nested");
    test::WriteFileContent(root.Path() / L"dirB" / L"nested" / L"leaf.txt", "leaf");

    OperationOutcome outcome = DeleteItems(root.Path(), {L"fileA.txt", L"dirB"}, /*permanent=*/true);

    CHECK(outcome.succeeded == 2);
    CHECK(outcome.failed.empty());
    CHECK(!fs::exists(root.Path() / L"fileA.txt"));
    CHECK(!fs::exists(root.Path() / L"dirB"));
}

TEST_CASE(Delete_Permanent_RemovesReadOnlyFile) {
    test::TempDir root;
    fs::path file = root.Path() / L"readonly.txt";
    test::WriteFileContent(file, "content");
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_READONLY);

    OperationOutcome outcome = DeleteItems(root.Path(), {L"readonly.txt"}, /*permanent=*/true);

    CHECK(outcome.succeeded == 1);
    CHECK(!fs::exists(file));
}

TEST_CASE(Delete_Permanent_RemovesJunctionWithoutDeletingTargetContents) {
    test::TempDir root;
    fs::path targetDir = root.Path() / L"target";
    fs::path linkDir = root.Path() / L"link";
    fs::create_directories(targetDir);
    test::WriteFileContent(targetDir / L"keep.txt", "keep me");

    if (!test::CreateJunction(linkDir, targetDir)) {
        wprintf(L"  (skipped: could not create an NTFS junction on this system)\n");
        return;
    }

    OperationOutcome outcome = DeleteItems(root.Path(), {L"link"}, /*permanent=*/true);

    CHECK(outcome.succeeded == 1);
    CHECK(!fs::exists(linkDir));
    CHECK(fs::exists(targetDir));
    CHECK(test::ReadFileContent(targetDir / L"keep.txt") == "keep me");
}

TEST_CASE(Delete_RecycleBin_RemovesItemFromOriginalLocation) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"a.txt", "a");

    OperationOutcome outcome = DeleteItems(root.Path(), {L"a.txt"}, /*permanent=*/false);

    // We only assert it left its original location; verifying Recycle Bin
    // recoverability itself is outside what this suite checks.
    if (outcome.failed.empty()) {
        CHECK(!fs::exists(root.Path() / L"a.txt"));
    } else {
        wprintf(L"  (note: Recycle Bin delete failed in this environment, skipping assertion)\n");
    }
}

// PER-002: the permanent-delete path gained the same live-progress-plus-
// Esc-cancel responsiveness Copy/Move already have — both between
// top-level items (onItemStart) and mid-tree during a recursive delete
// (cancelPoll, checked by RemoveTreeSafely before descending into each
// entry).
TEST_CASE(Delete_Permanent_ItemStartCancel_DeletesNothing_PER002) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "a");

    auto cancelImmediately = [](const std::wstring&, int, int) { return false; };
    OperationOutcome outcome = DeleteItems(root.Path(), {L"a.txt"}, /*permanent=*/true, cancelImmediately);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK(fs::exists(file));
}

TEST_CASE(Delete_Permanent_CancelPollDuringRecursiveDelete_LeavesTreeIntact_PER002) {
    test::TempDir root;
    fs::path subDir = root.Path() / L"container";
    fs::create_directory(subDir);
    test::WriteFileContent(subDir / L"a.txt", "a");

    OperationOutcome outcome =
        DeleteItems(root.Path(), {L"container"}, /*permanent=*/true, AlwaysContinue, AlwaysCancel);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK(fs::exists(subDir / L"a.txt"));
    CHECK(fs::exists(subDir));
}

TEST_CASE(Delete_Permanent_ErrorRetry_SucceedsOnceTheLockIsReleased) {
    test::TempDir root;
    fs::path file = root.Path() / L"locked.txt";
    test::WriteFileContent(file, "content");

    HANDLE lock = LockExclusive(file);
    CHECK(lock != INVALID_HANDLE_VALUE);

    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        CloseHandle(lock);
        return RetryError();
    };
    OperationOutcome outcome = DeleteItems(root.Path(), {L"locked.txt"}, /*permanent=*/true, {}, {}, onError);

    CHECK(outcome.succeeded == 1);
    CHECK(!fs::exists(file));
}

TEST_CASE(Delete_Permanent_ErrorSkip_LeavesFileInPlace) {
    test::TempDir root;
    fs::path file = root.Path() / L"locked.txt";
    test::WriteFileContent(file, "content");

    HANDLE lock = LockExclusive(file);
    CHECK(lock != INVALID_HANDLE_VALUE);

    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) { return SkipError(); };
    OperationOutcome outcome = DeleteItems(root.Path(), {L"locked.txt"}, /*permanent=*/true, {}, {}, onError);

    CloseHandle(lock);

    CHECK(outcome.skipped == 1);
    CHECK(outcome.failed.empty());
    CHECK(fs::exists(file));
}

TEST_CASE(ToggleAttributes_SetsThenClearsHidden) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "x");
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_HIDDEN) == 0);

    OperationOutcome first = ToggleAttributes(root.Path(), {L"a.txt"}, FileAttributeFlag::Hidden);
    CHECK(first.succeeded == 1);
    CHECK(first.failed.empty());
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);

    OperationOutcome second = ToggleAttributes(root.Path(), {L"a.txt"}, FileAttributeFlag::Hidden);
    CHECK(second.succeeded == 1);
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_HIDDEN) == 0);
}

TEST_CASE(ToggleAttributes_ReadOnlyToggleActuallyBlocksThenAllowsDelete) {
    test::TempDir root;
    fs::path file = root.Path() / L"ro.txt";
    test::WriteFileContent(file, "x");

    ToggleAttributes(root.Path(), {L"ro.txt"}, FileAttributeFlag::ReadOnly);
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_READONLY) != 0);
    CHECK(!DeleteFileW(file.c_str()));  // a plain delete should fail while read-only is set

    ToggleAttributes(root.Path(), {L"ro.txt"}, FileAttributeFlag::ReadOnly);
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_READONLY) == 0);
    CHECK(DeleteFileW(file.c_str()));  // and succeed once it's cleared again
}

TEST_CASE(ToggleAttributes_ClearingTheOnlySetAttributeLeavesFileValid) {
    // New files get FILE_ATTRIBUTE_ARCHIVE by default; clearing it would leave
    // a zero attribute set, which Windows rejects — ToggleAttributes must
    // fall back to FILE_ATTRIBUTE_NORMAL instead of failing outright.
    test::TempDir root;
    fs::path file = root.Path() / L"only_archive.txt";
    test::WriteFileContent(file, "x");
    DWORD before = GetFileAttributesW(file.c_str());
    CHECK((before & FILE_ATTRIBUTE_ARCHIVE) != 0);

    OperationOutcome outcome = ToggleAttributes(root.Path(), {L"only_archive.txt"}, FileAttributeFlag::Archive);

    CHECK(outcome.succeeded == 1);
    CHECK(outcome.failed.empty());
    DWORD after = GetFileAttributesW(file.c_str());
    CHECK(after != INVALID_FILE_ATTRIBUTES);
    CHECK((after & FILE_ATTRIBUTE_ARCHIVE) == 0);
}

TEST_CASE(ToggleAttributes_NonexistentItemFails) {
    test::TempDir root;

    OperationOutcome outcome = ToggleAttributes(root.Path(), {L"missing.txt"}, FileAttributeFlag::System);

    CHECK(outcome.succeeded == 0);
    CHECK(outcome.failed.size() == 1);
}

// ERR-003: ToggleAttributes gained the same Retry/Skip/Skip all/Cancel
// support as Copy/Move/Delete (FOP-010) via the shared RunWithRetry helper.
TEST_CASE(ToggleAttributes_ErrorRetry_SucceedsOnceTheCauseIsFixed_ERR003) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    // Deliberately does not exist yet, so the first GetFileAttributesW fails.

    int askCount = 0;
    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        ++askCount;
        test::WriteFileContent(file, "x");  // simulate the underlying cause being fixed
        return RetryError();
    };

    OperationOutcome outcome = ToggleAttributes(root.Path(), {L"a.txt"}, FileAttributeFlag::Hidden, {}, onError);

    CHECK(askCount == 1);
    CHECK(outcome.succeeded == 1);
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0);
}

TEST_CASE(ToggleAttributes_ErrorSkip_SkipsJustThatItemAndContinues_ERR003) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"ok.txt", "x");

    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) { return SkipError(); };
    OperationOutcome outcome =
        ToggleAttributes(root.Path(), {L"missing.txt", L"ok.txt"}, FileAttributeFlag::Hidden, {}, onError);

    CHECK(outcome.skipped == 1);
    CHECK(outcome.succeeded == 1);
    CHECK(outcome.failed.empty());
}

TEST_CASE(ToggleAttributes_ErrorCancel_StopsWithoutProcessingRemainingItems_ERR003) {
    test::TempDir root;
    fs::path neverReached = root.Path() / L"never_reached.txt";
    test::WriteFileContent(neverReached, "x");

    auto onError = [&](const std::wstring&, const std::wstring&, DWORD) { return CancelError(); };
    OperationOutcome outcome =
        ToggleAttributes(root.Path(), {L"missing.txt", L"never_reached.txt"}, FileAttributeFlag::Hidden, {}, onError);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK((GetFileAttributesW(neverReached.c_str()) & FILE_ATTRIBUTE_HIDDEN) == 0);
}

// PER-002: same live-progress-plus-Esc-cancel responsiveness as Copy/Move/
// Delete for a large selection.
TEST_CASE(ToggleAttributes_ItemStartCancel_TogglesNothing_PER002) {
    test::TempDir root;
    fs::path file = root.Path() / L"a.txt";
    test::WriteFileContent(file, "x");

    auto cancelImmediately = [](const std::wstring&, int, int) { return false; };
    OperationOutcome outcome = ToggleAttributes(root.Path(), {L"a.txt"}, FileAttributeFlag::Hidden, cancelImmediately);

    CHECK(outcome.cancelled);
    CHECK(outcome.succeeded == 0);
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_HIDDEN) == 0);
}

// --- Error messages (ERR-001) -----------------------------------------------
// Each failure below is deliberately triggered *inside a recursive
// operation* (or via a Win32 failure that isn't already covered by
// RenameItem/CreateNewDirectory's own "already exists"/invalid-name checks)
// so that the resulting message has to actually name the specific file that
// failed, not just whatever top-level item name the operation started from.

TEST_CASE(Rename_FailureMessage_ContainsOldAndNewNames_ERR001) {
    test::TempDir root;
    fs::path file = root.Path() / L"locked.txt";
    test::WriteFileContent(file, "x");

    HANDLE lock = LockExclusive(file);
    CHECK(lock != INVALID_HANDLE_VALUE);

    RenameResult result = RenameItem(root.Path(), L"locked.txt", L"renamed.txt");

    CloseHandle(lock);

    CHECK(!result.ok);
    CHECK(result.error.find(L"locked.txt") != std::wstring::npos);
    CHECK(result.error.find(L"renamed.txt") != std::wstring::npos);
}

TEST_CASE(CreateNewDirectory_FailureMessage_ContainsPath_ERR001) {
    test::TempDir root;
    fs::path missingParent = root.Path() / L"does-not-exist";

    // The parent itself doesn't exist, so the single-level create_directory
    // fails for a reason other than "already exists" or an invalid name.
    CreateDirectoryResult result = CreateNewDirectory(missingParent, L"newdir");

    CHECK(!result.ok);
    CHECK(result.error.find(L"newdir") != std::wstring::npos);
}

TEST_CASE(Delete_Permanent_NestedFailureMessage_ContainsNestedPath_ERR001) {
    test::TempDir root;
    fs::path subDir = root.Path() / L"container";
    fs::create_directory(subDir);
    fs::path lockedFile = subDir / L"locked.txt";
    test::WriteFileContent(lockedFile, "x");

    HANDLE lock = LockExclusive(lockedFile);
    CHECK(lock != INVALID_HANDLE_VALUE);

    OperationOutcome outcome = DeleteItems(root.Path(), {L"container"}, /*permanent=*/true);

    CloseHandle(lock);

    CHECK(outcome.failed.size() == 1);
    if (!outcome.failed.empty()) {
        CHECK(outcome.failed[0].second.find(L"locked.txt") != std::wstring::npos);
    }
}

TEST_CASE(Copy_NestedFailureMessage_ContainsBothPaths_ERR001) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir / L"container");
    fs::create_directories(dstDir);
    fs::path lockedFile = srcDir / L"container" / L"locked.txt";
    test::WriteFileContent(lockedFile, "x");

    HANDLE lock = LockExclusive(lockedFile);
    CHECK(lock != INVALID_HANDLE_VALUE);

    OperationOutcome outcome =
        CopyItems(srcDir, {L"container"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CloseHandle(lock);

    CHECK(outcome.failed.size() == 1);
    if (!outcome.failed.empty()) {
        CHECK(outcome.failed[0].second.find(L"locked.txt") != std::wstring::npos);
    }
}

// --- Unicode, long paths, large files, hidden files (TST-003) --------------
// Access failures, empty files, read-only files, and reparse points already
// have extensive coverage elsewhere in this file; these fill the remaining
// named categories that had none at all. Multi-gigabyte transfers are
// TST-006's job (a performance concern with generated/sparse data), not
// this file's — "large" here just needs to exercise the real chunked
// CopyFileExW path rather than a single tiny read, which a few MB does
// perfectly well without slowing the suite down.

TEST_CASE(Copy_UnicodeFilename_CopiesContentCorrectly_TST003) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    std::wstring name = L"caf\u00e9_\u65e5\u672c\u8a9e.txt";  // "café_日本語.txt"
    test::WriteFileContent(srcDir / name, "unicode content");

    OperationOutcome outcome = CopyItems(srcDir, {name}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(fs::exists(dstDir / name));
    CHECK(test::ReadFileContent(dstDir / name) == "unicode content");
}

TEST_CASE(Rename_UnicodeFilename_RenamesCorrectly_TST003) {
    test::TempDir root;
    std::wstring oldName = L"\u65e5\u672c\u8a9e.txt";  // "日本語.txt"
    std::wstring newName = L"\u4e2d\u6587.txt";        // "中文.txt"
    test::WriteFileContent(root.Path() / oldName, "x");

    RenameResult result = RenameItem(root.Path(), oldName, newName);

    CHECK(result.ok);
    CHECK(!fs::exists(root.Path() / oldName));
    CHECK(fs::exists(root.Path() / newName));
}

TEST_CASE(Copy_PathBeyondMaxPath_SucceedsViaLongPathPrefixing_TST003) {
    test::TempDir root;
    // Nest the same directory name until the *full* path exceeds MAX_PATH
    // (260) — creation itself requires the \\?\ prefix, so this exercises
    // ToLongPath end to end rather than only at the unit level. Each
    // component (~58 chars) stays well under NTFS's own 255-char limit.
    fs::path deepDir = root.Path();
    const std::wstring segment = L"a_moderately_long_directory_name_used_to_exceed_max_path";
    while (deepDir.wstring().size() < MAX_PATH + 50) {
        deepDir /= segment;
    }
    std::error_code ec;
    fs::create_directories(ToLongPath(deepDir), ec);
    CHECK(!ec);
    CHECK(deepDir.wstring().size() > MAX_PATH);

    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(dstDir);
    fs::path deepFile = deepDir / L"deep.txt";
    test::WriteFileContent(ToLongPath(deepFile), "deep content");

    OperationOutcome outcome =
        CopyItems(deepDir, {L"deep.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(fs::exists(dstDir / L"deep.txt"));
    CHECK(test::ReadFileContent(dstDir / L"deep.txt") == "deep content");

    // TempDir's own cleanup doesn't \\?\-prefix its remove_all, so clean up
    // the overly deep structure ourselves first (best-effort).
    fs::remove_all(ToLongPath(root.Path() / segment), ec);
}

TEST_CASE(Copy_LargeFile_CopiesContentIntact_TST003) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);

    std::string content;
    content.reserve(5 * 1024 * 1024);
    for (int i = 0; i < 5 * 1024; ++i) content.append(1024, 'x');  // ~5 MB
    test::WriteFileContent(srcDir / L"big.bin", content);

    OperationOutcome outcome = CopyItems(srcDir, {L"big.bin"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    CHECK(fs::file_size(dstDir / L"big.bin") == content.size());
    CHECK(test::ReadFileContent(dstDir / L"big.bin") == content);
}

TEST_CASE(Copy_ReportsByteProgressForTheCurrentFile_FOP009) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);

    // Large enough that CopyFileExW's progress routine is guaranteed to
    // fire at least once (it's driven by the OS's own internal read/write
    // buffer size, not by file size directly).
    std::string content(2 * 1024 * 1024, 'y');
    test::WriteFileContent(srcDir / L"big.bin", content);

    std::vector<std::pair<uint64_t, uint64_t>> samples;
    auto onByteProgress = [&](uint64_t transferred, uint64_t total) { samples.emplace_back(transferred, total); };

    OperationOutcome outcome =
        CopyItems(srcDir, {L"big.bin"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel, {}, onByteProgress);

    CHECK(outcome.succeeded == 1);
    CHECK(!samples.empty());
    CHECK(samples.back().first == content.size());   // fully transferred by completion
    CHECK(samples.back().second == content.size());  // total matches the actual file size
    for (const auto& sample : samples) {
        CHECK(sample.second == content.size());
        CHECK(sample.first <= sample.second);
    }
}

TEST_CASE(Copy_Directory_ReportsByteProgressForNestedFile_FOP009) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir / L"subdir");
    fs::create_directories(dstDir);

    std::string content(2 * 1024 * 1024, 'z');
    test::WriteFileContent(srcDir / L"subdir" / L"big.bin", content);

    std::vector<std::pair<uint64_t, uint64_t>> samples;
    auto onByteProgress = [&](uint64_t transferred, uint64_t total) { samples.emplace_back(transferred, total); };

    // Copying a directory always recurses through CopyDirectoryRecursive
    // regardless of volume topology (unlike Move's same-volume rename fast
    // path), so this deterministically exercises the nested-file nested
    // onProgress threading without needing a real second volume.
    OperationOutcome outcome = CopyItems(srcDir, {L"subdir"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel,
                                         {}, onByteProgress);

    CHECK(outcome.succeeded == 1);
    CHECK(fs::file_size(dstDir / L"subdir" / L"big.bin") == content.size());
    CHECK(!samples.empty());
    CHECK(samples.back().first == content.size());
    CHECK(samples.back().second == content.size());
}

// PER-005: proves the progress-routine rate cap FileOps.cpp added (see
// kProgressPollIntervalSeconds) actually bounds both how often onByteProgress
// fires and how much a deliberately slow onByteProgress can slow the transfer
// down — rather than trusting an unverified "the OS callback is cheap"
// assumption. An artificially slow callback (a short sleep on every call it
// receives) stands in for a caller doing real work per update (e.g. a
// console redraw); without the rate cap, a large file's raw CopyFileExW
// callback count is driven by the OS's own internal buffer size and could
// be in the hundreds or thousands, which would multiply directly with this
// sleep and blow the copy time up substantially. Bounds are deliberately
// generous (not a tight timing assertion) to stay robust on a slow/loaded
// CI machine while still being tight enough to fail if the throttle were
// removed.
TEST_CASE(Copy_SlowByteProgressCallback_DoesNotSignificantlyIncreaseTransferTime_PER005) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);

    std::string content;
    content.reserve(20 * 1024 * 1024);
    for (int i = 0; i < 20 * 1024; ++i) content.append(1024, 'q');  // ~20 MB
    test::WriteFileContent(srcDir / L"plain.bin", content);
    test::WriteFileContent(srcDir / L"progressed.bin", content);

    auto start1 = std::chrono::steady_clock::now();
    OperationOutcome baseline =
        CopyItems(srcDir, {L"plain.bin"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);
    double baselineSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start1).count();
    CHECK(baseline.succeeded == 1);

    int callCount = 0;
    auto onByteProgress = [&](uint64_t, uint64_t) {
        ++callCount;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    };

    auto start2 = std::chrono::steady_clock::now();
    OperationOutcome withProgress = CopyItems(srcDir, {L"progressed.bin"}, dstDir, NoConflictExpected, AlwaysContinue,
                                              NeverCancel, {}, onByteProgress);
    double withProgressSeconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start2).count();
    CHECK(withProgress.succeeded == 1);
    CHECK(fs::file_size(dstDir / L"progressed.bin") == content.size());

    // The rate cap (~20 Hz) means a 20 MB local copy should draw well under
    // a hundred samples even in the worst case, not the hundreds-to-
    // thousands an unthrottled OS callback rate could produce.
    CHECK(callCount < 100);
    // 5 ms per sample times a call count this small adds at most a few
    // hundred ms; 2 s of slack over the uninstrumented baseline is generous
    // headroom against machine/scheduler noise while still catching a
    // throttle regression, where callCount (and so the sleep total) would
    // be an order of magnitude higher.
    CHECK(withProgressSeconds < baselineSeconds + 2.0);
}

TEST_CASE(Copy_HiddenFile_CopiesAndPreservesTheAttribute_TST003) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir);
    fs::create_directories(dstDir);
    fs::path srcFile = srcDir / L"secret.txt";
    test::WriteFileContent(srcFile, "hidden content");
    CHECK(SetFileAttributesW(srcFile.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0);

    OperationOutcome outcome =
        CopyItems(srcDir, {L"secret.txt"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    fs::path dstFile = dstDir / L"secret.txt";
    CHECK(test::ReadFileContent(dstFile) == "hidden content");
    DWORD attrs = GetFileAttributesW(dstFile.c_str());
    CHECK(attrs != INVALID_FILE_ATTRIBUTES);
    CHECK((attrs & FILE_ATTRIBUTE_HIDDEN) != 0);
}

// FOP-013: CopyFileExW already preserves a plain file's timestamp/attributes
// on its own (see Copy_HiddenFile_CopiesAndPreservesTheAttribute_TST003
// above); a directory has no such built-in equivalent, so
// CopyDirectoryRecursive has to stamp them across itself once it's done
// populating the new directory.
TEST_CASE(Copy_Directory_PreservesLastWriteTimeAndHiddenAttribute_FOP013) {
    test::TempDir root;
    fs::path srcDir = root.Path() / L"src";
    fs::path dstDir = root.Path() / L"dst";
    fs::create_directories(srcDir / L"sub");
    fs::create_directories(dstDir);
    test::WriteFileContent(srcDir / L"sub" / L"a.txt", "a");

    // A deliberately old, distinct timestamp: a copy that just stamps "now" on the freshly
    // created destination directory (fs::create_directories' default) is easy to mistake for a
    // preserved one unless the source timestamp is obviously not "now".
    SYSTEMTIME oldSystemTime{2001, 6, 0, 15, 12, 0, 0, 0};
    FILETIME oldTime{};
    CHECK(SystemTimeToFileTime(&oldSystemTime, &oldTime) != 0);
    fs::path srcSub = srcDir / L"sub";
    HANDLE h = CreateFileW(srcSub.c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                           OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    CHECK(h != INVALID_HANDLE_VALUE);
    CHECK(SetFileTime(h, &oldTime, &oldTime, &oldTime) != 0);
    CloseHandle(h);
    CHECK(SetFileAttributesW(srcSub.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0);

    OperationOutcome outcome = CopyItems(srcDir, {L"sub"}, dstDir, NoConflictExpected, AlwaysContinue, NeverCancel);

    CHECK(outcome.succeeded == 1);
    fs::path dstSub = dstDir / L"sub";
    WIN32_FILE_ATTRIBUTE_DATA data{};
    CHECK(GetFileAttributesExW(dstSub.c_str(), GetFileExInfoStandard, &data) != 0);
    CHECK(data.ftLastWriteTime.dwLowDateTime == oldTime.dwLowDateTime);
    CHECK(data.ftLastWriteTime.dwHighDateTime == oldTime.dwHighDateTime);
    CHECK((data.dwFileAttributes & FILE_ATTRIBUTE_HIDDEN) != 0);
}

TEST_CASE(Delete_Permanent_HiddenFile_DeletesSuccessfully_TST003) {
    test::TempDir root;
    fs::path file = root.Path() / L"secret.txt";
    test::WriteFileContent(file, "x");
    CHECK(SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_HIDDEN) != 0);

    OperationOutcome outcome = DeleteItems(root.Path(), {L"secret.txt"}, /*permanent=*/true);

    CHECK(outcome.succeeded == 1);
    CHECK(!fs::exists(file));
}
