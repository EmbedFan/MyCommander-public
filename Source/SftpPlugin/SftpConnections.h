#pragma once

// SFTP-009 / SFTP-006: the SFTP flavour of the shared connections store
// (PluginCommon/RemoteConnections.h): %APPDATA%\MyCommander\sftp-connections.conf
// (never a password or passphrase; "auth" and "key-file" keys) and the
// Credential Manager target "MyCommander/SFTP/<name>".

#include "../PluginCommon/RemoteConnections.h"

namespace sftp {

inline const remote::StoreSpec& Store() {
    static const remote::StoreSpec spec{
        L"sftp-connections.conf",
        L"# MyCommander SFTP connections (SFTP-009). Passwords and key passphrases are never\n"
        L"# stored here; \"save-password = yes\" means one is kept in the Windows Credential Manager.\n"
        L"# auth = password | key | agent; key-file = the private key for \"key\".\n",
        L"MyCommander/SFTP/",
        22,
        L"",
        false,  // no passive mode
        true,   // auth + key-file
    };
    return spec;
}

}  // namespace sftp
