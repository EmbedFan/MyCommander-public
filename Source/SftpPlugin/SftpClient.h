#pragma once

// EXT-004 (D3, DEC-014): the SFTP client behind the shared remote panel, on
// the vendored libssh2 (Windows CNG crypto). The session is non-blocking:
// every wait is bounded by the connection's timeout and polls `cancel`, so
// Esc always works and a silent server can't freeze the app (SFTP-008).
// Transfers check `cancel` between 64 KB chunks and close cleanly, so the
// session survives an Esc. The host key is verified against MyCommander's own
// known_hosts and the user's OpenSSH one (DEC-015, SFTP-007); logging in uses
// a password (or keyboard-interactive), a private key file or the SSH agent
// (SFTP-005). The trace never contains a password, passphrase or key.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

#include "../PluginCommon/RemoteClient.h"
#include "SftpLogic.h"

#include <functional>
#include <optional>
#include <string>

typedef struct _LIBSSH2_SESSION LIBSSH2_SESSION;
typedef struct _LIBSSH2_SFTP LIBSSH2_SFTP;

namespace sftp {

struct HostKeyInfo {
    std::wstring host;
    uint16_t port = 22;
    std::wstring keyType;       // "ECDSA-SHA2-NISTP256"
    std::wstring fingerprint;   // "SHA256:..."
    HostKeyVerdict verdict = HostKeyVerdict::Unknown;
    std::wstring conflictFile;  // Changed: the known_hosts file with the old key
    int conflictLine = 0;       // ...and its line (1-based, 0 = unknown)
};

enum class HostKeyDecision { Reject, AcceptOnce, AcceptAndRemember };

// Asked for every Unknown or Changed key (a Changed key's answer is ignored:
// it is always refused). Trusted keys connect without asking.
using HostKeyFn = std::function<HostKeyDecision(const HostKeyInfo& info)>;

// A keyboard-interactive prompt that isn't the password (a one-time code,
// ...): the answer, or nullopt to cancel the login.
using PromptFn = std::function<std::optional<std::wstring>(const std::wstring& title, const std::wstring& prompt, bool echo)>;

struct KnownHostsFiles {
    std::wstring own;   // %APPDATA%\MyCommander\known_hosts (read and appended)
    std::wstring user;  // %USERPROFILE%\.ssh\known_hosts (read only, never written)
};
KnownHostsFiles DefaultKnownHostsFiles();

class Client final : public remote::Client {
public:
    Client(HostKeyFn hostKey, PromptFn prompt, KnownHostsFiles files = DefaultKnownHostsFiles());
    ~Client() override;
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;

    remote::Status Connect(const remote::Connection& connection, const std::wstring& secret,
                           const remote::CancelFn& cancel, std::wstring& error) override;
    void Disconnect() override;
    void SetTrace(remote::TraceFn trace) override { trace_ = std::move(trace); }
    bool Connected() const { return sftp_ != nullptr; }

    remote::Status HomeDirectory(std::wstring& path, const remote::CancelFn& cancel, std::wstring& error) override;
    remote::Status List(const std::wstring& dir, std::vector<remote::RemoteEntry>& entries,
                        const remote::CancelFn& cancel, std::wstring& error) override;
    remote::Status Download(const std::wstring& remotePath, const std::wstring& localPath, uint64_t sizeHint,
                            const remote::ProgressFn& progress, const remote::CancelFn& cancel,
                            std::wstring& error) override;
    remote::Status Upload(const std::wstring& localPath, const std::wstring& remotePath,
                          const remote::ProgressFn& progress, const remote::CancelFn& cancel,
                          std::wstring& error) override;
    remote::Status MakeRemoteDirectory(const std::wstring& path, const remote::CancelFn& cancel,
                                       std::wstring& error) override;
    remote::Status RemoveRemoteDirectory(const std::wstring& path, const remote::CancelFn& cancel,
                                         std::wstring& error) override;
    remote::Status DeleteRemoteFile(const std::wstring& path, const remote::CancelFn& cancel,
                                    std::wstring& error) override;
    remote::Status RenameRemote(const std::wstring& from, const std::wstring& to, const remote::CancelFn& cancel,
                                std::wstring& error) override;

    // For the keyboard-interactive callback (libssh2 calls a plain C function).
    void AnswerPrompts(const std::string& name, const std::string& instruction, int count, const void* promptData,
                       void* responseData);

private:
    HostKeyFn hostKey_;
    PromptFn prompt_;
    KnownHostsFiles files_;
    remote::TraceFn trace_;
    SOCKET socket_ = INVALID_SOCKET;
    LIBSSH2_SESSION* session_ = nullptr;
    LIBSSH2_SFTP* sftp_ = nullptr;
    int timeoutSeconds_ = 20;
    std::wstring secret_;  // only during Connect (keyboard-interactive answers)
    bool secretUsed_ = false;
    bool promptCancelled_ = false;

    void Trace(const std::wstring& line) const;
    // Repeats `op` while libssh2 says "would block", waiting on the socket.
    // Returns op's result, or kCancelled / kTimedOut (the session is then
    // dropped). A cancel lets the running request finish for up to 2 s.
    int Wait(const std::function<int()>& op, const remote::CancelFn& cancel);
    // Turns a libssh2 failure into a status and an error text; drops the
    // session when it is unusable.
    remote::Status Fail(int rc, const std::wstring& what, std::wstring& error);
    static remote::Status NotConnected(std::wstring& error);
    remote::Status OpenSocket(const remote::Connection& c, const remote::CancelFn& cancel, std::wstring& error);
    remote::Status VerifyHostKey(const remote::Connection& c, std::wstring& error);
    remote::Status Authenticate(const remote::Connection& c, const remote::CancelFn& cancel, std::wstring& error);
    remote::Status AuthenticateWithKey(const remote::Connection& c, const std::string& user,
                                       const remote::CancelFn& cancel, std::wstring& error);
    remote::Status AuthenticateWithAgent(const std::string& user, const remote::CancelFn& cancel, std::wstring& error);
    void DropSession();
};

}  // namespace sftp
