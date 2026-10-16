#include "TestFramework.h"
#include "TestFixtures.h"

#include "LocalProvider.h"

#include <algorithm>

using mc::LocalProvider;
using mc::ProviderEntry;
using mc::ProviderStatus;

namespace {

const ProviderEntry* Find(const std::vector<ProviderEntry>& entries, const std::wstring& name) {
    auto it = std::find_if(entries.begin(), entries.end(), [&](const ProviderEntry& e) { return e.name == name; });
    return it == entries.end() ? nullptr : &*it;
}

std::string ReadAllFrom(LocalProvider& p, const std::wstring& path) {
    std::unique_ptr<mc::IReadStream> in;
    std::wstring err;
    if (p.OpenRead(path, in, {}, err) != ProviderStatus::Ok) return "<error>";
    std::string data;
    char buffer[7];  // deliberately small: exercises several reads
    size_t got = 0;
    while (in->Read(buffer, sizeof(buffer), got, err) == ProviderStatus::Ok && got > 0) data.append(buffer, got);
    return data;
}

}  // namespace

TEST_CASE(LocalProvider_List_ReportsFilesFoldersSizesAndUnicodeNames_EXT002) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "12345");
    test::WriteFileContent(dir.Path() / L"árvíztűrő_日本.txt", "xy");
    std::filesystem::create_directory(dir.Path() / L"sub");
    LocalProvider p;
    std::vector<ProviderEntry> entries;
    std::wstring err;
    CHECK(p.List(dir.Path().wstring(), entries, {}, err) == ProviderStatus::Ok);
    CHECK(entries.size() == 3);
    const ProviderEntry* a = Find(entries, L"a.txt");
    CHECK(a && !a->isDirectory && a->size == 5 && a->modifiedUtc != 0);
    const ProviderEntry* u = Find(entries, L"árvíztűrő_日本.txt");
    CHECK(u && u->size == 2);
    const ProviderEntry* s = Find(entries, L"sub");
    CHECK(s && s->isDirectory && !s->isLink);
}

TEST_CASE(LocalProvider_JoinParentRoot_FollowWindowsPaths_EXT002) {
    LocalProvider p;
    CHECK(p.Join(L"C:\\a", L"b.txt") == L"C:\\a\\b.txt");
    CHECK(p.Parent(L"C:\\a\\b.txt") == L"C:\\a");
    CHECK(p.Parent(L"C:\\a") == L"C:\\");
    CHECK(p.IsRoot(L"C:\\"));
    CHECK(!p.IsRoot(L"C:\\a"));
    CHECK(p.Parent(L"C:\\") == L"C:\\");
    CHECK(p.SamePath(L"C:\\A\\b", L"c:\\a\\B"));
    CHECK(mc::ProviderLeafName(p, L"C:\\a\\b.txt") == L"b.txt");
    CHECK(mc::ProviderPathIsSameOrWithin(p, L"C:\\a", L"C:\\A\\b\\c"));
    CHECK(!mc::ProviderPathIsSameOrWithin(p, L"C:\\a", L"C:\\ab"));
}

TEST_CASE(LocalProvider_Stat_MissingPathIsNotFound_EXT002) {
    test::TempDir dir;
    LocalProvider p;
    ProviderEntry e;
    std::wstring err;
    CHECK(p.Stat((dir.Path() / L"nope.txt").wstring(), e, {}, err) == ProviderStatus::NotFound);
    test::WriteFileContent(dir.Path() / L"yes.txt", "abc");
    CHECK(p.Stat((dir.Path() / L"yes.txt").wstring(), e, {}, err) == ProviderStatus::Ok);
    CHECK(e.name == L"yes.txt" && e.size == 3 && !e.isDirectory);
}

TEST_CASE(LocalProvider_WriteStream_TargetUnchangedUntilFinish_EXT002) {
    test::TempDir dir;
    auto target = dir.Path() / L"t.txt";
    test::WriteFileContent(target, "old");
    LocalProvider p;
    std::unique_ptr<mc::IWriteStream> out;
    std::wstring err;
    CHECK(p.OpenWrite(target.wstring(), 3, 0, out, {}, err) == ProviderStatus::Ok);
    CHECK(out->Write("new!", 4, err) == ProviderStatus::Ok);
    CHECK(test::ReadFileContent(target) == "old");
    CHECK(std::filesystem::exists(target.wstring() + mc::kLocalPartSuffix));
    CHECK(out->Finish(err) == ProviderStatus::Ok);
    CHECK(test::ReadFileContent(target) == "new!");
    CHECK(!std::filesystem::exists(target.wstring() + mc::kLocalPartSuffix));
}

TEST_CASE(LocalProvider_WriteStream_AbortKeepsTheOriginalAndLeavesNoPartFile_EXT002) {
    test::TempDir dir;
    auto target = dir.Path() / L"t.txt";
    test::WriteFileContent(target, "original");
    LocalProvider p;
    {
        std::unique_ptr<mc::IWriteStream> out;
        std::wstring err;
        CHECK(p.OpenWrite(target.wstring(), 0, 0, out, {}, err) == ProviderStatus::Ok);
        out->Write("partial", 7, err);
        out->Abort();
    }
    {
        std::unique_ptr<mc::IWriteStream> out;
        std::wstring err;
        p.OpenWrite(target.wstring(), 0, 0, out, {}, err);
        out->Write("dropped", 7, err);
        // destroyed without Finish
    }
    CHECK(test::ReadFileContent(target) == "original");
    CHECK(!std::filesystem::exists(target.wstring() + mc::kLocalPartSuffix));
}

TEST_CASE(LocalProvider_WriteStream_AppliesModifiedTimeAndOverwritesReadOnly_EXT002) {
    test::TempDir dir;
    auto target = dir.Path() / L"ro.txt";
    test::WriteFileContent(target, "old");
    SetFileAttributesW(target.c_str(), FILE_ATTRIBUTE_READONLY);
    LocalProvider p;
    const uint64_t stamp = 132000000000000000ull;  // 2019
    std::unique_ptr<mc::IWriteStream> out;
    std::wstring err;
    CHECK(p.OpenWrite(target.wstring(), 0, stamp, out, {}, err) == ProviderStatus::Ok);
    out->Write("x", 1, err);
    CHECK(out->Finish(err) == ProviderStatus::Ok);
    ProviderEntry e;
    CHECK(p.Stat(target.wstring(), e, {}, err) == ProviderStatus::Ok);
    CHECK(e.modifiedUtc == stamp);
    CHECK(test::ReadFileContent(target) == "x");
}

TEST_CASE(LocalProvider_ReadStream_ReturnsTheWholeFile_EXT002) {
    test::TempDir dir;
    std::string content(100000, '\0');
    for (size_t i = 0; i < content.size(); ++i) content[i] = static_cast<char>(i * 31);
    test::WriteFileContent(dir.Path() / L"r.bin", content);
    LocalProvider p;
    CHECK(ReadAllFrom(p, (dir.Path() / L"r.bin").wstring()) == content);
}

TEST_CASE(LocalProvider_MakeDirectoryRenameDelete_EXT002) {
    test::TempDir dir;
    LocalProvider p;
    std::wstring err;
    std::wstring d = (dir.Path() / L"new").wstring();
    CHECK(p.MakeDirectory(d, {}, err) == ProviderStatus::Ok);
    CHECK(p.MakeDirectory(d, {}, err) == ProviderStatus::Failed);
    test::WriteFileContent(dir.Path() / L"a.txt", "a");
    test::WriteFileContent(dir.Path() / L"b.txt", "b");
    CHECK(p.Rename((dir.Path() / L"a.txt").wstring(), (dir.Path() / L"b.txt").wstring(), {}, err) ==
          ProviderStatus::Failed);  // never overwrites
    CHECK(test::ReadFileContent(dir.Path() / L"b.txt") == "b");
    CHECK(p.Rename((dir.Path() / L"a.txt").wstring(), (dir.Path() / L"c.txt").wstring(), {}, err) == ProviderStatus::Ok);
    CHECK(p.Delete((dir.Path() / L"c.txt").wstring(), false, {}, err) == ProviderStatus::Ok);
    CHECK(p.Delete(d, true, {}, err) == ProviderStatus::Ok);
    CHECK(!std::filesystem::exists(d));
}

TEST_CASE(LocalProvider_LongPaths_WorkBeyondMaxPath_EXT002) {
    test::TempDir dir;
    LocalProvider p;
    std::wstring err;
    std::wstring path = dir.Path().wstring();
    for (int i = 0; i < 8; ++i) {
        path = p.Join(path, std::wstring(40, static_cast<wchar_t>(L'a' + i)));
        CHECK(p.MakeDirectory(path, {}, err) == ProviderStatus::Ok);
    }
    CHECK(path.size() > MAX_PATH);
    std::wstring file = p.Join(path, L"deep.txt");
    std::unique_ptr<mc::IWriteStream> out;
    CHECK(p.OpenWrite(file, 0, 0, out, {}, err) == ProviderStatus::Ok);
    out->Write("deep", 4, err);
    CHECK(out->Finish(err) == ProviderStatus::Ok);
    CHECK(ReadAllFrom(p, file) == "deep");
}

TEST_CASE(LocalProvider_DeleteJunction_RemovesTheLinkNotItsTarget_EXT002_SEC006) {
    test::TempDir dir;
    std::filesystem::create_directory(dir.Path() / L"target");
    test::WriteFileContent(dir.Path() / L"target" / L"keep.txt", "keep");
    if (!test::CreateJunction(dir.Path() / L"link", dir.Path() / L"target")) {
        std::wprintf(L"  note: junction creation unavailable, skipping\n");
        return;
    }
    LocalProvider p;
    std::vector<ProviderEntry> entries;
    std::wstring err;
    p.List(dir.Path().wstring(), entries, {}, err);
    const ProviderEntry* link = Find(entries, L"link");
    CHECK(link && link->isLink && link->isDirectory);
    CHECK(p.Delete((dir.Path() / L"link").wstring(), true, {}, err) == ProviderStatus::Ok);
    CHECK(!std::filesystem::exists(dir.Path() / L"link"));
    CHECK(test::ReadFileContent(dir.Path() / L"target" / L"keep.txt") == "keep");
}
