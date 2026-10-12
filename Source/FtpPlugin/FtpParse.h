#pragma once

// EXT-005 / FTP-008: the FTP plugin's pure protocol helpers -- reply framing,
// passive-mode addresses, directory-listing lines, FEAT/MDTM, server paths,
// name encoding, and log masking. No sockets, no host calls, so the whole
// file is exercised by FtpPluginTests.cpp without a network.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "../PluginCommon/RemoteTypes.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace ftp {

// One complete server reply. `text` holds every line of a multi-line reply
// joined by '\n' (raw bytes, as sent). `code` is 0 for a malformed reply.
struct Reply {
    int code = 0;
    std::string text;
    bool Positive() const { return code >= 100 && code < 400; }
};

// Accumulates control-connection bytes and yields complete replies --
// single-line "123 text" or multi-line "123-first ... 123 last" (RFC 959 §4.2).
class ReplyReader {
public:
    void Feed(const char* data, size_t length) { buffer_.append(data, length); }
    std::optional<Reply> Next();
    void Clear() { buffer_.clear(); }

private:
    std::string buffer_;
};

// "227 Entering Passive Mode (h1,h2,h3,h4,p1,p2)" -> dotted host + port.
bool ParsePasvReply(const std::string& text, std::string& host, uint16_t& port);
// "229 Entering Extended Passive Mode (|||port|)" -> port.
bool ParseEpsvReply(const std::string& text, uint16_t& port);

using RemoteEntry = remote::RemoteEntry;  // shared with the SFTP plugin (EXT-004)

// One MLSD line: "fact=value;fact=value; name". Returns nullopt for the
// current/parent directory entries (type=cdir/pdir) and malformed lines.
std::optional<RemoteEntry> ParseMlsdLine(const std::string& rawLine, bool utf8);

// One LIST line in the Unix "ls -l" format or the Windows/IIS
// "MM-DD-YY hh:mmAM <DIR>|size name" format. `currentYear` resolves Unix
// dates given as "Mon dd hh:mm" (no year). "total N" lines, "." and ".."
// return nullopt. An unrecognized but non-empty line returns its text as a
// plain file name so nothing is ever silently hidden.
std::optional<RemoteEntry> ParseListLine(const std::string& rawLine, int currentYear, bool utf8);

// FEAT reply -> upper-cased feature keywords ("MLSD", "UTF8", "EPSV", "SIZE", ...).
std::vector<std::string> ParseFeatures(const std::string& text);
bool HasFeature(const std::vector<std::string>& features, const char* name);

// MDTM / MLSD "YYYYMMDDHHMMSS[.fff]" (UTC) -> FILETIME ticks.
std::optional<uint64_t> ParseFtpTime(const std::string& rawText);

// Server paths always use '/' (PluginCommon/RemoteTypes.h).
using remote::JoinRemotePath;
using remote::ParentRemotePath;
// "257 \"/home/user\" is the current directory" -> /home/user ("" doubling unescaped).
std::optional<std::string> ParsePwdReply(const std::string& text);

// Names travel as UTF-8 when the server supports it, else the ANSI code page.
std::wstring DecodeName(const std::string& bytes, bool utf8);
std::string EncodeName(const std::wstring& name, bool utf8);

// A command line safe to log: "PASS secret" -> "PASS ****".
std::string MaskForLog(const std::string& commandLine);

// "ftp://user:pass@host:port/path" (scheme optional) split into parts.
struct FtpUrl {
    std::wstring host;
    uint16_t port = 21;
    std::wstring user;
    std::wstring password;
    std::wstring path;
};
std::optional<FtpUrl> ParseFtpUrl(const std::wstring& rawText);

}  // namespace ftp
