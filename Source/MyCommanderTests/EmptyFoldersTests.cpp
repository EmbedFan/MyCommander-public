#include "TestFixtures.h"
#include "TestFramework.h"

#include "EmptyFolders.h"

#include <algorithm>
#include <set>

namespace fs = std::filesystem;

using mc::EmptyFolderOptions;
using mc::EmptyFoldersOutcome;
using mc::FindEmptyFolders;
using mc::IsFolderEmptyNow;

namespace {

// The reported folders as paths relative to `root`, for order-independent checks.
std::set<std::wstring> Relative(const EmptyFoldersOutcome& outcome, const fs::path& root) {
    std::set<std::wstring> names;
    for (const auto& f : outcome.folders) names.insert(f.path.lexically_relative(root).wstring());
    return names;
}

size_t BelowOf(const EmptyFoldersOutcome& outcome, const fs::path& path) {
    for (const auto& f : outcome.folders)
        if (f.path == path) return f.emptyChildren;
    return static_cast<size_t>(-1);
}

void Mkdirs(const fs::path& p) {
    std::error_code ec;
    fs::create_directories(p, ec);
}

}  // namespace

TEST_CASE(EmptyFolders_FindsEmptyFolderAndSkipsOneWithAFile_SRC009) {
    test::TempDir root;
    Mkdirs(root.Path() / L"empty");
    Mkdirs(root.Path() / L"full");
    test::WriteFileContent(root.Path() / L"full" / L"a.txt", "x");

    EmptyFoldersOutcome out = FindEmptyFolders(root.Path(), EmptyFolderOptions{});
    CHECK((Relative(out, root.Path()) == std::set<std::wstring>{L"empty"}));
    CHECK(!out.cancelled && !out.truncated && out.unreadable == 0);
}

TEST_CASE(EmptyFolders_ChainIsReportedOnceAtTheTop_SRC009) {
    test::TempDir root;
    Mkdirs(root.Path() / L"a" / L"b" / L"c");

    EmptyFolderOptions on;  // chainsCountAsEmpty defaults to true
    EmptyFoldersOutcome out = FindEmptyFolders(root.Path(), on);
    CHECK((Relative(out, root.Path()) == std::set<std::wstring>{L"a"}));
    CHECK(BelowOf(out, root.Path() / L"a") == 2);  // b and c

    EmptyFolderOptions off;
    off.chainsCountAsEmpty = false;
    out = FindEmptyFolders(root.Path(), off);
    CHECK((Relative(out, root.Path()) == std::set<std::wstring>{L"a\\b\\c"}));  // only the leaf
}

TEST_CASE(EmptyFolders_EmptySubfolderOfAFolderWithAFileIsStillFound_SRC009) {
    test::TempDir root;
    Mkdirs(root.Path() / L"mixed" / L"hollow");
    test::WriteFileContent(root.Path() / L"mixed" / L"keep.txt", "x");

    EmptyFoldersOutcome out = FindEmptyFolders(root.Path(), EmptyFolderOptions{});
    CHECK((Relative(out, root.Path()) == std::set<std::wstring>{L"mixed\\hollow"}));
}

TEST_CASE(EmptyFolders_JunkFilesAreIgnoredOnlyWhenAsked_SRC009) {
    test::TempDir root;
    Mkdirs(root.Path() / L"junky");
    test::WriteFileContent(root.Path() / L"junky" / L"Thumbs.db", "x");
    test::WriteFileContent(root.Path() / L"junky" / L"DESKTOP.INI", "x");

    EmptyFolderOptions ignore;
    CHECK((Relative(FindEmptyFolders(root.Path(), ignore), root.Path()) == std::set<std::wstring>{L"junky"}));

    EmptyFolderOptions strict;
    strict.ignoreJunkFiles = false;
    CHECK(FindEmptyFolders(root.Path(), strict).folders.empty());
}

TEST_CASE(EmptyFolders_HiddenFileKeepsItsFolder_HiddenFolderIsSkippedUnlessIncluded_SRC009) {
    test::TempDir root;
    Mkdirs(root.Path() / L"hasHiddenFile");
    fs::path hiddenFile = root.Path() / L"hasHiddenFile" / L"h.txt";
    test::WriteFileContent(hiddenFile, "x");
    SetFileAttributesW(hiddenFile.c_str(), FILE_ATTRIBUTE_HIDDEN);

    Mkdirs(root.Path() / L"outer" / L"hiddenDir");
    SetFileAttributesW((root.Path() / L"outer" / L"hiddenDir").c_str(), FILE_ATTRIBUTE_HIDDEN);

    EmptyFolderOptions skip;  // includeHidden = false
    EmptyFoldersOutcome out = FindEmptyFolders(root.Path(), skip);
    // A hidden *file* is content. A hidden *folder* that isn't entered keeps its
    // parent non-empty, so deleting "outer" could never take it along unseen.
    CHECK(out.folders.empty());

    EmptyFolderOptions include;
    include.includeHidden = true;
    out = FindEmptyFolders(root.Path(), include);
    CHECK((Relative(out, root.Path()) == std::set<std::wstring>{L"outer"}));  // the chain outer\hiddenDir
    CHECK(BelowOf(out, root.Path() / L"outer") == 1);
}

TEST_CASE(EmptyFolders_JunctionIsNeverEnteredOrReportedAndKeepsItsParent_SRC009) {
    test::TempDir root;
    test::TempDir elsewhere;
    Mkdirs(elsewhere.Path() / L"hollow");  // an empty folder *behind* the junction
    fs::path parent = root.Path() / L"parent";
    Mkdirs(parent);
    if (!test::CreateJunction(parent / L"link", elsewhere.Path())) {
        std::printf("note: junction creation unavailable here; skipping the junction check\n");
        return;
    }
    EmptyFoldersOutcome out = FindEmptyFolders(root.Path(), EmptyFolderOptions{});
    CHECK(out.folders.empty());  // "parent" holds a reparse point: not empty, and it is not walked
    CHECK(fs::exists(elsewhere.Path() / L"hollow"));
}

TEST_CASE(EmptyFolders_RootIsNeverReported_SRC009) {
    test::TempDir root;  // completely empty
    CHECK(FindEmptyFolders(root.Path(), EmptyFolderOptions{}).folders.empty());

    Mkdirs(root.Path() / L"only");
    EmptyFoldersOutcome out = FindEmptyFolders(root.Path(), EmptyFolderOptions{});
    CHECK((Relative(out, root.Path()) == std::set<std::wstring>{L"only"}));  // its child, not the root
}

TEST_CASE(EmptyFolders_CancelAndResultCap_SRC009) {
    test::TempDir root;
    for (int i = 0; i < 6; ++i) Mkdirs(root.Path() / (L"e" + std::to_wstring(i)));

    int polls = 0;
    EmptyFoldersOutcome cancelled =
        FindEmptyFolders(root.Path(), EmptyFolderOptions{}, [&] { return ++polls >= 3; });
    CHECK(cancelled.cancelled);

    EmptyFolderOptions capped;
    capped.maxResults = 2;
    EmptyFoldersOutcome out = FindEmptyFolders(root.Path(), capped);
    CHECK(out.folders.size() == 2);
    CHECK(out.truncated);
}

TEST_CASE(EmptyFolders_MissingOrNonFolderRootYieldsNothing_SRC009) {
    test::TempDir root;
    test::WriteFileContent(root.Path() / L"file.txt", "x");
    CHECK(FindEmptyFolders(root.Path() / L"nope", EmptyFolderOptions{}).folders.empty());
    CHECK(FindEmptyFolders(root.Path() / L"file.txt", EmptyFolderOptions{}).folders.empty());
}

TEST_CASE(EmptyFolders_RecheckSeesAFileThatAppearedAfterTheScan_SRC009) {
    test::TempDir root;
    Mkdirs(root.Path() / L"hollow" / L"inner");
    const fs::path hollow = root.Path() / L"hollow";

    CHECK(IsFolderEmptyNow(hollow, EmptyFolderOptions{}));
    test::WriteFileContent(hollow / L"inner" / L"late.txt", "x");  // appears after "the scan"
    CHECK(!IsFolderEmptyNow(hollow, EmptyFolderOptions{}));
    CHECK(!IsFolderEmptyNow(root.Path() / L"gone", EmptyFolderOptions{}));
}
