#pragma once

// EXT-004 (D2): the types shared by every remote-file-system plugin (FTP,
// SFTP) and the shared remote panel (RemotePanel.h). Header-only, no host
// dependency. Compiled into each plugin DLL separately -- there is no ABI
// between plugins.

#include <cstdint>
#include <functional>
#include <string>

namespace remote {

using CancelFn = std::function<bool()>;
using ProgressFn = std::function<void(uint64_t done, uint64_t total)>;
using TraceFn = std::function<void(const std::wstring& line)>;

// Ok; Failed (the server refused, the error text says why); Cancelled (Esc);
// ConnectionLost (the session is gone and must be reconnected).
enum class Status { Ok, Failed, Cancelled, ConnectionLost };

struct RemoteEntry {
    std::wstring name;
    bool isDirectory = false;
    bool isLink = false;
    uint64_t size = 0;
    uint64_t modifiedUtc = 0;  // FILETIME ticks, 0 = unknown
    std::wstring permissions;  // "rwxr-xr-x", may be empty
};

// How an SFTP connection logs in (SFTP-005). FTP always uses Password.
enum class AuthMethod { Password, KeyFile, Agent };

// One saved (or ad-hoc) connection. Never holds a password (FTP-006/SFTP-006).
struct Connection {
    std::wstring name;
    std::wstring host;
    uint16_t port = 21;
    std::wstring user = L"anonymous";
    std::wstring folder;  // initial folder, empty = the server's default
    int timeoutSeconds = 20;
    bool savePassword = false;  // a password/passphrase is in the Credential Manager
    bool passive = true;                       // FTP only
    AuthMethod auth = AuthMethod::Password;    // SFTP only
    std::wstring keyFile;                      // SFTP only, for AuthMethod::KeyFile
};

// Server paths always use '/'.
inline std::wstring JoinRemotePath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return L"/" + name;
    return dir.back() == L'/' ? dir + name : dir + L"/" + name;
}

// "/a/b" -> "/a", "/a" -> "/", "/" -> "/".
inline std::wstring ParentRemotePath(const std::wstring& path) {
    std::wstring p = path;
    while (p.size() > 1 && p.back() == L'/') p.pop_back();
    size_t slash = p.find_last_of(L'/');
    if (slash == std::wstring::npos || slash == 0) return L"/";
    return p.substr(0, slash);
}

}  // namespace remote
