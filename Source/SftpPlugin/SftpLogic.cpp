#include "SftpLogic.h"

#include <algorithm>
#include <cstring>
#include <cwctype>

namespace sftp {

namespace {

const char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

// Reads one SSH "string" (uint32 big-endian length + bytes) at `pos`.
bool ReadSshString(const std::vector<unsigned char>& data, size_t& pos, std::string& out) {
    if (pos + 4 > data.size()) return false;
    uint32_t length = (uint32_t{data[pos]} << 24) | (uint32_t{data[pos + 1]} << 16) | (uint32_t{data[pos + 2]} << 8) |
                      uint32_t{data[pos + 3]};
    pos += 4;
    if (length > data.size() - pos) return false;
    out.assign(reinterpret_cast<const char*>(data.data() + pos), length);
    pos += length;
    return true;
}

// The base64 body between a PEM "-----BEGIN x-----" line and its END line.
std::string PemBody(const std::string& text, const std::string& begin, const std::string& end) {
    size_t b = text.find(begin);
    if (b == std::string::npos) return {};
    b += begin.size();
    size_t e = text.find(end, b);
    return e == std::string::npos ? std::string{} : text.substr(b, e - b);
}

}  // namespace

std::string Base64Encode(const unsigned char* data, size_t length, bool padding) {
    std::string out;
    for (size_t i = 0; i < length; i += 3) {
        uint32_t chunk = uint32_t{data[i]} << 16;
        if (i + 1 < length) chunk |= uint32_t{data[i + 1]} << 8;
        if (i + 2 < length) chunk |= data[i + 2];
        out += kBase64[(chunk >> 18) & 63];
        out += kBase64[(chunk >> 12) & 63];
        if (i + 1 < length) out += kBase64[(chunk >> 6) & 63];
        else if (padding) out += '=';
        if (i + 2 < length) out += kBase64[chunk & 63];
        else if (padding) out += '=';
    }
    return out;
}

std::optional<std::vector<unsigned char>> Base64Decode(const std::string& text) {
    std::vector<unsigned char> out;
    uint32_t buffer = 0;
    int bits = 0;
    for (char c : text) {
        if (c == '=' || c == ' ' || c == '\r' || c == '\n' || c == '\t') continue;
        const char* p = c ? std::strchr(kBase64, c) : nullptr;
        if (!p) return std::nullopt;
        buffer = (buffer << 6) | static_cast<uint32_t>(p - kBase64);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<unsigned char>((buffer >> bits) & 0xFF));
        }
    }
    return out;
}

std::wstring FormatSha256Fingerprint(const unsigned char* hash, size_t length) {
    if (!hash || length == 0) return L"SHA256:?";
    return L"SHA256:" + Utf8ToWide(Base64Encode(hash, length, false));
}

HostKeyVerdict CombineKnownHosts(KnownState own, KnownState user) {
    if (own == KnownState::Mismatch || user == KnownState::Mismatch) return HostKeyVerdict::Changed;
    if (own == KnownState::Match || user == KnownState::Match) return HostKeyVerdict::Trusted;
    return HostKeyVerdict::Unknown;
}

std::string KnownHostsName(const std::string& host, uint16_t port) {
    if (port == 22) return host;
    return "[" + host + "]:" + std::to_string(port);
}

KeyFileInfo InspectKeyFile(const std::string& text) {
    KeyFileInfo info;
    if (text.find("-----BEGIN RSA PRIVATE KEY-----") != std::string::npos) {
        info.format = KeyFormat::PemRsa;
        info.keyType = "ssh-rsa";
        info.encrypted = text.find("Proc-Type: 4,ENCRYPTED") != std::string::npos;
        return info;
    }
    if (text.find("-----BEGIN OPENSSH PRIVATE KEY-----") != std::string::npos) {
        info.format = KeyFormat::OpenSsh;
        auto data = Base64Decode(PemBody(text, "-----BEGIN OPENSSH PRIVATE KEY-----", "-----END OPENSSH PRIVATE KEY-----"));
        static const char kMagic[] = "openssh-key-v1";  // followed by a NUL
        if (!data || data->size() < sizeof(kMagic) ||
            std::string(reinterpret_cast<const char*>(data->data()), sizeof(kMagic) - 1) != kMagic) {
            info.format = KeyFormat::Unreadable;
            return info;
        }
        size_t pos = sizeof(kMagic);
        std::string cipher, kdf, kdfOptions, publicKey;
        if (!ReadSshString(*data, pos, cipher) || !ReadSshString(*data, pos, kdf) ||
            !ReadSshString(*data, pos, kdfOptions) || pos + 4 > data->size()) {
            info.format = KeyFormat::Unreadable;
            return info;
        }
        info.encrypted = cipher != "none";
        pos += 4;  // number of keys (always 1)
        if (ReadSshString(*data, pos, publicKey)) {
            std::vector<unsigned char> blob(publicKey.begin(), publicKey.end());
            size_t blobPos = 0;
            ReadSshString(blob, blobPos, info.keyType);
            info.publicKeyBase64 = Base64Encode(blob.data(), blob.size());
        }
        return info;
    }
    if (text.find("-----BEGIN ") != std::string::npos || text.find("PuTTY-User-Key-File") != std::string::npos)
        info.format = KeyFormat::OtherPem;
    return info;
}

std::wstring KeyFileProblem(const KeyFileInfo& info, const std::wstring& path) {
    const std::wstring agentHint = L"Load the key into the SSH agent (ssh-add) and choose \"SSH agent\".";
    switch (info.format) {
        case KeyFormat::Unreadable:
            return L"\"" + path + L"\" is not a readable private key file.";
        case KeyFormat::PemRsa:
            return L"";
        case KeyFormat::OpenSsh:
            if (info.keyType.rfind("ecdsa-sha2-", 0) == 0 && !info.encrypted) return L"";
            if (info.keyType == "ssh-rsa")
                return L"OpenSSH-format RSA key files aren't supported by the built-in SSH library. " + agentHint +
                       L" Or convert the key once: ssh-keygen -p -m PEM -f \"" + path + L"\"";
            if (info.keyType.rfind("ecdsa-sha2-", 0) == 0)
                return L"Passphrase-protected ECDSA key files aren't supported by the built-in SSH library. " + agentHint;
            if (info.keyType == "ssh-ed25519")
                return L"Ed25519 key files aren't supported by the built-in SSH library. " + agentHint;
            return L"This key type (" + Utf8ToWide(info.keyType) + L") isn't supported. " + agentHint;
        default:
            return L"This private key format isn't supported (use an OpenSSH or PEM key file). " + agentHint;
    }
}

std::wstring DefaultKeyFile(const std::wstring& sshDir, const std::function<bool(const std::wstring&)>& exists) {
    for (const wchar_t* name : {L"id_ecdsa", L"id_rsa", L"id_ed25519"}) {
        std::wstring path = sshDir + L"\\" + name;
        if (exists(path)) return path;
    }
    return L"";
}

std::optional<SftpUrl> ParseSftpUrl(const std::wstring& text) {
    std::wstring rest = text;
    while (!rest.empty() && std::iswspace(rest.front())) rest.erase(rest.begin());
    while (!rest.empty() && std::iswspace(rest.back())) rest.pop_back();
    std::wstring lower = Lower(rest);
    if (lower.rfind(L"sftp://", 0) == 0) rest = rest.substr(7);
    else if (lower.rfind(L"ssh://", 0) == 0) rest = rest.substr(6);
    else if (lower.find(L"://") != std::wstring::npos) return std::nullopt;  // some other scheme
    SftpUrl url;
    size_t slash = rest.find(L'/');
    if (slash != std::wstring::npos) {
        url.path = rest.substr(slash);
        rest.resize(slash);
    }
    size_t at = rest.rfind(L'@');
    if (at != std::wstring::npos) {
        std::wstring credentials = rest.substr(0, at);
        rest = rest.substr(at + 1);
        size_t colon = credentials.find(L':');
        url.user = credentials.substr(0, colon);
        if (colon != std::wstring::npos) url.password = credentials.substr(colon + 1);
    }
    std::wstring portText;
    if (!rest.empty() && rest.front() == L'[') {  // [IPv6]:port
        size_t close = rest.find(L']');
        if (close == std::wstring::npos) return std::nullopt;
        url.host = rest.substr(1, close - 1);
        if (close + 1 < rest.size() && rest[close + 1] == L':') portText = rest.substr(close + 2);
    } else {
        size_t colon = rest.rfind(L':');
        if (colon != std::wstring::npos && rest.find(L':') == colon) {
            url.host = rest.substr(0, colon);
            portText = rest.substr(colon + 1);
        } else {
            url.host = rest;
        }
    }
    if (!portText.empty()) {
        unsigned long port = 0;
        try { port = std::stoul(portText); } catch (...) { return std::nullopt; }
        if (port < 1 || port > 65535) return std::nullopt;
        url.port = static_cast<uint16_t>(port);
    }
    if (url.host.empty()) return std::nullopt;
    return url;
}

std::wstring SftpStatusText(unsigned long code) {
    switch (code) {
        case 0: return L"OK.";
        case 1: return L"End of file.";
        case 2: return L"No such file or folder.";
        case 3: return L"Permission denied.";
        case 4: return L"The server reported a failure (the folder may not be empty, or the name may already exist).";
        case 5: return L"Bad message.";
        case 6: return L"No connection.";
        case 7: return L"The connection was lost.";
        case 8: return L"The server doesn't support this operation.";
        case 9: return L"Invalid handle.";
        case 10: return L"No such path.";
        case 11: return L"A file or folder with that name already exists.";
        case 12: return L"The file is write-protected.";
        case 13: return L"No media.";
        case 14: return L"No space left on the server.";
        case 15: return L"Quota exceeded.";
        case 16: return L"Unknown principal.";
        case 17: return L"Lock conflict.";
        case 18: return L"The folder is not empty.";
        case 19: return L"Not a folder.";
        case 20: return L"Invalid file name.";
        case 21: return L"Too many symbolic links.";
        default: return L"SFTP error " + std::to_wstring(code) + L".";
    }
}

std::wstring FormatPermissions(unsigned long mode) {
    std::wstring out;
    const wchar_t* letters = L"rwxrwxrwx";
    for (int i = 0; i < 9; ++i) out += (mode & (1u << (8 - i))) ? letters[i] : L'-';
    return out;
}

uint64_t UnixTimeToFileTime(uint64_t seconds) {
    if (seconds == 0) return 0;
    return (seconds + 11644473600ULL) * 10000000ULL;
}

bool IsPasswordPrompt(const std::wstring& prompt) {
    std::wstring l = Lower(prompt);
    return l.find(L"password") != std::wstring::npos || l.find(L"passwort") != std::wstring::npos ||
           l.find(L"jelsz") != std::wstring::npos;
}

std::wstring Utf8ToWide(const std::string& text) {
    if (text.empty()) return L"";
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), static_cast<int>(text.size()), nullptr, 0);
    UINT codePage = CP_UTF8;
    DWORD flags = MB_ERR_INVALID_CHARS;
    if (n <= 0) {  // not UTF-8: the ANSI code page, so nothing is ever hidden
        codePage = CP_ACP;
        flags = 0;
        n = MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), nullptr, 0);
    }
    std::wstring out(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(codePage, flags, text.data(), static_cast<int>(text.size()), out.data(), n);
    return out;
}

std::string WideToUtf8(const std::wstring& text) {
    if (text.empty()) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), n, nullptr, nullptr);
    return out;
}

}  // namespace sftp
