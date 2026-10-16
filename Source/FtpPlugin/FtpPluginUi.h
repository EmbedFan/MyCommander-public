#pragma once

// EXT-005: the FTP flavour of the shared remote-panel helpers
// (PluginCommon/RemoteUi.h, EXT-004), kept so FtpPluginTests.cpp can test them.

#include "../PluginCommon/RemoteUi.h"

namespace ftpui {

// FTP-003: downloads into "<target>.mcftp-part", then replaces the target.
inline bool DownloadReplacing(const std::wstring& target,
                              const std::function<bool(const std::wstring& tempPath)>& transfer) {
    return remote::DownloadReplacing(target, L".mcftp-part", transfer);
}

// "ftp://user@host[:port]/path" -- never a password (FTP-006); "anonymous"
// and port 21 are left out.
inline std::wstring FormatPanelTitle(const std::wstring& user, const std::wstring& host, uint16_t port,
                                     const std::wstring& path) {
    return remote::FormatPanelTitle(L"ftp", 21, user, host, port, path);
}

using remote::FormatSize;

}  // namespace ftpui
