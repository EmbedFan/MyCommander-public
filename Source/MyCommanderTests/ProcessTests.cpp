#include "TestFixtures.h"
#include "TestFramework.h"

#include "Process.h"

#include <windows.h>

using mc::DetachedProcessResult;
using mc::QuoteCommandLineArgument;
using mc::RunDetached;

// --- QuoteCommandLineArgument ------------------------------------------------

TEST_CASE(QuoteCommandLineArgument_PlainWord_IsUnquoted) {
    CHECK(QuoteCommandLineArgument(L"plain") == L"plain");
}

TEST_CASE(QuoteCommandLineArgument_ArgumentWithSpace_IsQuoted) {
    CHECK(QuoteCommandLineArgument(L"has space") == L"\"has space\"");
}

TEST_CASE(QuoteCommandLineArgument_EmptyArgument_IsQuoted) {
    // An empty argument must still round-trip as one empty argument, not
    // disappear entirely.
    CHECK(QuoteCommandLineArgument(L"") == L"\"\"");
}

// --- RunDetached (IS-0002) ---------------------------------------------------
// Process.cpp had no dedicated test file before IS-0002 (RunAndWait had zero
// coverage anywhere in the suite) — these cover the new RunDetached entry
// point IS-0002 added for a non-blocking F4 edit.

TEST_CASE(RunDetached_ValidExecutable_StartsAndReturnsImmediately) {
    test::TempDir root;
    // cmd.exe /c exit 0 -- a real, short-lived child process, matching this
    // project's existing precedent of exercising real Win32 process/file
    // APIs in tests rather than mocking them.
    DetachedProcessResult result = RunDetached(L"cmd.exe", {L"/c", L"exit 0"}, root.Path());

    CHECK(result.started);
    CHECK(result.error.empty());
    CHECK(result.processHandle != nullptr);

    if (result.processHandle != nullptr) {
        // A generous real timeout (not the app's own non-blocking 0ms poll,
        // which belongs to main.cpp's PollPendingEdits, not this test) --
        // confirms the process handle genuinely becomes signaled on exit
        // rather than staying perpetually unsignaled due to some handle bug.
        DWORD waitResult = WaitForSingleObject(result.processHandle, 5000);
        CHECK(waitResult == WAIT_OBJECT_0);
        CloseHandle(result.processHandle);
    }
}

TEST_CASE(RunDetached_NonexistentExecutable_FailsWithoutAHandle) {
    test::TempDir root;
    DetachedProcessResult result =
        RunDetached(root.Path().wstring() + L"\\this-does-not-exist.exe", {}, root.Path());

    CHECK(!result.started);
    CHECK(!result.error.empty());
    CHECK(result.processHandle == nullptr);
}
