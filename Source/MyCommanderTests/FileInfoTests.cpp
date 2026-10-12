#include "TestFixtures.h"
#include "TestFramework.h"

#include "FileInfo.h"

#include <windows.h>

using mc::CheckDigitalSignature;
using mc::FileVersionFields;
using mc::IsRunnableExtension;
using mc::ReadFileVersionFields;
using mc::SignatureStatus;
using test::TempDir;
using test::WriteFileContent;

// --- IsRunnableExtension (FEA-0003) ---------------------------------------

TEST_CASE(IsRunnableExtension_ExeDllAndFriends_AreRunnable_FEA0003) {
    CHECK(IsRunnableExtension(L".exe"));
    CHECK(IsRunnableExtension(L".dll"));
    CHECK(IsRunnableExtension(L".ocx"));
    CHECK(IsRunnableExtension(L".sys"));
    CHECK(IsRunnableExtension(L".cpl"));
    CHECK(IsRunnableExtension(L".scr"));
    CHECK(IsRunnableExtension(L".drv"));
}

TEST_CASE(IsRunnableExtension_WithoutLeadingDot_StillMatches_FEA0003) {
    CHECK(IsRunnableExtension(L"exe"));
    CHECK(IsRunnableExtension(L"dll"));
}

TEST_CASE(IsRunnableExtension_IsCaseInsensitive_FEA0003) {
    CHECK(IsRunnableExtension(L".EXE"));
    CHECK(IsRunnableExtension(L".Dll"));
}

TEST_CASE(IsRunnableExtension_OrdinaryDataExtensions_AreNotRunnable_FEA0003) {
    CHECK(!IsRunnableExtension(L".txt"));
    CHECK(!IsRunnableExtension(L".ini"));
    CHECK(!IsRunnableExtension(L".md"));
    CHECK(!IsRunnableExtension(L".bat"));  // a script, not a PE file with VERSIONINFO
}

TEST_CASE(IsRunnableExtension_EmptyExtension_IsNotRunnable_FEA0003) {
    CHECK(!IsRunnableExtension(L""));
}

// --- ReadFileVersionFields (FEA-0003) -------------------------------------

TEST_CASE(ReadFileVersionFields_OrdinaryTextFile_IsUnavailable_FEA0003) {
    TempDir dir;
    auto path = dir.Path() / L"notes.txt";
    WriteFileContent(path, "just some text, no VERSIONINFO resource here");

    FileVersionFields fields = ReadFileVersionFields(path);

    CHECK(!fields.available);
    CHECK(fields.description.empty());
}

TEST_CASE(ReadFileVersionFields_NonexistentPath_IsUnavailable_NoCrash_FEA0003) {
    TempDir dir;
    FileVersionFields fields = ReadFileVersionFields(dir.Path() / L"does-not-exist.exe");
    CHECK(!fields.available);
}

TEST_CASE(ReadFileVersionFields_TheRunningTestExeItself_ReadsItsOwnRealVersionInfo_FEA0003) {
    // The test harness's own exe carries a real VERSIONINFO resource
    // (MyCommanderTests.rc) with a known ProductName -- the same "point it
    // at our own already-known binary" approach Help.cpp's About section
    // effectively relies on, reused here as a test fixture.
    wchar_t selfPath[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    CHECK(len > 0 && len < MAX_PATH);
    if (len == 0 || len >= MAX_PATH) return;

    FileVersionFields fields = ReadFileVersionFields(selfPath);

    CHECK(fields.available);
    CHECK(fields.productName == L"MyCommander");
    CHECK(!fields.fileVersion.empty());
}

// --- CheckDigitalSignature (FEA-0003) -------------------------------------
// The positive (signed-and-trusted) path can't be exercised without a real,
// currently-valid Authenticode-signed binary checked into the repo (fragile
// -- certificates expire) -- matching this session's own established
// precedent for OS-trust-subsystem code that can't be fully driven by an
// automated test (Elevation.h's RunElevated, MyCommanderSetup's
// IShellLinkW). The negative/failure paths below are fully covered.

TEST_CASE(CheckDigitalSignature_OrdinaryUnsignedFile_IsNotSigned_FEA0003) {
    TempDir dir;
    auto path = dir.Path() / L"plain.txt";
    WriteFileContent(path, "not a signed executable");

    auto info = CheckDigitalSignature(path);

    CHECK(info.status == SignatureStatus::NotSigned);
    CHECK(info.signerName.empty());
}

TEST_CASE(CheckDigitalSignature_NonexistentPath_IsCheckFailed_NoCrash_FEA0003) {
    TempDir dir;
    auto info = CheckDigitalSignature(dir.Path() / L"does-not-exist.exe");
    CHECK(info.status == SignatureStatus::CheckFailed);
}
