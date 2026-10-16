#include "TestFramework.h"

#include "NetDrives.h"

using mc::FormatConnectionRow;
using mc::IsValidUncPath;
using mc::NetworkConnectionInfo;

TEST_CASE(FormatConnectionRow_WithDriveLetter_ShowsLocalAndRemote_NAV014) {
    NetworkConnectionInfo info{L"Z:", L"\\\\server\\share", false};
    CHECK(FormatConnectionRow(info) == L"Z:  \\\\server\\share");
}

TEST_CASE(FormatConnectionRow_Persistent_AppendsTheMarker_NAV014) {
    NetworkConnectionInfo info{L"Z:", L"\\\\server\\share", true};
    CHECK(FormatConnectionRow(info) == L"Z:  \\\\server\\share  (persistent)");
}

TEST_CASE(FormatConnectionRow_NoDriveLetter_StillShowsTheRemoteName_NAV014) {
    NetworkConnectionInfo info{L"", L"\\\\server\\share", false};
    std::wstring row = FormatConnectionRow(info);
    CHECK(row.find(L"\\\\server\\share") != std::wstring::npos);
}

TEST_CASE(IsValidUncPath_WellFormed_IsAccepted_NAV014) {
    CHECK(IsValidUncPath(L"\\\\server\\share"));
    CHECK(IsValidUncPath(L"\\\\server\\share\\sub\\folder"));
}

TEST_CASE(IsValidUncPath_MissingShareOrServer_IsRejected_NAV014) {
    CHECK(!IsValidUncPath(L"\\\\server"));
    CHECK(!IsValidUncPath(L"\\\\server\\"));
    CHECK(!IsValidUncPath(L"\\\\"));
    CHECK(!IsValidUncPath(L"server\\share"));
    CHECK(!IsValidUncPath(L"C:\\local\\path"));
    CHECK(!IsValidUncPath(L""));
}
