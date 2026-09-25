#include "TestFramework.h"

#include "../MyCommanderSetup/SetupLogic.h"

using mc::BuildUninstallInfo;
using mc::DefaultInstallPath;
using mc::FormatUpgradeMessage;
using mc::IsWindowsVersionSupported;
using mc::UninstallInfo;

TEST_CASE(DefaultInstallPath_AppendsProgramsMyCommander_FEA0006) {
    CHECK(DefaultInstallPath(L"C:\\Users\\alice\\AppData\\Local") ==
          L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander");
}

TEST_CASE(DefaultInstallPath_TrimsTrailingBackslash_FEA0006) {
    CHECK(DefaultInstallPath(L"C:\\Users\\alice\\AppData\\Local\\") ==
          L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander");
}

TEST_CASE(IsWindowsVersionSupported_Windows10RTM_IsSupported_FEA0006) {
    CHECK(IsWindowsVersionSupported(10, 10240));
}

TEST_CASE(IsWindowsVersionSupported_Windows11_IsSupported_FEA0006) {
    // Windows 11 still reports major version 10 through RtlGetVersion; its
    // build numbers (22000+) are what actually distinguish it.
    CHECK(IsWindowsVersionSupported(10, 22000));
}

TEST_CASE(IsWindowsVersionSupported_Windows8Point1_IsNotSupported_FEA0006) {
    CHECK(!IsWindowsVersionSupported(6, 9600));
}

TEST_CASE(IsWindowsVersionSupported_Windows10BelowMinimumBuild_IsNotSupported_FEA0006) {
    CHECK(!IsWindowsVersionSupported(10, 10000));
}

TEST_CASE(IsWindowsVersionSupported_FutureMajorVersion_IsSupported_FEA0006) {
    CHECK(IsWindowsVersionSupported(11, 0));
}

TEST_CASE(BuildUninstallInfo_PopulatesEveryField_FEA0006) {
    UninstallInfo info =
        BuildUninstallInfo(L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander",
                            L"MyCommanderSetup.exe", L"0.1.60.245", 3343);

    CHECK(info.displayName == L"MyCommander");
    CHECK(info.displayVersion == L"0.1.60.245");
    CHECK(info.publisher == L"Attila Gallai");
    CHECK(info.installLocation == L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander");
    CHECK(info.estimatedSizeKB == 3343);
}

TEST_CASE(BuildUninstallInfo_UninstallStringPointsAtCopiedSetupExeWithUninstallFlag_FEA0006) {
    UninstallInfo info =
        BuildUninstallInfo(L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander",
                            L"MyCommanderSetup.exe", L"0.1.60.245", 0);

    // No surrounding quotes: QuoteCommandLineArgument (Process.h) only
    // quotes when actually needed (whitespace/quotes present), matching the
    // CommandLineToArgvW round-trip convention - a space-free path is
    // already unambiguous.
    CHECK(info.uninstallString ==
          L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander\\MyCommanderSetup.exe --uninstall");
}

TEST_CASE(BuildUninstallInfo_QuotesAnInstallPathContainingSpaces_FEA0006) {
    UninstallInfo info = BuildUninstallInfo(L"C:\\Program Files\\My Commander", L"MyCommanderSetup.exe",
                                             L"0.1.60.245", 0);

    CHECK(info.uninstallString == L"\"C:\\Program Files\\My Commander\\MyCommanderSetup.exe\" --uninstall");
}

TEST_CASE(BuildUninstallInfo_TrimsTrailingBackslashOnInstallPath_FEA0006) {
    UninstallInfo info = BuildUninstallInfo(L"C:\\Programs\\MyCommander\\", L"MyCommanderSetup.exe",
                                             L"0.1.60.245", 0);

    CHECK(info.uninstallString == L"C:\\Programs\\MyCommander\\MyCommanderSetup.exe --uninstall");
}

TEST_CASE(FormatUpgradeMessage_MentionsBothVersions_FEA0006) {
    std::wstring message = FormatUpgradeMessage(L"0.1.59.240", L"0.1.60.245");
    CHECK(message.find(L"0.1.59.240") != std::wstring::npos);
    CHECK(message.find(L"0.1.60.245") != std::wstring::npos);
}
