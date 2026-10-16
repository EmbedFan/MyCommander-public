#include "TestFixtures.h"
#include "TestFramework.h"

#include "SecureDelete.h"
#include "Strings.h"

#include <iostream>
#include <windows.h>

namespace fs = std::filesystem;

namespace {

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

}  // namespace

TEST_CASE(SecureDelete_Check_Verdicts_FOP024) {
    using mc::SecureVerdict;
    CHECK(mc::CheckSecureDelete(FILE_ATTRIBUTE_NORMAL).verdict == SecureVerdict::Allowed);
    CHECK(mc::CheckSecureDelete(FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN).verdict == SecureVerdict::Allowed);
    CHECK(mc::CheckSecureDelete(FILE_ATTRIBUTE_READONLY).verdict == SecureVerdict::ReadOnlyNeedsClearing);
    for (uint32_t bad : {static_cast<uint32_t>(FILE_ATTRIBUTE_REPARSE_POINT), static_cast<uint32_t>(FILE_ATTRIBUTE_COMPRESSED),
                         static_cast<uint32_t>(FILE_ATTRIBUTE_SPARSE_FILE), static_cast<uint32_t>(FILE_ATTRIBUTE_ENCRYPTED)}) {
        mc::SecureCheck c = mc::CheckSecureDelete(bad | FILE_ATTRIBUTE_READONLY);
        CHECK(c.verdict == SecureVerdict::Refused);
        CHECK(!c.reason.empty());
    }
}

TEST_CASE(SecureDelete_File_OverwritesThenDeletes_FOP024) {
    test::TempDir dir;
    fs::path f = dir.Path() / L"secret.txt";
    std::string content(150 * 1024, 'S');  // spans three 64 KB chunks
    test::WriteFileContent(f, content);

    int passes = 0;
    bool zerosSeen = false, randomChanged = false, nameChanged = false;
    mc::SecureHooks hooks;
    hooks.afterPass = [&](int pass, const fs::path& current) {
        ++passes;
        nameChanged = current.filename() != f.filename();
        std::string now = test::ReadFileContent(current);
        if (pass == 1) zerosSeen = now.size() == content.size() && now.find_first_not_of('\0') == std::string::npos;
        if (pass == 2) randomChanged = now.size() == content.size() && now != content && now.find_first_not_of('\0') != std::string::npos;
    };
    mc::SecureFileResult r = mc::SecureDeleteFile(f, hooks);
    CHECK(r.status == mc::SecureStatus::Erased);
    CHECK(passes == 2);
    CHECK(zerosSeen);
    CHECK(randomChanged);
    CHECK(nameChanged);
    CHECK(!Exists(f));
    // nothing left in the folder under any name
    CHECK(fs::is_empty(dir.Path()));
}

TEST_CASE(SecureDelete_ReadOnlyFile_ClearedAndErased_FOP024) {
    test::TempDir dir;
    fs::path f = dir.Path() / L"ro.txt";
    test::WriteFileContent(f, "abc");
    SetFileAttributesW(f.c_str(), FILE_ATTRIBUTE_READONLY);
    CHECK(mc::SecureDeleteFile(f, {}).status == mc::SecureStatus::Erased);
    CHECK(!Exists(f));
}

TEST_CASE(SecureDelete_Cancel_KeepsFileUnderOriginalName_FOP024) {
    test::TempDir dir;
    fs::path f = dir.Path() / L"keep.bin";
    test::WriteFileContent(f, std::string(200 * 1024, 'K'));
    int polls = 0;
    mc::SecureHooks hooks;
    hooks.cancel = [&]() { return ++polls > 2; };
    mc::SecureFileResult r = mc::SecureDeleteFile(f, hooks);
    CHECK(r.status == mc::SecureStatus::Cancelled);
    CHECK(r.message.find(L"partially overwritten") != std::wstring::npos);
    CHECK(Exists(f));
    CHECK(fs::file_size(f) == 200 * 1024);
}

TEST_CASE(SecureDelete_EmptyFile_Erased_FOP024) {
    test::TempDir dir;
    fs::path f = dir.Path() / L"empty.txt";
    test::WriteFileContent(f, "");
    CHECK(mc::SecureDeleteFile(f, {}).status == mc::SecureStatus::Erased);
    CHECK(!Exists(f));
}

TEST_CASE(SecureDelete_Tree_FoldersAndRefusals_FOP024) {
    test::TempDir dir;
    fs::path root = dir.Path() / L"tree";
    fs::create_directories(root / L"sub" / L"deep");
    test::WriteFileContent(root / L"a.txt", "aaaa");
    test::WriteFileContent(root / L"sub" / L"b.txt", "bbbb");
    test::WriteFileContent(root / L"sub" / L"deep" / L"c.txt", "cccc");
    int seen = 0;
    mc::SecureTreeOutcome o = mc::SecureDeleteTree(root, {}, [&](const fs::path&) { ++seen; });
    CHECK(o.erased == 3);
    CHECK(seen == 3);
    CHECK(o.problems.empty());
    CHECK(!Exists(root));
}

TEST_CASE(SecureDelete_Tree_JunctionRemovedTargetUntouched_FOP024) {
    test::TempDir dir;
    fs::path target = dir.Path() / L"target";
    fs::create_directories(target);
    test::WriteFileContent(target / L"precious.txt", "keep me");
    fs::path root = dir.Path() / L"root";
    fs::create_directories(root);
    if (!test::CreateJunction(root / L"link", target)) {
        std::wcout << L"  (junction creation unavailable; skipped)\n";
        return;
    }
    mc::SecureTreeOutcome o = mc::SecureDeleteTree(root, {}, nullptr);
    CHECK(o.links == 1);
    CHECK(!Exists(root));
    CHECK(test::ReadFileContent(target / L"precious.txt") == "keep me");
}

TEST_CASE(SecureDelete_Tree_CompressedFileRefusedAndFolderKept_FOP024) {
    test::TempDir dir;
    fs::path root = dir.Path() / L"c";
    fs::create_directories(root);
    fs::path f = root / L"packed.txt";
    test::WriteFileContent(f, std::string(8192, 'z'));
    // Mark compressed; skip if the volume refuses (not NTFS).
    HANDLE h = CreateFileW(f.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, 0, nullptr);
    bool compressed = false;
    if (h != INVALID_HANDLE_VALUE) {
        USHORT fmt = 1;  // COMPRESSION_FORMAT_DEFAULT
        DWORD ret = 0;
        compressed = DeviceIoControl(h, FSCTL_SET_COMPRESSION, &fmt, sizeof(fmt), nullptr, 0, &ret, nullptr) != 0;
        CloseHandle(h);
    }
    if (!compressed || !(GetFileAttributesW(f.c_str()) & FILE_ATTRIBUTE_COMPRESSED)) {
        std::wcout << L"  (compression unavailable; skipped)\n";
        return;
    }
    mc::SecureTreeOutcome o = mc::SecureDeleteTree(root, {}, nullptr);
    CHECK(o.refused == 1);
    CHECK(o.erased == 0);
    CHECK(Exists(f));
    CHECK(Exists(root));
}

TEST_CASE(SecureDelete_ConfirmationText_StatesLimits_FOP024) {
    std::wstring text = std::wstring(kStrSecureDeleteLimitSsd) + kStrSecureDeleteLimitJournal + kStrSecureDeleteLimitCopies +
                       kStrSecureDeleteLimitCaches + kStrSecureDeleteAdvice;
    CHECK(text.find(L"SSD") != std::wstring::npos);
    CHECK(text.find(L"shadow") != std::wstring::npos || text.find(L"Shadow") != std::wstring::npos);
    CHECK(text.find(L"BitLocker") != std::wstring::npos);
}
