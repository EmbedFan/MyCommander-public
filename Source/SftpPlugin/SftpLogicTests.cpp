// EXT-004: standalone regression harness for the SFTP plugin's pure logic
// (SftpLogic.cpp) and its connections store (PluginCommon/RemoteConnections
// with the SFTP StoreSpec) -- no libssh2, no network.
// Build from a Developer Command Prompt (or run Source\SftpPlugin\BuildTests.cmd):
//   cl /utf-8 /std:c++20 /EHsc /MTd /W4 /FoBuild\ /FeBuild\SftpLogicTests.exe Source\SftpPlugin\SftpLogicTests.cpp
//      Source\SftpPlugin\SftpLogic.cpp Source\PluginCommon\RemoteConnections.cpp
// Run: Build\SftpLogicTests.exe   (exit code 0 = every check passed)

#include "SftpConnections.h"
#include "SftpLogic.h"

#include <cstdio>
#include <set>

namespace {

int failures = 0;
int checks = 0;
void Check(bool condition, const char* label) {
    ++checks;
    if (!condition) {
        std::printf("FAIL: %s\n", label);
        ++failures;
    }
}

std::string SshString(const std::string& s) {
    uint32_t n = static_cast<uint32_t>(s.size());
    std::string out{static_cast<char>(n >> 24), static_cast<char>(n >> 16), static_cast<char>(n >> 8), static_cast<char>(n)};
    return out + s;
}

// A structurally valid "-----BEGIN OPENSSH PRIVATE KEY-----" file (the
// private part is filler: InspectKeyFile only reads the header).
std::string OpenSshKey(const std::string& keyType, const std::string& cipher) {
    std::string publicBlob = SshString(keyType) + SshString(std::string(32, 'k'));
    std::string body = std::string("openssh-key-v1") + '\0' + SshString(cipher) + SshString(cipher == "none" ? "none" : "bcrypt") +
                       SshString(cipher == "none" ? "" : "salt+rounds") + std::string("\0\0\0\1", 4) + SshString(publicBlob) +
                       SshString(std::string(64, 'p'));
    std::string encoded = sftp::Base64Encode(reinterpret_cast<const unsigned char*>(body.data()), body.size());
    std::string text = "-----BEGIN OPENSSH PRIVATE KEY-----\n";
    for (size_t i = 0; i < encoded.size(); i += 70) text += encoded.substr(i, 70) + "\n";
    return text + "-----END OPENSSH PRIVATE KEY-----\n";
}

bool Contains(const std::wstring& text, const wchar_t* part) { return text.find(part) != std::wstring::npos; }

}  // namespace

int wmain() {
    // --- base64 and fingerprints (DEC-015) ---------------------------------------
    unsigned char bytes[32];
    for (int i = 0; i < 32; ++i) bytes[i] = static_cast<unsigned char>(i);
    Check(sftp::FormatSha256Fingerprint(bytes, 32) == L"SHA256:AAECAwQFBgcICQoLDA0ODxAREhMUFRYXGBkaGxwdHh8",
          "fingerprint = SHA256: + unpadded base64");
    const unsigned char rebex[32] = {0x3b, 0x3b, 0xe9, 0x43, 0x14, 0x54, 0xcd, 0x27, 0xd5, 0xf4, 0x5f, 0xc4, 0x08, 0xc5, 0xdb, 0x43,
                                     0xb0, 0x7b, 0xcd, 0xb2, 0xb4, 0x69, 0x39, 0xe0, 0xae, 0x11, 0x42, 0x05, 0x49, 0xe8, 0xeb, 0x97};
    Check(sftp::FormatSha256Fingerprint(rebex, 32) == L"SHA256:OzvpQxRUzSfV9F/ECMXbQ7B7zbK0aTngrhFCBUno65c",
          "fingerprint matches what ssh-keygen -l shows");
    Check(sftp::Base64Encode(reinterpret_cast<const unsigned char*>("a"), 1) == "YQ==" &&
              sftp::Base64Encode(reinterpret_cast<const unsigned char*>("ab"), 2) == "YWI=" &&
              sftp::Base64Encode(reinterpret_cast<const unsigned char*>("abc"), 3) == "YWJj",
          "base64 padding");
    auto decoded = sftp::Base64Decode("YWJj\nYQ==");
    Check(decoded && std::string(decoded->begin(), decoded->end()) == "abca", "base64 decode skips whitespace");
    Check(!sftp::Base64Decode("ab$c"), "base64 decode rejects bad characters");

    // --- the known-hosts verdict (DEC-015, SFTP-007) -----------------------------
    using sftp::KnownState;
    using sftp::HostKeyVerdict;
    Check(sftp::CombineKnownHosts(KnownState::NotFound, KnownState::NotFound) == HostKeyVerdict::Unknown, "unknown host");
    Check(sftp::CombineKnownHosts(KnownState::Match, KnownState::NotFound) == HostKeyVerdict::Trusted, "trusted by MyCommander");
    Check(sftp::CombineKnownHosts(KnownState::NotFound, KnownState::Match) == HostKeyVerdict::Trusted, "trusted by ~/.ssh");
    Check(sftp::CombineKnownHosts(KnownState::Mismatch, KnownState::NotFound) == HostKeyVerdict::Changed, "changed (own)");
    Check(sftp::CombineKnownHosts(KnownState::Match, KnownState::Mismatch) == HostKeyVerdict::Changed,
          "a mismatch anywhere wins over a match: changed keys are never accepted");
    Check(sftp::KnownHostsName("example.com", 22) == "example.com" && sftp::KnownHostsName("h", 2222) == "[h]:2222",
          "known_hosts host field");

    // --- key files (SFTP-005) ------------------------------------------------------
    auto pem = sftp::InspectKeyFile("-----BEGIN RSA PRIVATE KEY-----\nMIIE\n-----END RSA PRIVATE KEY-----\n");
    Check(pem.format == sftp::KeyFormat::PemRsa && !pem.encrypted && sftp::KeyFileProblem(pem, L"k").empty(),
          "PEM RSA: supported");
    auto pemEncrypted = sftp::InspectKeyFile(
        "-----BEGIN RSA PRIVATE KEY-----\nProc-Type: 4,ENCRYPTED\nDEK-Info: AES-128-CBC,00\n\nMIIE\n-----END RSA PRIVATE KEY-----\n");
    Check(pemEncrypted.encrypted && sftp::KeyFileProblem(pemEncrypted, L"k").empty(), "PEM RSA with a passphrase: supported");
    auto ecdsa = sftp::InspectKeyFile(OpenSshKey("ecdsa-sha2-nistp256", "none"));
    Check(ecdsa.format == sftp::KeyFormat::OpenSsh && ecdsa.keyType == "ecdsa-sha2-nistp256" && !ecdsa.encrypted &&
              sftp::KeyFileProblem(ecdsa, L"k").empty(),
          "OpenSSH ECDSA without a passphrase: supported");
    Check(ecdsa.publicKeyBase64.size() > 40, "the public half is extracted for a .pub line");
    auto ecdsaEncrypted = sftp::InspectKeyFile(OpenSshKey("ecdsa-sha2-nistp256", "aes256-ctr"));
    Check(ecdsaEncrypted.encrypted && Contains(sftp::KeyFileProblem(ecdsaEncrypted, L"k"), L"SSH agent"),
          "OpenSSH ECDSA with a passphrase: points to the agent");
    auto ed = sftp::InspectKeyFile(OpenSshKey("ssh-ed25519", "none"));
    Check(ed.keyType == "ssh-ed25519" && Contains(sftp::KeyFileProblem(ed, L"k"), L"Ed25519") &&
              Contains(sftp::KeyFileProblem(ed, L"k"), L"SSH agent"),
          "Ed25519 key file: explained, points to the agent");
    auto rsaNew = sftp::InspectKeyFile(OpenSshKey("ssh-rsa", "none"));
    Check(Contains(sftp::KeyFileProblem(rsaNew, L"C:\\k\\id_rsa"), L"ssh-keygen -p -m PEM -f \"C:\\k\\id_rsa\""),
          "OpenSSH RSA key file: offers the one-time conversion");
    Check(sftp::InspectKeyFile("-----BEGIN PRIVATE KEY-----\nMII\n-----END PRIVATE KEY-----\n").format == sftp::KeyFormat::OtherPem,
          "PKCS#8: other format");
    Check(sftp::InspectKeyFile("hello").format == sftp::KeyFormat::Unreadable &&
              Contains(sftp::KeyFileProblem(sftp::InspectKeyFile("hello"), L"x"), L"not a readable"),
          "not a key file");
    std::set<std::wstring> present{L"C:\\u\\.ssh\\id_rsa", L"C:\\u\\.ssh\\id_ed25519"};
    auto exists = [&](const std::wstring& p) { return present.count(p) > 0; };
    Check(sftp::DefaultKeyFile(L"C:\\u\\.ssh", exists) == L"C:\\u\\.ssh\\id_rsa", "default key file: ecdsa, rsa, then ed25519");
    Check(sftp::DefaultKeyFile(L"C:\\none", exists).empty(), "no default key file");

    // --- sftp:// addresses -------------------------------------------------------------
    auto url = sftp::ParseSftpUrl(L"sftp://bob:pw@example.com:2222/srv/data");
    Check(url && url->host == L"example.com" && url->port == 2222 && url->user == L"bob" && url->password == L"pw" &&
              url->path == L"/srv/data",
          "full sftp:// address");
    url = sftp::ParseSftpUrl(L"alice@host");
    Check(url && url->host == L"host" && url->port == 22 && url->user == L"alice" && url->path.empty(), "user@host");
    url = sftp::ParseSftpUrl(L"ssh://[::1]:22/");
    Check(url && url->host == L"::1" && url->path == L"/", "IPv6 host");
    Check(!sftp::ParseSftpUrl(L"ftp://host") && !sftp::ParseSftpUrl(L"sftp://host:99999") && !sftp::ParseSftpUrl(L"sftp://"),
          "other schemes, bad ports and empty hosts are rejected");

    // --- statuses, modes, times, prompts -------------------------------------------
    Check(sftp::SftpStatusText(2) == L"No such file or folder." && sftp::SftpStatusText(3) == L"Permission denied." &&
              Contains(sftp::SftpStatusText(99), L"99"),
          "SFTP status texts");
    Check(sftp::FormatPermissions(0755) == L"rwxr-xr-x" && sftp::FormatPermissions(0100640) == L"rw-r-----", "Unix modes");
    Check(sftp::UnixTimeToFileTime(0) == 0 && sftp::UnixTimeToFileTime(1) == 116444736010000000ULL, "Unix time to FILETIME");
    Check(sftp::IsPasswordPrompt(L"Password: ") && sftp::IsPasswordPrompt(L"bob@host's password:") &&
              !sftp::IsPasswordPrompt(L"Verification code: "),
          "keyboard-interactive: which prompt is the password");
    Check(sftp::Utf8ToWide("\xC3\xA1rv\xC3\xADz") == L"árvíz" && sftp::WideToUtf8(L"árvíz") == "\xC3\xA1rv\xC3\xADz",
          "UTF-8 names");
    Check(!sftp::Utf8ToWide("\xE1rv\xEDz").empty(), "a non-UTF-8 name still shows (ANSI fallback)");

    // --- sftp-connections.conf (SFTP-009, SFTP-006) ---------------------------------
    std::vector<std::wstring> warnings;
    auto list = remote::ParseConnections(sftp::Store(),
                                         L"[Work]\nhost = ssh.example.com\nuser = bob\nauth = key\n"
                                         L"key-file = C:\\Users\\bob\\.ssh\\id_rsa\nfolder = /srv\n"
                                         L"passphrase = hunter2\npassive = no\nport = 0\n"
                                         L"[Agent]\nhost = h2\nauth = agent\nsave-password = yes\n",
                                         warnings);
    Check(list.size() == 2 && list[0].auth == remote::AuthMethod::KeyFile && list[0].keyFile == L"C:\\Users\\bob\\.ssh\\id_rsa" &&
              list[0].port == 22 && list[1].auth == remote::AuthMethod::Agent && list[1].savePassword,
          "connections: auth, key file, default port 22");
    bool passphraseWarned = false, passiveWarned = false, portWarned = false;
    for (const auto& w : warnings) {
        passphraseWarned |= Contains(w, L"never read");
        passiveWarned |= Contains(w, L"unknown key \"passive\"");
        portWarned |= Contains(w, L"using 22");
    }
    Check(passphraseWarned && passiveWarned && portWarned, "passphrase line ignored; FTP-only key unknown; port default 22");
    std::wstring text = remote::FormatConnections(sftp::Store(), list);
    Check(Contains(text, L"auth = key") && Contains(text, L"key-file = C:\\Users\\bob\\.ssh\\id_rsa") &&
              Contains(text, L"auth = agent") && !Contains(text, L"passive") && !Contains(text, L"hunter2"),
          "format: auth/key-file written, no passive, never a secret");
    warnings.clear();
    auto again = remote::ParseConnections(sftp::Store(), text, warnings);
    Check(again.size() == 2 && warnings.empty() && again[0].keyFile == list[0].keyFile && again[1].auth == list[1].auth,
          "round trip");
    Check(remote::NewConnection(sftp::Store()).port == 22 && remote::NewConnection(sftp::Store()).user.empty(),
          "a new SFTP connection: port 22, no default user");

    std::printf("SFTP logic tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
