#include "TestFixtures.h"
#include "TestFramework.h"

#include "UndoJournal.h"

#include <windows.h>

namespace fs = std::filesystem;

namespace {

// A stand-in Recycle Bin: moves the path into a "bin" folder so tests never
// touch the real one (TST-004).
mc::UndoActions FakeBin(const fs::path& bin, int* calls = nullptr) {
    mc::UndoActions a;
    a.recycle = [bin, calls](const fs::path& p, std::wstring& error) {
        if (calls) ++*calls;
        std::error_code ec;
        fs::create_directories(bin, ec);
        fs::rename(p, bin / p.filename(), ec);
        if (ec) error = L"fake bin failed";
        return !ec;
    };
    return a;
}

mc::UndoEntry Entry(mc::UndoKind kind, std::vector<mc::UndoItem> items, std::wstring text = L"test") {
    mc::UndoEntry e;
    e.kind = kind;
    e.description = std::move(text);
    e.items = std::move(items);
    return e;
}

}  // namespace

TEST_CASE(UndoJournal_PushPopOrderAndCap_FOP022) {
    mc::UndoJournal j;
    CHECK(j.Empty() && j.Top() == nullptr);
    for (int i = 0; i < 25; ++i) j.Push(Entry(mc::UndoKind::NewFile, {}, std::to_wstring(i)));
    CHECK(j.Size() == mc::UndoJournal::kCapacity);
    CHECK(j.Top()->description == L"24");
    j.Pop();
    CHECK(j.Top()->description == L"23");
    j.Clear();
    CHECK(j.Empty());
}

TEST_CASE(Undo_Rename_RoundTrip_FOP022) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "alpha");
    fs::rename(dir.Path() / L"a.txt", dir.Path() / L"b.txt");
    auto entry = Entry(mc::UndoKind::Rename, {mc::MakeMovedItem(dir.Path() / L"a.txt", dir.Path() / L"b.txt")});
    mc::UndoResult r = mc::PerformUndo(entry, {});
    CHECK(r.ok && r.reversed == 1);
    CHECK(fs::exists(dir.Path() / L"a.txt") && !fs::exists(dir.Path() / L"b.txt"));
    CHECK(test::ReadFileContent(dir.Path() / L"a.txt") == "alpha");
}

TEST_CASE(Undo_Rename_RefusedWhenTheFileWasEditedAfterwards_FOP022) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "alpha");
    fs::rename(dir.Path() / L"a.txt", dir.Path() / L"b.txt");
    auto entry = Entry(mc::UndoKind::Rename, {mc::MakeMovedItem(dir.Path() / L"a.txt", dir.Path() / L"b.txt")});
    test::WriteFileContent(dir.Path() / L"b.txt", "alpha, and more text now");
    mc::UndoResult r = mc::PerformUndo(entry, {});
    CHECK(!r.ok && r.reversed == 0 && !r.error.empty());
    CHECK(fs::exists(dir.Path() / L"b.txt") && !fs::exists(dir.Path() / L"a.txt"));  // nothing was touched
}

TEST_CASE(Undo_Rename_RefusedWhenTheOriginalNameIsTakenAgain_FOP022) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "alpha");
    fs::rename(dir.Path() / L"a.txt", dir.Path() / L"b.txt");
    auto entry = Entry(mc::UndoKind::Rename, {mc::MakeMovedItem(dir.Path() / L"a.txt", dir.Path() / L"b.txt")});
    test::WriteFileContent(dir.Path() / L"a.txt", "someone else");
    mc::UndoCheck c = mc::CheckUndo(entry);
    CHECK(!c.ok && !c.problem.empty());
    CHECK(test::ReadFileContent(dir.Path() / L"a.txt") == "someone else");
}

TEST_CASE(Undo_Move_RoundTripIncludingAFolder_FOP022) {
    test::TempDir dir;
    fs::create_directories(dir.Path() / L"from" / L"sub");
    fs::create_directories(dir.Path() / L"to");
    test::WriteFileContent(dir.Path() / L"from" / L"sub" / L"x.txt", "x");
    fs::rename(dir.Path() / L"from" / L"sub", dir.Path() / L"to" / L"sub");
    auto entry = Entry(mc::UndoKind::Move,
                       {mc::MakeMovedItem(dir.Path() / L"from" / L"sub", dir.Path() / L"to" / L"sub")});
    mc::UndoResult r = mc::PerformUndo(entry, {});
    CHECK(r.ok);
    CHECK(test::ReadFileContent(dir.Path() / L"from" / L"sub" / L"x.txt") == "x");
    CHECK(!fs::exists(dir.Path() / L"to" / L"sub"));
}

TEST_CASE(Undo_Move_RefusedWhenAFileInsideTheFolderChanged_FOP022) {
    test::TempDir dir;
    fs::create_directories(dir.Path() / L"to" / L"sub");
    fs::create_directories(dir.Path() / L"from");
    test::WriteFileContent(dir.Path() / L"to" / L"sub" / L"x.txt", "x");
    auto entry = Entry(mc::UndoKind::Move,
                       {mc::MakeMovedItem(dir.Path() / L"from" / L"sub", dir.Path() / L"to" / L"sub")});
    test::WriteFileContent(dir.Path() / L"to" / L"sub" / L"y.txt", "new file inside");
    CHECK(!mc::CheckUndo(entry).ok);
}

TEST_CASE(Undo_Copy_SendsTheCopiesToTheBinOnlyWhenUnchanged_FOP022) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "alpha");
    test::WriteFileContent(dir.Path() / L"a - copy.txt", "alpha");
    auto entry = Entry(mc::UndoKind::Copy,
                       {mc::MakeCreatedItem(dir.Path() / L"a.txt", dir.Path() / L"a - copy.txt")});
    int calls = 0;
    mc::UndoResult r = mc::PerformUndo(entry, FakeBin(dir.Path() / L"bin", &calls));
    CHECK(r.ok && calls == 1);
    CHECK(!fs::exists(dir.Path() / L"a - copy.txt") && fs::exists(dir.Path() / L"bin" / L"a - copy.txt"));
    CHECK(fs::exists(dir.Path() / L"a.txt"));  // the source stays

    test::WriteFileContent(dir.Path() / L"c.txt", "c");
    auto edited = Entry(mc::UndoKind::Copy, {mc::MakeCreatedItem(dir.Path() / L"a.txt", dir.Path() / L"c.txt")});
    test::WriteFileContent(dir.Path() / L"c.txt", "edited later, longer");
    calls = 0;
    r = mc::PerformUndo(edited, FakeBin(dir.Path() / L"bin", &calls));
    CHECK(!r.ok && calls == 0 && fs::exists(dir.Path() / L"c.txt"));
}

TEST_CASE(Undo_Attributes_RestoreTheRecordedBits_FOP022) {
    test::TempDir dir;
    auto file = dir.Path() / L"f.txt";
    test::WriteFileContent(file, "f");
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_ARCHIVE);
    uint32_t before = GetFileAttributesW(file.c_str());
    SetFileAttributesW(file.c_str(), before | FILE_ATTRIBUTE_READONLY);
    auto entry = Entry(mc::UndoKind::Attributes, {mc::MakeAttributeItem(file, before)});
    mc::UndoResult r = mc::PerformUndo(entry, {});
    CHECK(r.ok);
    CHECK((GetFileAttributesW(file.c_str()) & FILE_ATTRIBUTE_READONLY) == 0);

    SetFileAttributesW(file.c_str(), before | FILE_ATTRIBUTE_HIDDEN);  // changed again by someone
    CHECK(!mc::CheckUndo(entry).ok);
    SetFileAttributesW(file.c_str(), FILE_ATTRIBUTE_NORMAL);
}

TEST_CASE(Undo_CreatedFolderAndFile_OnlyWhileStillEmpty_FOP022) {
    test::TempDir dir;
    fs::create_directory(dir.Path() / L"newdir");
    test::WriteFileContent(dir.Path() / L"new.txt", "");
    auto folder = Entry(mc::UndoKind::NewFolder, {mc::MakeCreatedItem(dir.Path() / L"newdir")});
    auto file = Entry(mc::UndoKind::NewFile, {mc::MakeCreatedItem(dir.Path() / L"new.txt")});

    test::WriteFileContent(dir.Path() / L"newdir" / L"inner.txt", "keep me");
    CHECK(!mc::CheckUndo(folder).ok);  // it gained a file: left alone
    fs::remove(dir.Path() / L"newdir" / L"inner.txt");
    CHECK(mc::PerformUndo(folder, {}).ok && !fs::exists(dir.Path() / L"newdir"));

    CHECK(mc::PerformUndo(file, {}).ok && !fs::exists(dir.Path() / L"new.txt"));

    test::WriteFileContent(dir.Path() / L"filled.txt", "");
    auto filled = Entry(mc::UndoKind::NewFile, {mc::MakeCreatedItem(dir.Path() / L"filled.txt")});
    test::WriteFileContent(dir.Path() / L"filled.txt", "content");
    CHECK(!mc::CheckUndo(filled).ok && fs::exists(dir.Path() / L"filled.txt"));
}

TEST_CASE(Undo_WholeEntryIsRefusedIfAnyItemChanged_FOP022) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"1.txt", "one");
    test::WriteFileContent(dir.Path() / L"2.txt", "two");
    fs::rename(dir.Path() / L"1.txt", dir.Path() / L"a.txt");
    fs::rename(dir.Path() / L"2.txt", dir.Path() / L"b.txt");
    auto entry = Entry(mc::UndoKind::Rename, {mc::MakeMovedItem(dir.Path() / L"1.txt", dir.Path() / L"a.txt"),
                                              mc::MakeMovedItem(dir.Path() / L"2.txt", dir.Path() / L"b.txt")});
    fs::remove(dir.Path() / L"b.txt");  // the second item vanished
    mc::UndoResult r = mc::PerformUndo(entry, {});
    CHECK(!r.ok && r.reversed == 0);
    CHECK(fs::exists(dir.Path() / L"a.txt") && !fs::exists(dir.Path() / L"1.txt"));  // the first was not reversed either
}

TEST_CASE(Fingerprint_SeesSizeCountAndTime_FOP022) {
    test::TempDir dir;
    fs::create_directories(dir.Path() / L"d");
    test::WriteFileContent(dir.Path() / L"d" / L"a.txt", "12345");
    test::WriteFileContent(dir.Path() / L"d" / L"b.txt", "123");
    mc::PathFingerprint f = mc::FingerprintOf(dir.Path() / L"d");
    CHECK(f.exists && f.isDirectory && f.files == 2 && f.bytes == 8 && f.newestWrite != 0);
    CHECK(!mc::FingerprintOf(dir.Path() / L"nope").exists);
    test::WriteFileContent(dir.Path() / L"d" / L"c.txt", "x");
    CHECK(!(mc::FingerprintOf(dir.Path() / L"d") == f));
}
