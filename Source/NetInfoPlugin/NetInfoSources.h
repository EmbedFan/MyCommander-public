#pragma once

// FEA-0010: the real Windows calls behind INetInfoSource. Read-only: nothing here connects to, disconnects from
// or changes a network, reads a saved key, or stores or logs a credential (NET-010).

#include "NetInfoScanner.h"

#include <memory>

namespace netinfo {

std::shared_ptr<INetInfoSource> MakeWindowsSource();

// Pure helpers the source uses (and NetInfoModelTests checks).
// "nas.fritz.box" -> "nas"; an address or a name without dots is returned as it is.
std::wstring ShortHostName(const std::wstring& host);
// A friendly text for the Win32 / network-management error behind a failed share list.
std::wstring ShareErrorText(unsigned long error);

}  // namespace netinfo
