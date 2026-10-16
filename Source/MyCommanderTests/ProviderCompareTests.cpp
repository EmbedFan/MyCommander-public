#include "TestFixtures.h"
#include "TestFramework.h"

#include "DirCompare.h"
#include "LocalProvider.h"
#include "MemoryProvider.h"
#include "Search.h"

#include <algorithm>

using mc::CompareMethod;
using mc::CompareOptions;
using mc::CompareStatus;
using mc::MemoryProvider;

namespace {

const mc::CompareItem* Item(const mc::CompareOutcome& o, const std::wstring& relativePath) {
    auto it = std::find_if(o.items.begin(), o.items.end(),
                           [&](const mc::CompareItem& i) { return i.relativePath == relativePath; });
    return it == o.items.end() ? nullptr : &*it;
}

}  // namespace

TEST_CASE(ProviderCompare_StatusesBetweenTwoProviders_EXT002_SRC006) {
    MemoryProvider a(L"zip://a"), b(L"sftp://b");
    a.AddFile(L"/same.txt", "same", 100);
    b.AddFile(L"/same.txt", "same", 100);
    a.AddFile(L"/newer.txt", "x", 900000000);
    b.AddFile(L"/newer.txt", "x", 100);
    a.AddFile(L"/only-left.txt", "l");
    b.AddFile(L"/only-right.txt", "r");
    a.AddFile(L"/sub/inner.txt", "abc", 100);
    b.AddFile(L"/sub/inner.txt", "abd", 100);  // same size and time, different content
    CompareOptions options;
    options.method = CompareMethod::Content;
    options.recursive = true;
    mc::CompareOutcome o = mc::CompareDirectories(a, L"/", b, L"/", options);
    CHECK(o.leftListed && o.rightListed && !o.cancelled);
    CHECK(Item(o, L"same.txt") && Item(o, L"same.txt")->status == CompareStatus::Identical);
    CHECK(Item(o, L"newer.txt") && Item(o, L"newer.txt")->status == CompareStatus::Identical);  // content wins over time
    CHECK(Item(o, L"only-left.txt") && Item(o, L"only-left.txt")->status == CompareStatus::LeftOnly);
    CHECK(Item(o, L"only-right.txt") && Item(o, L"only-right.txt")->status == CompareStatus::RightOnly);
    CHECK(Item(o, L"sub\\inner.txt") && Item(o, L"sub\\inner.txt")->status == CompareStatus::Different);
    CHECK(Item(o, L"sub") && Item(o, L"sub")->status == CompareStatus::Different);  // the folder rolls up
    CHECK(o.bytesRead > 0);

    options.method = CompareMethod::Timestamp;
    o = mc::CompareDirectories(a, L"/", b, L"/", options);
    CHECK(Item(o, L"newer.txt")->status == CompareStatus::LeftNewer);
}

TEST_CASE(ProviderCompare_LocalAgainstAProvider_EXT002_SRC006) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"a.txt", "aaa");
    test::WriteFileContent(dir.Path() / L"b.txt", "local");
    mc::LocalProvider local;
    MemoryProvider server(L"ftp://x");
    server.AddFile(L"/a.txt", "aaa");
    server.AddFile(L"/b.txt", "remote");
    CompareOptions options;
    options.method = CompareMethod::Content;
    mc::CompareOutcome o = mc::CompareDirectories(local, dir.Path().wstring(), server, L"/", options);
    CHECK(Item(o, L"a.txt") && Item(o, L"a.txt")->status == CompareStatus::Identical);
    CHECK(Item(o, L"b.txt") && Item(o, L"b.txt")->status != CompareStatus::Identical);
}

TEST_CASE(ProviderCompare_LinksAreNeverDescendedOrRead_EXT002_SEC006) {
    MemoryProvider a, b;
    a.AddFile(L"/target/secret.txt", "s");
    a.AddLink(L"/lnk", L"/target", true);
    b.AddDirectory(L"/lnk");
    b.AddFile(L"/lnk/other.txt", "o");
    CompareOptions options;
    options.recursive = true;
    mc::CompareOutcome o = mc::CompareDirectories(a, L"/", b, L"/", options);
    CHECK(Item(o, L"lnk") != nullptr);
    CHECK(Item(o, L"lnk\\secret.txt") == nullptr && Item(o, L"lnk\\other.txt") == nullptr);
}

TEST_CASE(ProviderCompare_AMissingRootIsReported_EXT002) {
    MemoryProvider a, b;
    mc::CompareOutcome o = mc::CompareDirectories(a, L"/missing", b, L"/", CompareOptions{});
    CHECK(!o.leftListed && o.items.empty());
}

TEST_CASE(JoinProviderRelative_UsesTheProvidersOwnSeparator_EXT002) {
    MemoryProvider p;
    CHECK(mc::JoinProviderRelative(p, L"/root", std::filesystem::path(L"sub\\x.txt")) == L"/root/sub/x.txt");
    CHECK(mc::JoinProviderRelative(p, L"/", std::filesystem::path()) == L"/");
}

TEST_CASE(SearchProvider_FindsNamesRecursivelyWithoutFollowingLinks_EXT002_SRC001) {
    MemoryProvider p;
    p.AddFile(L"/a.log", "1");
    p.AddFile(L"/docs/b.log", "2");
    p.AddFile(L"/docs/c.txt", "3");
    p.AddFile(L"/elsewhere/d.log", "4");
    p.AddLink(L"/docs/lnk", L"/elsewhere", true);
    mc::SearchOptions options;
    options.nameMask = L"*.log";
    mc::SearchOutcome o = mc::SearchProvider(p, L"/", options);
    std::vector<std::wstring> found;
    for (const auto& r : o.results) found.push_back(r.fullPath.wstring());
    std::sort(found.begin(), found.end());
    CHECK(found == (std::vector<std::wstring>{L"/a.log", L"/docs/b.log", L"/elsewhere/d.log"}));  // once, not via the link
    options.nameMask = L"lnk";
    o = mc::SearchProvider(p, L"/docs", options);
    CHECK(o.results.size() == 1 && o.results[0].isDirectory);  // a matching link is still reported
}

TEST_CASE(SearchProvider_CancelAndTruncate_EXT002) {
    MemoryProvider p;
    for (int i = 0; i < 20; ++i) p.AddFile(L"/f" + std::to_wstring(i) + L".txt", "x");
    mc::SearchOptions options;
    options.nameMask = L"*";
    options.maxResults = 5;
    mc::SearchOutcome o = mc::SearchProvider(p, L"/", options);
    CHECK(o.truncated && o.results.size() == 5);
    o = mc::SearchProvider(p, L"/", options, [] { return true; });
    CHECK(o.cancelled && o.results.empty());
}
