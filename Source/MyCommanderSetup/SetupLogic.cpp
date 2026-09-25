#include "SetupLogic.h"

#include "../MyCommander/Process.h"

namespace mc {

std::wstring DefaultInstallPath(const std::wstring& localAppData) {
    std::wstring base = localAppData;
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    return base + L"\\Programs\\MyCommander";
}

bool IsWindowsVersionSupported(DWORD majorVersion, DWORD buildNumber) {
    constexpr DWORD kMinMajorVersion = 10;
    constexpr DWORD kMinBuildNumber = 10240;
    if (majorVersion > kMinMajorVersion) return true;
    return majorVersion == kMinMajorVersion && buildNumber >= kMinBuildNumber;
}

UninstallInfo BuildUninstallInfo(const std::wstring& installPath, const std::wstring& setupExeName,
                                  const std::wstring& version, DWORD estimatedSizeKB) {
    UninstallInfo info;
    info.displayName = L"MyCommander";
    info.displayVersion = version;
    info.publisher = L"Attila Gallai";
    info.installLocation = installPath;

    std::wstring base = installPath;
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    std::wstring copiedSetupPath = base + L"\\" + setupExeName;
    info.uninstallString = QuoteCommandLineArgument(copiedSetupPath) + L" --uninstall";

    info.estimatedSizeKB = estimatedSizeKB;
    return info;
}

std::wstring FormatUpgradeMessage(const std::wstring& installedVersion, const std::wstring& newVersion) {
    return L"MyCommander " + installedVersion + L" is already installed. Install " + newVersion +
           L" over it?";
}

}  // namespace mc
