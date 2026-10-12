#pragma once

// EXT-004 (D2): what the shared remote panel needs from a protocol. The FTP
// plugin adapts ftp::Client to it, the SFTP plugin sftp::Client; the panel
// harness (RemotePanelTests.cpp) uses an in-memory fake. Every call is
// bounded and polls `cancel` (FTP-009/SFTP-008).

#include "RemoteTypes.h"

#include <vector>

namespace remote {

class Client {
public:
    virtual ~Client() = default;

    // Opens the session and logs in. `secret` is the password or the key's
    // passphrase (may be empty). Also used to reconnect after ConnectionLost.
    virtual Status Connect(const Connection& connection, const std::wstring& secret, const CancelFn& cancel,
                           std::wstring& error) = 0;
    virtual void Disconnect() = 0;
    virtual void SetTrace(TraceFn trace) = 0;  // Debug-log lines; never a secret

    // The folder a session starts in (FTP: PWD, SFTP: realpath(".")).
    virtual Status HomeDirectory(std::wstring& path, const CancelFn& cancel, std::wstring& error) = 0;
    virtual Status List(const std::wstring& dir, std::vector<RemoteEntry>& entries, const CancelFn& cancel,
                        std::wstring& error) = 0;
    // Writes the remote file into `localPath` (created/truncated); the panel
    // downloads to a temporary name and renames on success.
    virtual Status Download(const std::wstring& remotePath, const std::wstring& localPath, uint64_t sizeHint,
                            const ProgressFn& progress, const CancelFn& cancel, std::wstring& error) = 0;
    virtual Status Upload(const std::wstring& localPath, const std::wstring& remotePath, const ProgressFn& progress,
                          const CancelFn& cancel, std::wstring& error) = 0;
    // Named *Remote* so <windows.h>'s DeleteFile/RemoveDirectory macros can't clash.
    virtual Status MakeRemoteDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error) = 0;
    virtual Status RemoveRemoteDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error) = 0;
    virtual Status DeleteRemoteFile(const std::wstring& path, const CancelFn& cancel, std::wstring& error) = 0;
    virtual Status RenameRemote(const std::wstring& from, const std::wstring& to, const CancelFn& cancel,
                                std::wstring& error) = 0;
};

}  // namespace remote
