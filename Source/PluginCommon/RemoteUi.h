#pragma once

// EXT-004 (D2): small UI-side helpers of the remote panel that don't need the
// host, kept apart so the plugin harnesses can test them.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <string>

namespace remote {

// FTP-003 / SFTP-003: downloads into "<target><partSuffix>" next to the
// target and only then replaces the target, so a failed or cancelled
// transfer never leaves a partial file or destroys an existing one.
// `transfer` writes the temporary path and returns success; on failure the
// temporary file is deleted.
bool DownloadReplacing(const std::wstring& target, const std::wstring& partSuffix,
                       const std::function<bool(const std::wstring& tempPath)>& transfer);

// "<scheme>://user@host[:port]/path" for the panel header -- never a
// password; an empty or "anonymous" user and the default port are left out.
std::wstring FormatPanelTitle(const std::wstring& scheme, uint16_t defaultPort, const std::wstring& user,
                              const std::wstring& host, uint16_t port, const std::wstring& path);

// "1.2 MB"-style size for the panel's size column.
std::wstring FormatSize(uint64_t bytes);

// "2026-10-08 14:05" (local time) from FILETIME ticks; empty for 0.
std::wstring FormatTime(uint64_t fileTimeUtc);

// Case-insensitive '*' / '?' match for Num+ / Num- selection masks.
bool WildcardMatch(const wchar_t* name, const wchar_t* pattern);

}  // namespace remote
