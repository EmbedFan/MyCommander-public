#include "TestFixtures.h"
#include "TestFramework.h"

#include "RecycleBin.h"
#include "UndoJournal.h"

#include <windows.h>
#include <cstdlib>

namespace fs = std::filesystem;

namespace {

// A fake Recycle Bin: "deleting" moves a path into a folder and records it.
class FakeBin final : public mc::IRecycleBin {
public:
    explicit FakeBin(fs::path store) : store_(std::move(store)) { fs::create_directories(store_); }
    void Delete(const fs::path& original, uint64_t when) {
        mc::RecycleBinItem item;
        item.displayName = original.filename().wstring();
        item.originalPath = original;
        item.deletedUtc = when;
        std::error_code ec;
        item.isDirectory = fs::is_directory(original, ec);
        item.sizeBytes = item.isDirectory ? 0 : static_cast<uint64_t>(fs::file_size(original, ec));
        item.handle = items_.size();
        fs::rename(original, store_ / std::to_wstring(item.handle), ec);
        items_.push_back(item);
        alive_.push_back(true);
    }
    std::vector<mc::RecycleBinItem> List(std::wstring&) override {
        std::vector<mc::RecycleBinItem> out;
        for (size_t i = 0; i < items_.size(); ++i)
            if (alive_[i]) out.push_back(items_[i]);
        mc::SortNewestFirst(out);
        return out;
    }
    bool Restore(const mc::RecycleBinItem& item, std::wstring& error) override {
        if (failRestore) {
            error = L"fake restore failure";
            return false;
        }
        std::error_code ec;
        fs::create_directories(item.originalPath.parent_path(), ec);
        fs::rename(store_ / std::to_wstring(item.handle), item.originalPath, ec);
        if (ec) {
            error = L"rename failed";
            return false;
        }
        alive_[item.handle] = false;
        return true;
    }
    bool failRestore = false;

private:
    fs::path store_;
    std::vector<mc::RecycleBinItem> items_;
    std::vector<bool> alive_;
};

bool Exists(const fs::path& p) {
    std::error_code ec;
    return fs::exists(p, ec);
}

}  // namespace

TEST_CASE(RecycleBin_CheckRestore_RefusesTakenName_FOP023) {
    mc::RecycleBinItem item;
    item.displayName = L"a.txt";
    item.originalPath = L"C:\\x\\A.TXT";
    auto taken = [](const fs::path& p) { return p == fs::path(L"C:\\x\\A.TXT"); };
    auto free_ = [](const fs::path&) { return false; };
    CHECK(!mc::CheckRestore(item, taken).ok);
    CHECK(!mc::CheckRestore(item, taken).message.empty());
    CHECK(mc::CheckRestore(item, free_).ok);
    item.originalPath.clear();
    CHECK(!mc::CheckRestore(item, free_).ok);
}

TEST_CASE(RecycleBin_Row_And_Sort_FOP023) {
    mc::RecycleBinItem a, b, c;
    a.displayName = L"b.txt";
    a.originalPath = L"C:\\d\\b.txt";
    a.deletedUtc = 100;
    a.sizeBytes = 1536;
    b.displayName = L"\x6587\x4ef6.txt";  // CJK name
    b.originalPath = fs::path(L"C:\\d") / b.displayName;
    b.deletedUtc = 300;
    c.displayName = L"a.txt";
    c.originalPath = L"C:\\d\\a.txt";
    c.deletedUtc = 100;
    std::vector<mc::RecycleBinItem> v{a, b, c};
    mc::SortNewestFirst(v);
    CHECK(v[0].displayName == b.displayName);  // newest first
    CHECK(v[1].displayName == L"a.txt");       // tie broken by name
    CHECK(v[2].displayName == L"b.txt");
    std::wstring row = mc::FormatRecycleBinRow(a);
    CHECK(row.find(L"b.txt") != std::wstring::npos);
    CHECK(row.find(L"C:\\d") != std::wstring::npos);
    CHECK(row.find(L"1.5 KB") != std::wstring::npos);
    CHECK(mc::FormatRecycleBinRow(b).find(b.displayName) != std::wstring::npos);
}

TEST_CASE(RecycleBin_FindInBin_Matching_FOP022) {
    std::vector<mc::RecycleBinItem> v(3);
    v[0].originalPath = L"C:\\d\\f.txt";
    v[0].deletedUtc = 50;  // older than the op
    v[0].sizeBytes = 10;
    v[1].originalPath = L"C:\\D\\F.TXT";
    v[1].deletedUtc = 200;
    v[1].sizeBytes = 10;
    v[2].originalPath = L"C:\\d\\f.txt";
    v[2].deletedUtc = 300;
    v[2].sizeBytes = 99;
    CHECK(mc::FindInBin(v, L"C:\\d\\f.txt", 100, 10, true) == &v[1]);  // case-insensitive, not older, size matches
    CHECK(mc::FindInBin(v, L"C:\\d\\f.txt", 100, 99, false) == &v[2]);  // folders: newest by path and time
    CHECK(mc::FindInBin(v, L"C:\\d\\f.txt", 400, 10, true) == nullptr);
    CHECK(mc::FindInBin(v, L"C:\\d\\other.txt", 0, 0, false) == nullptr);
}

TEST_CASE(RecycleBin_FakeBin_RestoreFailureKeepsItem_FOP023) {
    test::TempDir dir;
    FakeBin bin(dir.Path() / L"bin");
    fs::path f = dir.Path() / L"f.txt";
    test::WriteFileContent(f, "hello");
    bin.Delete(f, 10);
    std::wstring err;
    auto items = bin.List(err);
    CHECK(items.size() == 1);
    bin.failRestore = true;
    CHECK(!bin.Restore(items[0], err));
    CHECK(bin.List(err).size() == 1);
    CHECK(!Exists(f));
    bin.failRestore = false;
    CHECK(bin.Restore(items[0], err));
    CHECK(Exists(f));
    CHECK(bin.List(err).empty());
}

TEST_CASE(RecycleBin_DeleteUndo_RoundTrip_FOP022) {
    test::TempDir dir;
    FakeBin bin(dir.Path() / L"bin");
    fs::path file = dir.Path() / L"data.txt";
    fs::path sub = dir.Path() / L"folder";
    test::WriteFileContent(file, "12345");
    fs::create_directories(sub);

    mc::UndoEntry e;
    e.kind = mc::UndoKind::Delete;
    e.description = L"delete 2 items";
    e.startedUtc = 5;
    e.items.push_back(mc::MakeDeletedItem(file, mc::FingerprintOf(file)));
    e.items.push_back(mc::MakeDeletedItem(sub, mc::FingerprintOf(sub)));
    bin.Delete(file, 10);
    bin.Delete(sub, 10);
    CHECK(mc::CheckUndo(e).ok);

    std::wstring err;
    std::vector<mc::RecycleBinItem> listed = bin.List(err);
    mc::UndoActions actions;
    auto find = [&](const fs::path& p, uint64_t nb, const mc::PathFingerprint& fp) {
        return mc::FindInBin(listed, p, nb, fp.bytes, !fp.isDirectory);
    };
    actions.locate = [&](const fs::path& p, uint64_t nb, const mc::PathFingerprint& fp, std::wstring& problem) {
        if (find(p, nb, fp)) return true;
        problem = L"missing";
        return false;
    };
    actions.restore = [&](const fs::path& p, uint64_t nb, const mc::PathFingerprint& fp, std::wstring& error) {
        const mc::RecycleBinItem* item = find(p, nb, fp);
        return item && bin.Restore(*item, error);
    };
    mc::UndoResult r = mc::PerformUndo(e, actions);
    CHECK(r.ok);
    CHECK(r.reversed == 2);
    CHECK(Exists(file));
    CHECK(Exists(sub));
}

TEST_CASE(RecycleBin_DeleteUndo_RefusedWhenNameReusedOrMissing_FOP022) {
    test::TempDir dir;
    FakeBin bin(dir.Path() / L"bin");
    fs::path file = dir.Path() / L"data.txt";
    test::WriteFileContent(file, "abc");
    mc::UndoEntry e;
    e.kind = mc::UndoKind::Delete;
    e.description = L"delete data.txt";
    e.items.push_back(mc::MakeDeletedItem(file, mc::FingerprintOf(file)));
    bin.Delete(file, 10);

    // The name was created again: refused by CheckUndo, nothing touched.
    test::WriteFileContent(file, "new");
    mc::UndoResult r = mc::PerformUndo(e, {});
    CHECK(!r.ok);
    CHECK(r.reversed == 0);
    CHECK(test::ReadFileContent(file) == "new");

    // The bin no longer holds it (emptied): locate fails, nothing is restored.
    fs::remove(file);
    mc::UndoActions actions;
    actions.locate = [](const fs::path&, uint64_t, const mc::PathFingerprint&, std::wstring& problem) {
        problem = L"not found in the Recycle Bin.";
        return false;
    };
    int restored = 0;
    actions.restore = [&](const fs::path&, uint64_t, const mc::PathFingerprint&, std::wstring&) {
        ++restored;
        return true;
    };
    r = mc::PerformUndo(e, actions);
    CHECK(!r.ok);
    CHECK(r.reversed == 0);
    CHECK(restored == 0);
}

// Opt-in: touches the REAL Recycle Bin of the user running the tests (lists
// it; never restores or deletes anything). TST-004.
TEST_CASE(RecycleBin_RealBin_ListsWithoutError_FOP023) {
    char flag[8] = {};
    size_t flagLen = 0;
    getenv_s(&flagLen, flag, sizeof(flag), "MC_RECYCLE_TEST");
    if (std::string(flag) != "1") return;
    std::wstring err;
    auto bin = mc::OpenShellRecycleBin(err);
    CHECK(bin != nullptr);
    if (!bin) return;
    auto items = bin->List(err);
    for (const auto& item : items) {
        CHECK(!item.displayName.empty());
        CHECK(!item.originalPath.empty());
    }
}
