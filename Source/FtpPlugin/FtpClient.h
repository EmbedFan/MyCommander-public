#pragma once

// EXT-005 / DEC-012: a minimal FTP client on Winsock2 -- session (USER/PASS,
// FEAT, OPTS UTF8, TYPE I), passive EPSV/PASV or active EPRT/PORT data
// connections, MLSD or LIST listings, RETR/STOR, and the small file
// operations. Every socket wait is bounded by the connection's timeout and
// polls `cancel` every 100 ms (FTP-009), so Esc always works and a silent
// server can never freeze the app. Single-threaded, like the rest of the
// plugin. No host dependency: FtpPluginTests.cpp drives it against a fake
// server on the loopback interface.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "FtpParse.h"

#include <functional>
#include <string>
#include <vector>

namespace ftp {

using remote::CancelFn;
using remote::ProgressFn;
using remote::TraceFn;

struct ClientOptions {
    std::wstring host;
    uint16_t port = 21;
    std::wstring user = L"anonymous";
    std::wstring password;
    bool passive = true;
    int timeoutSeconds = 20;
};

using Status = remote::Status;

class Client {
public:
    Client();
    ~Client();
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    // Every exchange, with PASS masked (FTP-006), for the Debug log.
    void SetTrace(TraceFn trace) { trace_ = std::move(trace); }

    Status Connect(const ClientOptions& options, const CancelFn& cancel, std::wstring& error);
    void Disconnect();  // QUIT (best effort) and close
    bool Connected() const { return control_ != INVALID_SOCKET; }
    bool Utf8() const { return utf8_; }
    bool UsesMlsd() const { return mlsd_; }
    const ClientOptions& Options() const { return options_; }

    Status CurrentDirectory(std::wstring& path, const CancelFn& cancel, std::wstring& error);
    Status ChangeDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error);
    // Lists `dir` (CWD there first, then MLSD or LIST).
    Status List(const std::wstring& dir, std::vector<RemoteEntry>& entries, const CancelFn& cancel, std::wstring& error);
    // Writes the remote file into `localPath` (created/truncated). The caller
    // downloads to a temporary name and renames on success (FTP-003).
    Status Download(const std::wstring& remotePath, const std::wstring& localPath, uint64_t sizeHint,
                    const ProgressFn& progress, const CancelFn& cancel, std::wstring& error);
    Status Upload(const std::wstring& localPath, const std::wstring& remotePath, const ProgressFn& progress,
                  const CancelFn& cancel, std::wstring& error);
    Status MakeRemoteDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error);
    Status RemoveRemoteDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error);
    Status DeleteRemoteFile(const std::wstring& path, const CancelFn& cancel, std::wstring& error);
    Status RenameRemote(const std::wstring& from, const std::wstring& to, const CancelFn& cancel, std::wstring& error);

private:
    SOCKET control_ = INVALID_SOCKET;
    ReplyReader reader_;
    ClientOptions options_;
    bool utf8_ = false;
    bool mlsd_ = false;
    bool epsvWorks_ = true;
    TraceFn trace_;

    void Trace(const std::wstring& line) const;
    void CloseControl();
    Status SendLine(const std::string& line, const CancelFn& cancel, std::wstring& error);
    Status ReadReply(Reply& reply, const CancelFn& cancel, std::wstring& error, int timeoutSeconds = -1);
    Status Command(const std::string& line, Reply& reply, const CancelFn& cancel, std::wstring& error);
    Status SimpleCommand(const std::string& verb, const std::wstring& path, int okClass, const CancelFn& cancel,
                         std::wstring& error);
    // Opens the data channel: a connected socket (passive) or a listening
    // one plus the PORT/EPRT exchange (active).
    Status OpenData(SOCKET& socketOut, bool& isListener, const CancelFn& cancel, std::wstring& error);
    // Sends `command`, accepts the data connection, runs `body` on it, then
    // reads the final reply. On cancel: ABOR, drain, Status::Cancelled.
    Status Transfer(const std::string& command, const std::function<Status(SOCKET)>& body, const CancelFn& cancel,
                    std::wstring& error);
    // Reads the server's answer(s) to ABOR so the next command's reply isn't
    // one behind: usually two (the transfer's 426, or 226 if it had already
    // finished, then the ABOR's 226/225), sometimes one.
    void DrainAfterAbort();
    std::string Encode(const std::wstring& path) const { return EncodeName(path, utf8_); }
};

}  // namespace ftp
