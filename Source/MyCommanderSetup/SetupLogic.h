#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <string>

namespace mc {

// FEA-0006/INST-003: the per-user install folder MyCommanderSetup proposes
// by default, given %LOCALAPPDATA%'s value (a pure function of that one
// string, so it's testable without calling SHGetKnownFolderPath itself).
// Needs no administrator rights to write to, unlike %ProgramFiles%.
std::wstring DefaultInstallPath(const std::wstring& localAppData);

// FEA-0006/INST-002: true if `majorVersion`/`buildNumber` (as RtlGetVersion
// reports them — the *true* OS version, not GetVersionEx's compatibility-
// shim lie) meet MyCommander's minimum supported Windows version (Windows
// 10, build 10240 "1507" — matching README's documented
// "Requirements: Windows 10/11"). Windows 11 still reports major version 10
// through this API; its build numbers (22000+) already clear this bar, so
// no separate "is this actually 11" check is needed.
bool IsWindowsVersionSupported(DWORD majorVersion, DWORD buildNumber);

struct UninstallInfo {
    std::wstring displayName;
    std::wstring displayVersion;
    std::wstring publisher;
    std::wstring installLocation;
    std::wstring uninstallString;
    DWORD estimatedSizeKB = 0;
};

// FEA-0006/INST-005: builds the HKCU\...\Uninstall registry value set for
// the app installed at `installPath`, whose copied setup exe is
// `setupExeName` (just the file name — always alongside MyCommander.exe in
// `installPath`, reused as the uninstaller) at app version `version`
// (MC_VERSION_STR-shaped) with total installed size `estimatedSizeKB`.
// `uninstallString` is built via `mc::QuoteCommandLineArgument`
// (Process.h) so a space in `installPath` can never break it.
UninstallInfo BuildUninstallInfo(const std::wstring& installPath, const std::wstring& setupExeName,
                                  const std::wstring& version, DWORD estimatedSizeKB);

// FEA-0006 §5: the message shown when an existing installation is detected
// before a fresh install/upgrade proceeds.
std::wstring FormatUpgradeMessage(const std::wstring& installedVersion, const std::wstring& newVersion);

}  // namespace mc
