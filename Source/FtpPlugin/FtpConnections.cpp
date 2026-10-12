#include "FtpConnections.h"

namespace ftp {

const remote::StoreSpec& Store() {
    static const remote::StoreSpec spec{
        L"ftp-connections.conf",
        L"# MyCommander FTP connections (FTP-007). Passwords are never stored here;\n"
        L"# \"save-password = yes\" means one is kept in the Windows Credential Manager.\n",
        L"MyCommander/FTP/",
        21,
        L"anonymous",
        true,   // passive
        false,  // no SSH auth keys
    };
    return spec;
}

}  // namespace ftp
