#pragma once

// EXT-004 (D4) / FTP-007 / SFTP-009: saved connections in a DEC-007-style
// text file under %APPDATA%\MyCommander (never any password or passphrase),
// and the opt-in secret store in the Windows Credential Manager (DEC-013,
// FTP-006/SFTP-006). One implementation for every protocol, described by a
// StoreSpec. Parse/Format are pure.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "RemoteTypes.h"

#include <optional>
#include <string>
#include <vector>

namespace remote {

struct StoreSpec {
    const wchar_t* fileName;          // "ftp-connections.conf"
    const wchar_t* header;            // the comment lines written at the top
    const wchar_t* credentialPrefix;  // "MyCommander/FTP/" (+ connection name)
    uint16_t defaultPort;             // 21 / 22
    const wchar_t* defaultUser;       // "anonymous" / ""
    bool hasPassive;                  // FTP: "passive = yes|no"
    bool hasAuth;                     // SFTP: "auth = password|key|agent", "key-file = ..."
};

// A connection with the protocol's defaults (port, user).
Connection NewConnection(const StoreSpec& spec);

// [name] blocks with host/port/user/folder/timeout/save-password lines (plus
// the protocol's own keys). Unknown keys and any password/passphrase line
// produce a warning and are ignored; a block without a host is dropped with
// a warning.
std::vector<Connection> ParseConnections(const StoreSpec& spec, const std::wstring& text,
                                         std::vector<std::wstring>& warnings);
std::wstring FormatConnections(const StoreSpec& spec, const std::vector<Connection>& connections);

std::wstring ConnectionsFilePath(const StoreSpec& spec);  // empty if %APPDATA% can't be resolved
std::vector<Connection> LoadConnections(const StoreSpec& spec, std::vector<std::wstring>& warnings);
bool SaveConnections(const StoreSpec& spec, const std::vector<Connection>& connections);

// Windows Credential Manager, target "<credentialPrefix><name>" (generic,
// per-user, DPAPI-protected, no elevation).
bool StorePassword(const StoreSpec& spec, const std::wstring& connectionName, const std::wstring& user,
                   const std::wstring& password);
std::optional<std::wstring> ReadPassword(const StoreSpec& spec, const std::wstring& connectionName);
void ForgetPassword(const StoreSpec& spec, const std::wstring& connectionName);

}  // namespace remote
