#pragma once

// FTP-007 / FTP-006: saved connections (%APPDATA%\MyCommander\ftp-connections.conf,
// DEC-007-style text, never any password) and the opt-in password store in
// the Windows Credential Manager (DEC-013). Since EXT-004 this is the FTP
// flavour of the shared PluginCommon/RemoteConnections store.

#include "../PluginCommon/RemoteConnections.h"

namespace ftp {

using Connection = remote::Connection;

// ftp-connections.conf, "MyCommander/FTP/<name>", port 21, user anonymous, passive.
const remote::StoreSpec& Store();

// [name] blocks with host/port/user/folder/passive/timeout/save-password
// lines. Unknown keys and any password line produce a warning and are
// ignored; a block without a host is dropped with a warning.
inline std::vector<Connection> ParseConnections(const std::wstring& text, std::vector<std::wstring>& warnings) {
    return remote::ParseConnections(Store(), text, warnings);
}
inline std::wstring FormatConnections(const std::vector<Connection>& connections) {
    return remote::FormatConnections(Store(), connections);
}

inline std::wstring ConnectionsFilePath() { return remote::ConnectionsFilePath(Store()); }
inline std::vector<Connection> LoadConnections(std::vector<std::wstring>& warnings) {
    return remote::LoadConnections(Store(), warnings);
}
inline bool SaveConnections(const std::vector<Connection>& connections) {
    return remote::SaveConnections(Store(), connections);
}

inline bool StorePassword(const std::wstring& connectionName, const std::wstring& user, const std::wstring& password) {
    return remote::StorePassword(Store(), connectionName, user, password);
}
inline std::optional<std::wstring> ReadPassword(const std::wstring& connectionName) {
    return remote::ReadPassword(Store(), connectionName);
}
inline void ForgetPassword(const std::wstring& connectionName) { remote::ForgetPassword(Store(), connectionName); }

}  // namespace ftp
