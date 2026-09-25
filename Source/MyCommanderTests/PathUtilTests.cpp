#include "TestFixtures.h"
#include "TestFramework.h"

#include "PathUtil.h"

#include <system_error>

using mc::ResolveExistingDirectory;
using mc::ToWideMessage;
using test::TempDir;

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
