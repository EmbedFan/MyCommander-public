#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace mc {

// NAV-013: Disk Label (Commands menu, menu-only). `SetVolumeLabelDirect`
// is the plain, unelevated attempt main.cpp tries first; a fixed/system
// drive typically answers ERROR_ACCESS_DENIED, at which point main.cpp
// offers Elevate & Retry (SEC-003) through `Elevation.h`'s
// ElevatedAction::SetVolumeLabel, whose elevated child calls
// `SetVolumeLabelElevated` -- the same `*PathElevated`/`err`+`win32Error`
// shape FileOps.h's own elevated primitives already use.
bool SetVolumeLabelDirect(const std::filesystem::path& root, const std::wstring& label, DWORD& win32Error);
bool SetVolumeLabelElevated(const std::filesystem::path& root, const std::wstring& label, std::wstring& err,
                            DWORD& win32Error);

// NAV-014: Network Connection (Network menu, menu-only). Pure data/formatting
// below; the WNet* calls that actually touch the system are grouped at the
// bottom, no elevation needed for any of them (SEC-002: mapping/disconnecting
// a drive is an ordinary per-user operation, same as the OS's own net use).

struct NetworkConnectionInfo {
    std::wstring localDevice;  // "Z:", or empty for a UNC connection with no drive letter
    std::wstring remoteName;   // "\\server\share"
    bool persistent = false;
};

// "Z:  \\server\share  (persistent)" -- pure, as shown in the connections list.
std::wstring FormatConnectionRow(const NetworkConnectionInfo& info);

// A syntactically well-formed "\\server\share" (or deeper, "\\server\share\sub"):
// two leading backslashes, then a non-empty server and a non-empty share
// component. Doesn't check the server/share actually exist -- that's what
// the real connection attempt below is for.
bool IsValidUncPath(const std::wstring& path);

std::vector<NetworkConnectionInfo> EnumerateNetworkConnections();
// `username`/`password` may be empty (the current user's own credentials).
// `persistent` sets CONNECT_UPDATE_PROFILE ("reconnect at sign-in") --
// Windows' own Credential Manager integration then decides whether to keep
// the credential across sign-ins; this process never stores it itself
// (DEC-013's own precedent), and `password` is never logged.
DWORD MapNetworkDrive(const std::wstring& driveLetter, const std::wstring& uncPath, bool persistent,
                      const std::wstring& username, const std::wstring& password);
DWORD DisconnectNetworkDrive(const std::wstring& localDeviceOrUncPath, bool force);

} // namespace mc
