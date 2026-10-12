#include "TestFixtures.h"
#include "TestFramework.h"

#include "PathUtil.h"

#include <algorithm>
#include <cwctype>
#include <system_error>

using mc::PathsEquivalent;
using mc::ResolveExistingDirectory;
using mc::ToWideMessage;
using test::TempDir;

// --- ChildNameToward (IS-0019) -------------------------------------------------

TEST_CASE(ChildNameToward_ImmediateAndDeeperChild_IS0019) {
    CHECK(mc::ChildNameToward(L"C:\\Work", L"C:\\Work\\Projects") == L"Projects");
    CHECK(mc::ChildNameToward(L"C:\\Work", L"C:\\Work\\Projects\\App") == L"Projects");
    CHECK(mc::ChildNameToward(L"C:\\", L"C:\\Work") == L"Work");
    CHECK(mc::ChildNameToward(L"\\\\srv\\share", L"\\\\srv\\share\\a\\b") == L"a");
}

TEST_CASE(ChildNameToward_CaseAndTrailingSeparatorIgnored_IS0019) {
    CHECK(mc::ChildNameToward(L"c:\\work", L"C:\\Work\\Projects") == L"Projects");
    CHECK(mc::ChildNameToward(L"C:\\Work\\", L"C:\\Work\\Projects\\") == L"Projects");
    CHECK(mc::ChildNameToward(L"C:/Work", L"C:\\Work\\Projects") == L"Projects");
}

TEST_CASE(ChildNameToward_NotAnAncestorIsEmpty_IS0019) {
    CHECK(mc::ChildNameToward(L"C:\\Work", L"C:\\Work").empty());
    CHECK(mc::ChildNameToward(L"C:\\Work\\Projects", L"C:\\Work").empty());  // a child, not an ancestor
    CHECK(mc::ChildNameToward(L"C:\\Work", L"C:\\Other\\Projects").empty());
    CHECK(mc::ChildNameToward(L"C:\\Work", L"C:\\Workshop\\x").empty());     // prefix of a name isn't a parent
    CHECK(mc::ChildNameToward(L"D:\\Work", L"C:\\Work\\Projects").empty());
}

// --- ResolveCopyAsTarget (FOP-019, Shift+F5) ---------------------------------

TEST_CASE(ResolveCopyAsTarget_BareName_IsSiblingInSourceDir_FOP019) {
    TempDir base;
    auto t = mc::ResolveCopyAsTarget(base.Path(), L"a.txt", L"b.txt");
    CHECK(t.has_value());
    if (t) CHECK(*t == std::filesystem::weakly_canonical(base.Path() / L"b.txt"));
}

TEST_CASE(ResolveCopyAsTarget_QuotedAndPadded_IsTrimmed_FOP019) {
    TempDir base;
    auto t = mc::ResolveCopyAsTarget(base.Path(), L"a.txt", L"  \"my copy.txt\" ");
    CHECK(t.has_value());
    if (t) CHECK(t->filename() == L"my copy.txt");
}

TEST_CASE(ResolveCopyAsTarget_FullPath_UsedAsGiven_FOP019) {
    TempDir base;
    TempDir other;
    auto t = mc::ResolveCopyAsTarget(base.Path(), L"a.txt", (other.Path() / L"c.txt").wstring());
    CHECK(t.has_value());
    if (t) {
        CHECK(std::filesystem::equivalent(t->parent_path(), other.Path()));
        CHECK(t->filename() == L"c.txt");
    }
}

TEST_CASE(ResolveCopyAsTarget_ExistingDirectory_CopiesInsideKeepingName_FOP019) {
    TempDir base;
    std::filesystem::create_directory(base.Path() / L"sub");
    auto t = mc::ResolveCopyAsTarget(base.Path(), L"a.txt", L"sub");
    CHECK(t.has_value());
    if (t) {
        CHECK(std::filesystem::equivalent(t->parent_path(), base.Path() / L"sub"));
        CHECK(t->filename() == L"a.txt");
    }
}

TEST_CASE(ResolveCopyAsTarget_TrailingSeparator_MeansIntoDirectory_FOP019) {
    TempDir base;
    auto t = mc::ResolveCopyAsTarget(base.Path(), L"a.txt", L"newdir\\");
    CHECK(t.has_value());
    if (t) {
        CHECK(t->filename() == L"a.txt");
        CHECK(t->parent_path().filename() == L"newdir");
    }
}

TEST_CASE(ResolveCopyAsTarget_UnchangedDirectoryName_IsTheSourceItself_FOP019) {
    // A directory source confirmed with its own name must resolve to itself
    // (so the caller reports "same as the original"), not to "dir\dir".
    TempDir base;
    std::filesystem::create_directory(base.Path() / L"dir");
    auto t = mc::ResolveCopyAsTarget(base.Path(), L"dir", L"dir");
    CHECK(t.has_value());
    if (t) CHECK(mc::PathsEquivalent(*t, base.Path() / L"dir"));
}

TEST_CASE(ResolveCopyAsTarget_EmptyInput_IsNullopt_FOP019) {
    TempDir base;
    CHECK(!mc::ResolveCopyAsTarget(base.Path(), L"a.txt", L"   ").has_value());
    CHECK(!mc::ResolveCopyAsTarget(base.Path(), L"a.txt", L"\"\"").has_value());
}

TEST_CASE(ResolveExistingDirectory_RelativeNameUnderBase_Resolves) {
    TempDir base;
    std::filesystem::create_directory(base.Path() / L"subdir");

    auto resolved = ResolveExistingDirectory(base.Path(), L"subdir");
    CHECK(resolved.has_value());
    if (resolved) CHECK(std::filesystem::equivalent(*resolved, base.Path() / L"subdir"));
}

TEST_CASE(ResolveExistingDirectory_AbsolutePath_ResolvesRegardlessOfBase) {
    TempDir base;
    TempDir elsewhere;

    auto resolved = ResolveExistingDirectory(base.Path(), elsewhere.Path().wstring());
    CHECK(resolved.has_value());
    if (resolved) CHECK(std::filesystem::equivalent(*resolved, elsewhere.Path()));
}

TEST_CASE(ResolveExistingDirectory_DotDotSegment_NormalizesToParent) {
    TempDir base;
    std::filesystem::create_directory(base.Path() / L"subdir");

    auto resolved = ResolveExistingDirectory(base.Path() / L"subdir", L"..");
    CHECK(resolved.has_value());
    if (resolved) CHECK(std::filesystem::equivalent(*resolved, base.Path()));
}

TEST_CASE(ResolveExistingDirectory_SurroundingWhitespaceAndQuotes_AreTrimmed) {
    TempDir base;
    std::filesystem::create_directory(base.Path() / L"subdir");

    auto resolved = ResolveExistingDirectory(base.Path(), L"  \"subdir\"  ");
    CHECK(resolved.has_value());
    if (resolved) CHECK(std::filesystem::equivalent(*resolved, base.Path() / L"subdir"));
}

TEST_CASE(ResolveExistingDirectory_NonexistentPath_ReturnsNullopt) {
    TempDir base;
    CHECK(!ResolveExistingDirectory(base.Path(), L"does-not-exist").has_value());
}

TEST_CASE(ResolveExistingDirectory_ExistingFileNotDirectory_ReturnsNullopt) {
    TempDir base;
    test::WriteFileContent(base.Path() / L"file.txt", "hi");
    CHECK(!ResolveExistingDirectory(base.Path(), L"file.txt").has_value());
}

TEST_CASE(ResolveExistingDirectory_BlankInput_ReturnsNullopt) {
    TempDir base;
    CHECK(!ResolveExistingDirectory(base.Path(), L"   ").has_value());
    CHECK(!ResolveExistingDirectory(base.Path(), L"").has_value());
}

// IS-0015/NAV-012: PathsEquivalent backs the "are these two panels showing
// the same directory" check used to keep them in sync.
TEST_CASE(PathsEquivalent_SamePathIsEquivalentToItself) {
    TempDir dir;
    CHECK(PathsEquivalent(dir.Path(), dir.Path()));
}

TEST_CASE(PathsEquivalent_DifferentCaseIsStillEquivalentOnWindows) {
    TempDir dir;
    std::wstring upper = dir.Path().wstring();
    std::transform(upper.begin(), upper.end(), upper.begin(),
                    [](wchar_t c) { return static_cast<wchar_t>(std::towupper(c)); });
    CHECK(PathsEquivalent(dir.Path(), std::filesystem::path(upper)));
}

TEST_CASE(PathsEquivalent_TrailingSeparatorDoesNotMatter) {
    TempDir dir;
    std::filesystem::path withSlash(dir.Path().wstring() + L"\\");
    CHECK(PathsEquivalent(dir.Path(), withSlash));
}

TEST_CASE(PathsEquivalent_DifferentDirectoriesAreNotEquivalent) {
    TempDir dirA;
    TempDir dirB;
    CHECK(!PathsEquivalent(dirA.Path(), dirB.Path()));
}

TEST_CASE(PathsEquivalent_NonexistentPathIsFalseNotAnException) {
    TempDir base;
    CHECK(!PathsEquivalent(base.Path() / L"does-not-exist", base.Path()));
}

// Regression test: an earlier version of this code wrote
// std::wstring(ec.message().begin(), ec.message().end()) directly at each
// call site. std::error_code::message() returns by value, so that pattern
// calls it twice and takes a begin from one temporary std::string and an
// end from a *different* one — undefined behavior that only ever surfaced
// under the debug STL's iterator-range checking, as a blocking "Debug
// Assertion Failed: string iterators in range are from different
// containers" dialog, the first time any code path actually exercised it.
TEST_CASE(ToWideMessage_ProducesNonEmptyMessageForANonzeroErrorCode) {
    std::error_code ec = std::make_error_code(std::errc::no_such_file_or_directory);
    std::wstring msg = ToWideMessage(ec);
    CHECK(!msg.empty());
}
