#include "TestFramework.h"

#include "NetShares.h"

using mc::DefaultShareName;
using mc::IsValidShareName;

TEST_CASE(IsValidShareName_OrdinaryName_IsAccepted_NAV015) {
    CHECK(IsValidShareName(L"Backup"));
    CHECK(IsValidShareName(L"Shared Docs"));
    CHECK(IsValidShareName(L"Hidden$"));  // trailing $ hides it from browsing, still a valid name
}

TEST_CASE(IsValidShareName_EmptyOrTooLong_IsRejected_NAV015) {
    CHECK(!IsValidShareName(L""));
    CHECK(!IsValidShareName(std::wstring(81, L'a')));
    CHECK(IsValidShareName(std::wstring(80, L'a')));
}

TEST_CASE(IsValidShareName_ForbiddenCharacters_AreRejected_NAV015) {
    CHECK(!IsValidShareName(L"a\\b"));
    CHECK(!IsValidShareName(L"a/b"));
    CHECK(!IsValidShareName(L"a:b"));
    CHECK(!IsValidShareName(L"a*b"));
    CHECK(!IsValidShareName(L"a?b"));
    CHECK(!IsValidShareName(L"a\"b"));
    CHECK(!IsValidShareName(L"a<b"));
    CHECK(!IsValidShareName(L"a>b"));
    CHECK(!IsValidShareName(L"a|b"));
}

TEST_CASE(DefaultShareName_UsesTheFolderSOwnName_NAV015) {
    CHECK(DefaultShareName(L"C:\\Users\\me\\Documents") == L"Documents");
    CHECK(DefaultShareName(L"C:\\Backup") == L"Backup");
}

TEST_CASE(SharePathMatches_IgnoresCaseSeparatorsAndPrefixes_IS0022) {
    using mc::SharePathMatches;
    CHECK(SharePathMatches(L"C:\\Work\\Projects", L"C:\\Work\\Projects"));
    CHECK(SharePathMatches(L"C:\\Work\\Projects", L"c:\\work\\PROJECTS"));
    CHECK(SharePathMatches(L"C:\\Work\\Projects", L"C:\\Work\\Projects\\"));
    CHECK(SharePathMatches(L"C:\\Work\\Projects\\", L"C:\\Work\\Projects"));
    CHECK(SharePathMatches(L"C:\\Work\\Projects", L"\\\\?\\C:\\Work\\Projects"));
    CHECK(SharePathMatches(L"\\\\srv\\share\\a", L"\\\\?\\UNC\\srv\\share\\a"));
    CHECK(SharePathMatches(L"C:\\", L"C:"));
    CHECK(SharePathMatches(L"C:\\", L"C:\\"));
}

TEST_CASE(SharePathMatches_ParentChildAndEmpty_AreNotMatches_IS0022) {
    using mc::SharePathMatches;
    CHECK(!SharePathMatches(L"C:\\Work\\Projects", L"C:\\Work"));
    CHECK(!SharePathMatches(L"C:\\Work", L"C:\\Work\\Projects"));
    CHECK(!SharePathMatches(L"C:\\Work\\Projects", L"C:\\Work\\Project"));
    CHECK(!SharePathMatches(L"", L"C:\\Work"));
    CHECK(!SharePathMatches(L"C:\\Work", L""));
}

TEST_CASE(SharedFolderSet_ContainsOnlyShareRoots_AndSkipsSpecialShares_IS0022) {
    std::vector<mc::ShareInfo> shares(4);
    shares[0].shareName = L"Projects";
    shares[0].path = L"C:\\Work\\Projects";
    shares[1].shareName = L"Projects2";  // a second name for the same folder
    shares[1].path = L"c:\\work\\projects\\";
    shares[2].shareName = L"C$";
    shares[2].path = L"C:\\";
    shares[2].isSpecial = true;
    shares[3].shareName = L"Docs";
    shares[3].path = L"D:\\Docs";
    mc::SharedFolderSet set(shares);
    CHECK(set.Size() == 2);
    CHECK(set.Contains(L"C:\\Work\\Projects"));
    CHECK(set.Contains(L"C:\\WORK\\projects"));
    CHECK(set.Contains(L"D:\\Docs"));
    CHECK(!set.Contains(L"C:\\Work"));
    CHECK(!set.Contains(L"C:\\Work\\Projects\\App"));
    CHECK(!set.Contains(L"C:\\"));  // C$ is administrative
    CHECK(!mc::SharedFolderSet().Contains(L"C:\\Work"));
    CHECK(mc::SharedFolderSet().Empty());
}

TEST_CASE(ShareTargetFolder_CursorFolderElsePanelFolder_IS0022) {
    using mc::ShareTargetFolder;
    CHECK(ShareTargetFolder(L"C:\\Work", L"Projects", true) == std::filesystem::path(L"C:\\Work\\Projects"));
    CHECK(ShareTargetFolder(L"C:\\Work", L"..", true) == std::filesystem::path(L"C:\\Work"));
    CHECK(ShareTargetFolder(L"C:\\Work", L"a.txt", false) == std::filesystem::path(L"C:\\Work"));
    CHECK(ShareTargetFolder(L"C:\\Work", L"", true) == std::filesystem::path(L"C:\\Work"));
}

TEST_CASE(EnumerateAllShares_ReadsTheMachineWithoutChangingIt_IS0022) {
    std::vector<mc::ShareInfo> all;
    if (!mc::EnumerateAllShares(all)) return;  // no server service on this machine: nothing to check
    for (const auto& s : all) CHECK(!s.shareName.empty());
    // the Unshare lookup never offers an administrative share
    for (const auto& s : all) {
        if (!s.isSpecial) continue;
        for (const auto& found : mc::EnumerateSharesFor(s.path)) CHECK(found.shareName != s.shareName);
    }
}

TEST_CASE(DefaultShareName_DriveRoot_FallsBackToTheRootName_NAV015) {
    std::wstring name = DefaultShareName(L"C:\\");
    CHECK(!name.empty());  // a drive root has no filename() of its own
}
