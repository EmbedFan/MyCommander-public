#pragma once

// EXT-004: the SFTP plugin's pure helpers -- host-key fingerprints and the
// known-hosts verdict (DEC-015, SFTP-007), private-key file inspection
// (SFTP-005), sftp:// addresses, SFTP status texts, Unix modes and times.
// No libssh2, no sockets, no host calls: SftpLogicTests.cpp tests all of it.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace sftp {

// ---------------------------------------------------------------------------
// Host keys (DEC-015)

// "SHA256:<base64, no padding>" from the 32-byte SHA-256 of the host key --
// the same text `ssh-keygen -l` and `ssh` show.
std::wstring FormatSha256Fingerprint(const unsigned char* hash, size_t length);

// What one known_hosts file says about the presented key.
enum class KnownState { NotFound, Match, Mismatch };
enum class HostKeyVerdict { Trusted, Unknown, Changed };

// MyCommander's own known_hosts and the user's OpenSSH one (read-only):
// a mismatch in either is a changed key (refused, no override); otherwise a
// match in either is trusted; otherwise the host is unknown (ask).
HostKeyVerdict CombineKnownHosts(KnownState own, KnownState user);

// The known_hosts host field: "host", or "[host]:port" off port 22.
std::string KnownHostsName(const std::string& host, uint16_t port);

// ---------------------------------------------------------------------------
// Private key files (SFTP-005)

enum class KeyFormat {
    Unreadable,   // missing or not a key file
    PemRsa,       // "-----BEGIN RSA PRIVATE KEY-----" (optionally encrypted)
    OpenSsh,      // "-----BEGIN OPENSSH PRIVATE KEY-----"
    OtherPem,     // PKCS#8, PEM EC/DSA, PuTTY .ppk, ...
};

struct KeyFileInfo {
    KeyFormat format = KeyFormat::Unreadable;
    bool encrypted = false;
    std::string keyType;          // "ssh-rsa", "ecdsa-sha2-nistp256", "ssh-ed25519" (OpenSSH format)
    std::string publicKeyBase64;  // the public key blob (OpenSSH format), for a ".pub" line
};

KeyFileInfo InspectKeyFile(const std::string& text);

// Whether the built-in SSH library (libssh2 with Windows CNG, DEC-014) can
// sign with this key file, and if not, why and what to do instead. Empty =
// usable. Supported: PEM RSA (with or without a passphrase) and OpenSSH-format
// ECDSA without a passphrase; everything else works through the SSH agent.
std::wstring KeyFileProblem(const KeyFileInfo& info, const std::wstring& path);

// The first of id_ecdsa, id_rsa, id_ed25519 that exists in `sshDir`, or "".
std::wstring DefaultKeyFile(const std::wstring& sshDir, const std::function<bool(const std::wstring&)>& exists);

// ---------------------------------------------------------------------------
// Addresses, statuses, attributes

// "sftp://user:pass@host:port/path" (scheme optional; "ssh://" accepted).
struct SftpUrl {
    std::wstring host;
    uint16_t port = 22;
    std::wstring user;
    std::wstring password;
    std::wstring path;
};
std::optional<SftpUrl> ParseSftpUrl(const std::wstring& text);

// SSH_FX_* status code -> a short sentence ("No such file or folder.").
std::wstring SftpStatusText(unsigned long code);

// Unix mode bits -> "rwxr-xr-x".
std::wstring FormatPermissions(unsigned long mode);

// Seconds since 1970 (UTC) -> FILETIME ticks; 0 stays 0 (unknown).
uint64_t UnixTimeToFileTime(uint64_t seconds);

// Whether a keyboard-interactive prompt asks for the account password (it is
// then answered with the password the user gave; anything else is asked).
bool IsPasswordPrompt(const std::wstring& prompt);

std::wstring Utf8ToWide(const std::string& text);
std::string WideToUtf8(const std::wstring& text);

std::string Base64Encode(const unsigned char* data, size_t length, bool padding = true);
std::optional<std::vector<unsigned char>> Base64Decode(const std::string& text);  // whitespace ignored

}  // namespace sftp
