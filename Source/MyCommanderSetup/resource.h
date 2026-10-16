#pragma once

// Resource IDs for MyCommanderSetup.rc's embedded RCDATA payload (FEA-0006
// §4) — the files main.cpp's ExtractResourceToFile extracts into the
// install folder. Not a generated file (no dialog/menu editor involved);
// plain manually-assigned IDs, same as the rest of this small project.
#define IDR_PAYLOAD_MYCOMMANDER 101
#define IDR_PAYLOAD_USERGUIDE 102
#define IDR_PAYLOAD_LICENSE 103
#define IDR_PAYLOAD_ZIPPLUGIN 104
#define IDR_PAYLOAD_FTPPLUGIN 105  // EXT-005 / FTP-012
#define IDR_PAYLOAD_SFTPPLUGIN 106  // EXT-004 / SFTP-010
#define IDR_PAYLOAD_NETINFOPLUGIN 108  // FEA-0010
#define IDR_PAYLOAD_ICON 107        // IS-0021: mycommander.ico, installed beside MyCommander.exe
#define IDI_APPICON 1               // IS-0021: the installer's own icon resource
