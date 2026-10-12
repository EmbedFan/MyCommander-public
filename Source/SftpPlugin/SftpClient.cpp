#include "SftpClient.h"

#include <libssh2.h>
#include <libssh2_sftp.h>
#include <shlobj.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <vector>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace sftp {

using remote::Status;

namespace {

constexpr int kCancelled = -10001;
constexpr int kTimedOut = -10002;
constexpr size_t kChunk = 64 * 1024;

int g_libraryUsers = 0;  // single-threaded, like the rest of the plugin

void LibraryAcquire() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
    if (g_libraryUsers++ == 0) libssh2_init(0);
}

void LibraryRelease() {
    if (--g_libraryUsers == 0) libssh2_exit();
    WSACleanup();
}

std::wstring KnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) result = path;
    CoTaskMemFree(path);
    return result;
}

std::optional<std::string> ReadFileBytes(const std::wstring& path) {
    if (path.empty()) return std::nullopt;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return std::nullopt;
    std::string data;
    char buffer[16384];
    DWORD read = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &read, nullptr) && read > 0) data.append(buffer, read);
    CloseHandle(h);
    return data;
}

bool WriteFileAtomically(const std::wstring& path, const std::string& data) {
    std::wstring temp = path + L".tmp";
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    bool ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) && written == data.size();
    CloseHandle(h);
    if (ok) ok = MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
    if (!ok) DeleteFileW(temp.c_str());
    return ok;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> lines;
    size_t start = 0;
    while (start <= text.size()) {
        size_t end = text.find('\n', start);
        std::string line = text.substr(start, end == std::string::npos ? std::string::npos : end - start);
        if (!line.empty() && line.back() == '\r') line.pop_back();
        lines.push_back(line);
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return lines;
}

// libssh2 opens key files with fopen (ANSI path): a path the ANSI code page
// can't spell is passed in its 8.3 short form.
std::string CrtPath(const std::wstring& path) {
    auto narrow = [](const std::wstring& p, bool& lossy) {
        BOOL usedDefault = FALSE;
        int n = WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, p.c_str(), -1, nullptr, 0, nullptr, &usedDefault);
        std::string out(static_cast<size_t>(std::max(n, 1)), '\0');
        WideCharToMultiByte(CP_ACP, WC_NO_BEST_FIT_CHARS, p.c_str(), -1, out.data(), n, nullptr, &usedDefault);
        out.resize(strlen(out.c_str()));
        lossy = usedDefault != FALSE;
        return out;
    };
    bool lossy = false;
    std::string result = narrow(path, lossy);
    if (!lossy) return result;
    std::wstring shortPath(32768, L'\0');
    DWORD n = GetShortPathNameW(path.c_str(), shortPath.data(), static_cast<DWORD>(shortPath.size()));
    if (n == 0 || n >= shortPath.size()) return result;
    shortPath.resize(n);
    return narrow(shortPath, lossy);
}

std::wstring SocketErrorText(int code) {
    wchar_t* text = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, reinterpret_cast<wchar_t*>(&text), 0, nullptr);
    std::wstring result = text ? text : L"error " + std::to_wstring(code);
    LocalFree(text);
    while (!result.empty() && (result.back() == L'\n' || result.back() == L'\r' || result.back() == L'.')) result.pop_back();
    return result;
}

int KnownHostKeyBit(int hostKeyType) {
    switch (hostKeyType) {
        case LIBSSH2_HOSTKEY_TYPE_RSA: return LIBSSH2_KNOWNHOST_KEY_SSHRSA;
        case LIBSSH2_HOSTKEY_TYPE_DSS: return LIBSSH2_KNOWNHOST_KEY_SSHDSS;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: return LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: return LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: return LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
        case LIBSSH2_HOSTKEY_TYPE_ED25519: return LIBSSH2_KNOWNHOST_KEY_ED25519;
        default: return LIBSSH2_KNOWNHOST_KEY_UNKNOWN;
    }
}

// Loads known_hosts lines into a libssh2 collection.
LIBSSH2_KNOWNHOSTS* LoadKnownHosts(LIBSSH2_SESSION* session, const std::vector<std::string>& lines) {
    LIBSSH2_KNOWNHOSTS* hosts = libssh2_knownhost_init(session);
    if (!hosts) return nullptr;
    for (const auto& line : lines) {
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '#') continue;
        libssh2_knownhost_readline(hosts, line.c_str(), line.size(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);  // bad lines ignored
    }
    return hosts;
}

LIBSSH2_USERAUTH_KBDINT_RESPONSE_FUNC(KeyboardInteractive) {
    auto* client = static_cast<Client*>(*abstract);
    client->AnswerPrompts(std::string(name ? name : "", static_cast<size_t>(std::max(name_len, 0))),
                          std::string(instruction ? instruction : "", static_cast<size_t>(std::max(instruction_len, 0))),
                          num_prompts, prompts, responses);
}

}  // namespace

KnownHostsFiles DefaultKnownHostsFiles() {
    KnownHostsFiles files;
    std::wstring appData = KnownFolder(FOLDERID_RoamingAppData);
    if (!appData.empty()) files.own = appData + L"\\MyCommander\\known_hosts";
    std::wstring profile = KnownFolder(FOLDERID_Profile);
    if (!profile.empty()) files.user = profile + L"\\.ssh\\known_hosts";
    return files;
}

Client::Client(HostKeyFn hostKey, PromptFn prompt, KnownHostsFiles files)
    : hostKey_(std::move(hostKey)), prompt_(std::move(prompt)), files_(std::move(files)) {
    LibraryAcquire();
}

Client::~Client() {
    Disconnect();
    LibraryRelease();
}

void Client::Trace(const std::wstring& line) const {
    if (trace_) trace_(line);
}

int Client::Wait(const std::function<int()>& op, const remote::CancelFn& cancel) {
    ULONGLONG start = GetTickCount64(), cancelAt = 0;
    for (;;) {
        int rc = op();
        if (rc != LIBSSH2_ERROR_EAGAIN) return rc;
        ULONGLONG now = GetTickCount64();
        if (!cancelAt && cancel && cancel()) cancelAt = now;
        if (cancelAt && now - cancelAt > 2000) {
            DropSession();
            return kCancelled;
        }
        if (now - start > static_cast<ULONGLONG>(timeoutSeconds_) * 1000) {
            DropSession();
            return kTimedOut;
        }
        fd_set readSet, writeSet;
        FD_ZERO(&readSet);
        FD_ZERO(&writeSet);
        int directions = session_ ? libssh2_session_block_directions(session_) : 0;
        if ((directions & LIBSSH2_SESSION_BLOCK_INBOUND) || !(directions & LIBSSH2_SESSION_BLOCK_OUTBOUND))
            FD_SET(socket_, &readSet);
        if (directions & LIBSSH2_SESSION_BLOCK_OUTBOUND) FD_SET(socket_, &writeSet);
        timeval tick{0, 100000};
        select(0, &readSet, &writeSet, nullptr, &tick);
    }
}

Status Client::NotConnected(std::wstring& error) {
    error = L"Not connected to the server.";
    return Status::ConnectionLost;
}

Status Client::Fail(int rc, const std::wstring& what, std::wstring& error) {
    std::wstring prefix = what.empty() ? L"" : what + L": ";
    if (rc == kCancelled) {
        error = L"Cancelled.";
        return Status::Cancelled;
    }
    if (rc == kTimedOut) {
        error = prefix + L"the server did not answer within " + std::to_wstring(timeoutSeconds_) + L" seconds.";
        return Status::ConnectionLost;
    }
    if (rc == LIBSSH2_ERROR_SFTP_PROTOCOL && sftp_) {
        unsigned long code = libssh2_sftp_last_error(sftp_);
        error = prefix + SftpStatusText(code);
        if (code == LIBSSH2_FX_NO_CONNECTION || code == LIBSSH2_FX_CONNECTION_LOST) {
            DropSession();
            return Status::ConnectionLost;
        }
        return Status::Failed;
    }
    char* message = nullptr;
    if (session_) libssh2_session_last_error(session_, &message, nullptr, 0);
    error = prefix + (message && *message ? Utf8ToWide(message) : L"SSH error " + std::to_wstring(rc));
    if (rc == LIBSSH2_ERROR_BUFFER_TOO_SMALL || rc == LIBSSH2_ERROR_INVAL) return Status::Failed;
    DropSession();
    return Status::ConnectionLost;
}

Status Client::OpenSocket(const remote::Connection& c, const remote::CancelFn& cancel, std::wstring& error) {
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* addresses = nullptr;
    std::wstring port = std::to_wstring(c.port);
    if (GetAddrInfoW(c.host.c_str(), port.c_str(), &hints, &addresses) != 0 || !addresses) {
        error = L"Cannot find the server \"" + c.host + L"\".";
        return Status::Failed;
    }
    int lastError = 0;
    for (ADDRINFOW* a = addresses; a && socket_ == INVALID_SOCKET; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        u_long nonBlocking = 1;
        ioctlsocket(s, FIONBIO, &nonBlocking);
        bool connected = connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == 0;
        if (!connected && WSAGetLastError() == WSAEWOULDBLOCK) {
            ULONGLONG start = GetTickCount64();
            for (;;) {
                if (cancel && cancel()) {
                    closesocket(s);
                    FreeAddrInfoW(addresses);
                    error = L"Cancelled.";
                    return Status::Cancelled;
                }
                fd_set writeSet, errorSet;
                FD_ZERO(&writeSet);
                FD_ZERO(&errorSet);
                FD_SET(s, &writeSet);
                FD_SET(s, &errorSet);
                timeval tick{0, 100000};
                if (select(0, nullptr, &writeSet, &errorSet, &tick) > 0) {
                    int soError = 0, length = sizeof(soError);
                    getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &length);
                    connected = FD_ISSET(s, &writeSet) && soError == 0;
                    lastError = soError ? soError : WSAECONNREFUSED;
                    break;
                }
                if (GetTickCount64() - start > static_cast<ULONGLONG>(timeoutSeconds_) * 1000) {
                    lastError = WSAETIMEDOUT;
                    break;
                }
            }
        } else if (!connected) {
            lastError = WSAGetLastError();
        }
        if (connected) socket_ = s;
        else closesocket(s);
    }
    FreeAddrInfoW(addresses);
    if (socket_ == INVALID_SOCKET) {
        error = L"Cannot connect to " + c.host + L":" + port + L" (" + SocketErrorText(lastError) + L").";
        return Status::Failed;
    }
    return Status::Ok;
}

Status Client::Connect(const remote::Connection& connection, const std::wstring& secret, const remote::CancelFn& cancel,
                       std::wstring& error) {
    Disconnect();
    timeoutSeconds_ = connection.timeoutSeconds > 0 ? connection.timeoutSeconds : 20;
    Trace(L"connecting to " + connection.host + L":" + std::to_wstring(connection.port));
    Status s = OpenSocket(connection, cancel, error);
    if (s != Status::Ok) return s;
    session_ = libssh2_session_init_ex(nullptr, nullptr, nullptr, this);
    if (!session_) {
        DropSession();
        error = L"Cannot start an SSH session.";
        return Status::Failed;
    }
    libssh2_session_set_blocking(session_, 0);
    int rc = Wait([&] { return libssh2_session_handshake(session_, socket_); }, cancel);
    if (rc != 0) return Fail(rc, L"SSH handshake", error);
    const char* kex = libssh2_session_methods(session_, LIBSSH2_METHOD_KEX);
    const char* cipher = libssh2_session_methods(session_, LIBSSH2_METHOD_CRYPT_CS);
    const char* mac = libssh2_session_methods(session_, LIBSSH2_METHOD_MAC_CS);
    Trace(L"negotiated " + Utf8ToWide(kex ? kex : "?") + L", " + Utf8ToWide(cipher ? cipher : "?") + L", " +
          Utf8ToWide(mac ? mac : "?"));

    s = VerifyHostKey(connection, error);
    if (s != Status::Ok) {
        DropSession();
        return s;
    }
    secret_ = secret;
    secretUsed_ = false;
    promptCancelled_ = false;
    s = Authenticate(connection, cancel, error);
    SecureZeroMemory(secret_.data(), secret_.size() * sizeof(wchar_t));
    secret_.clear();
    if (s != Status::Ok) {
        DropSession();
        return s;
    }

    LIBSSH2_SFTP* sftp = nullptr;
    rc = Wait([&] {
        sftp = libssh2_sftp_init(session_);
        return sftp ? 0 : libssh2_session_last_errno(session_);
    }, cancel);
    if (!sftp) return Fail(rc, L"Starting SFTP", error);
    sftp_ = sftp;
    Trace(L"SFTP session open");
    return Status::Ok;
}

Status Client::VerifyHostKey(const remote::Connection& c, std::wstring& error) {
    size_t keyLength = 0;
    int keyType = 0;
    const char* key = libssh2_session_hostkey(session_, &keyLength, &keyType);
    if (!key || keyLength == 0) {
        error = L"The server sent no host key.";
        return Status::Failed;
    }
    const char* hash = libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256);
    const char* method = libssh2_session_methods(session_, LIBSSH2_METHOD_HOSTKEY);
    HostKeyInfo info;
    info.host = c.host;
    info.port = c.port;
    info.keyType = Utf8ToWide(method ? method : "unknown");
    info.fingerprint = FormatSha256Fingerprint(reinterpret_cast<const unsigned char*>(hash), hash ? 32 : 0);
    int typeMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | KnownHostKeyBit(keyType);
    std::string host = WideToUtf8(c.host);

    auto check = [&](const std::wstring& file, int& conflictLine) {
        auto bytes = ReadFileBytes(file);
        if (!bytes) return KnownState::NotFound;
        std::vector<std::string> lines = SplitLines(*bytes);
        LIBSSH2_KNOWNHOSTS* hosts = LoadKnownHosts(session_, lines);
        if (!hosts) return KnownState::NotFound;
        int result = libssh2_knownhost_checkp(hosts, host.c_str(), c.port, key, keyLength, typeMask, nullptr);
        libssh2_knownhost_free(hosts);
        if (result == LIBSSH2_KNOWNHOST_CHECK_MATCH) return KnownState::Match;
        if (result != LIBSSH2_KNOWNHOST_CHECK_MISMATCH) return KnownState::NotFound;
        for (size_t i = 0; i < lines.size() && conflictLine == 0; ++i) {  // which line holds the old key
            LIBSSH2_KNOWNHOSTS* one = LoadKnownHosts(session_, {lines[i]});
            if (!one) break;
            if (libssh2_knownhost_checkp(one, host.c_str(), c.port, key, keyLength, typeMask, nullptr) ==
                LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
                conflictLine = static_cast<int>(i) + 1;
            libssh2_knownhost_free(one);
        }
        return KnownState::Mismatch;
    };
    int ownLine = 0, userLine = 0;
    KnownState own = check(files_.own, ownLine);
    KnownState user = check(files_.user, userLine);
    info.verdict = CombineKnownHosts(own, user);
    if (info.verdict == HostKeyVerdict::Changed) {
        info.conflictFile = own == KnownState::Mismatch ? files_.own : files_.user;
        info.conflictLine = own == KnownState::Mismatch ? ownLine : userLine;
    }
    static const wchar_t* kVerdicts[] = {L"trusted", L"unknown", L"CHANGED"};
    Trace(L"host key " + info.keyType + L" " + info.fingerprint + L" (" + kVerdicts[static_cast<int>(info.verdict)] + L")");
    if (info.verdict == HostKeyVerdict::Trusted) return Status::Ok;

    HostKeyDecision decision = hostKey_ ? hostKey_(info) : HostKeyDecision::Reject;
    if (info.verdict == HostKeyVerdict::Changed || decision == HostKeyDecision::Reject) {
        error = L"The host key was not accepted.";
        return Status::Cancelled;  // the prompt already said why; no second message
    }
    if (decision == HostKeyDecision::AcceptAndRemember && !files_.own.empty()) {
        bool saved = false;
        if (LIBSSH2_KNOWNHOSTS* hosts = libssh2_knownhost_init(session_)) {
            libssh2_knownhost* entry = nullptr;
            std::string name = KnownHostsName(host, c.port);
            static const char kComment[] = "added by MyCommander";
            if (libssh2_knownhost_addc(hosts, name.c_str(), nullptr, key, keyLength, kComment, sizeof(kComment) - 1,
                                       typeMask, &entry) == 0 && entry) {
                char line[16384];
                size_t length = 0;
                if (libssh2_knownhost_writeline(hosts, entry, line, sizeof(line), &length,
                                                LIBSSH2_KNOWNHOST_FILE_OPENSSH) == 0) {
                    std::string text = ReadFileBytes(files_.own).value_or("");
                    if (!text.empty() && text.back() != '\n') text += '\n';
                    std::string newLine(line, length);
                    while (!newLine.empty() && (newLine.back() == '\n' || newLine.back() == '\r')) newLine.pop_back();
                    CreateDirectoryW(files_.own.substr(0, files_.own.find_last_of(L'\\')).c_str(), nullptr);
                    saved = WriteFileAtomically(files_.own, text + newLine + "\n");
                }
            }
            libssh2_knownhost_free(hosts);
        }
        Trace(saved ? L"host key saved in " + files_.own : L"the host key could not be saved in " + files_.own);
    }
    return Status::Ok;
}

void Client::AnswerPrompts(const std::string& name, const std::string& instruction, int count, const void* promptData,
                           void* responseData) {
    auto* prompts = static_cast<const LIBSSH2_USERAUTH_KBDINT_PROMPT*>(promptData);
    auto* responses = static_cast<LIBSSH2_USERAUTH_KBDINT_RESPONSE*>(responseData);
    std::wstring title = Utf8ToWide(name.empty() ? instruction : name);
    if (title.empty()) title = L"SSH login";
    for (int i = 0; i < count; ++i) {
        std::wstring prompt = Utf8ToWide(std::string(reinterpret_cast<const char*>(prompts[i].text), prompts[i].length));
        bool echo = prompts[i].echo != 0;
        std::wstring answer;
        if (!echo && !secretUsed_ && IsPasswordPrompt(prompt)) {
            answer = secret_;
            secretUsed_ = true;
        } else if (!promptCancelled_ && prompt_) {
            auto given = prompt_(title, prompt, echo);
            if (given) answer = *given;
            else promptCancelled_ = true;
        }
        std::string utf8 = WideToUtf8(answer);
        SecureZeroMemory(answer.data(), answer.size() * sizeof(wchar_t));
        responses[i].text = static_cast<char*>(std::malloc(utf8.size() + 1));  // freed by libssh2
        if (responses[i].text) {
            std::memcpy(responses[i].text, utf8.c_str(), utf8.size() + 1);
            responses[i].length = static_cast<unsigned int>(utf8.size());
        } else {
            responses[i].length = 0;
        }
        SecureZeroMemory(utf8.data(), utf8.size());
    }
}

Status Client::Authenticate(const remote::Connection& c, const remote::CancelFn& cancel, std::wstring& error) {
    std::string user = WideToUtf8(c.user);
    char* list = nullptr;
    int rc = Wait([&] {
        list = libssh2_userauth_list(session_, user.c_str(), static_cast<unsigned int>(user.size()));
        if (list || libssh2_userauth_authenticated(session_)) return 0;
        return libssh2_session_last_errno(session_);
    }, cancel);
    if (rc != 0) return Fail(rc, L"Login", error);
    if (!list && libssh2_userauth_authenticated(session_)) {
        Trace(L"logged in without credentials");
        return Status::Ok;
    }
    std::string methods = list ? list : "";
    Trace(L"the server accepts: " + Utf8ToWide(methods));
    auto offers = [&](const char* method) { return ("," + methods + ",").find(std::string(",") + method + ",") != std::string::npos; };
    std::wstring accepts = L" (it accepts: " + Utf8ToWide(methods) + L")";

    if (c.auth == remote::AuthMethod::Agent) return AuthenticateWithAgent(user, cancel, error);
    if (c.auth == remote::AuthMethod::KeyFile) {
        if (!offers("publickey")) {
            error = L"The server doesn't accept key logins" + accepts + L".";
            return Status::Failed;
        }
        return AuthenticateWithKey(c, user, cancel, error);
    }
    if (offers("password")) {
        std::string password = WideToUtf8(secret_);
        rc = Wait([&] {
            return libssh2_userauth_password_ex(session_, user.c_str(), static_cast<unsigned int>(user.size()),
                                                password.c_str(), static_cast<unsigned int>(password.size()), nullptr);
        }, cancel);
        SecureZeroMemory(password.data(), password.size());
        if (rc == 0) {
            Trace(L"logged in (password)");
            return Status::Ok;
        }
        if (rc != LIBSSH2_ERROR_AUTHENTICATION_FAILED) return Fail(rc, L"Login", error);
        if (!offers("keyboard-interactive")) {
            error = L"Wrong user name or password.";
            return Status::Failed;
        }
    }
    if (offers("keyboard-interactive")) {
        rc = Wait([&] {
            return libssh2_userauth_keyboard_interactive_ex(session_, user.c_str(), static_cast<unsigned int>(user.size()),
                                                            &KeyboardInteractive);
        }, cancel);
        if (rc == 0) {
            Trace(L"logged in (keyboard-interactive)");
            return Status::Ok;
        }
        if (promptCancelled_) {
            error = L"Login cancelled.";
            return Status::Cancelled;
        }
        if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED) {
            error = L"Wrong user name or password.";
            return Status::Failed;
        }
        return Fail(rc, L"Login", error);
    }
    error = L"The server doesn't accept password logins" + accepts + L". Use a key file or the SSH agent.";
    return Status::Failed;
}

Status Client::AuthenticateWithKey(const remote::Connection& c, const std::string& user, const remote::CancelFn& cancel,
                                   std::wstring& error) {
    if (c.keyFile.empty()) {
        error = L"No private key file is set for this connection.";
        return Status::Failed;
    }
    auto bytes = ReadFileBytes(c.keyFile);
    if (!bytes) {
        error = L"Cannot read the key file \"" + c.keyFile + L"\".";
        return Status::Failed;
    }
    KeyFileInfo info = InspectKeyFile(*bytes);
    SecureZeroMemory(bytes->data(), bytes->size());
    std::wstring problem = KeyFileProblem(info, c.keyFile);
    if (!problem.empty()) {
        error = problem;
        return Status::Failed;
    }
    // The public key: "<key>.pub" next to it, else (OpenSSH format) a
    // temporary .pub written from the public half stored in the key file.
    std::wstring publicPath = c.keyFile + L".pub", temporaryPublic;
    if (GetFileAttributesW(publicPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
        publicPath.clear();
        wchar_t dir[MAX_PATH], temp[MAX_PATH];
        if (!info.publicKeyBase64.empty() && GetTempPathW(MAX_PATH, dir) && GetTempFileNameW(dir, L"mcs", 0, temp) &&
            WriteFileAtomically(temp, info.keyType + " " + info.publicKeyBase64 + "\n"))
            publicPath = temporaryPublic = temp;
    }
    std::string privateFile = CrtPath(c.keyFile), publicFile = publicPath.empty() ? "" : CrtPath(publicPath);
    std::string passphrase = WideToUtf8(secret_);
    int rc = Wait([&] {
        return libssh2_userauth_publickey_fromfile_ex(session_, user.c_str(), static_cast<unsigned int>(user.size()),
                                                      publicFile.empty() ? nullptr : publicFile.c_str(),
                                                      privateFile.c_str(), passphrase.c_str());
    }, cancel);
    SecureZeroMemory(passphrase.data(), passphrase.size());
    if (!temporaryPublic.empty()) DeleteFileW(temporaryPublic.c_str());
    if (rc == 0) {
        Trace(L"logged in (key file " + c.keyFile + L")");
        return Status::Ok;
    }
    if (rc == LIBSSH2_ERROR_FILE || rc == LIBSSH2_ERROR_PUBLICKEY_UNVERIFIED) {
        error = info.encrypted ? L"The key file could not be unlocked (wrong passphrase?)."
                               : L"The key file could not be read.";
        return Status::Failed;
    }
    if (rc == LIBSSH2_ERROR_AUTHENTICATION_FAILED || rc == LIBSSH2_ERROR_PUBLICKEY_UNRECOGNIZED) {
        error = L"The server did not accept this key for " + c.user + L".";
        return Status::Failed;
    }
    return Fail(rc, L"Login", error);
}

Status Client::AuthenticateWithAgent(const std::string& user, const remote::CancelFn& cancel, std::wstring& error) {
    LIBSSH2_AGENT* agent = libssh2_agent_init(session_);
    if (!agent) {
        error = L"Cannot use the SSH agent.";
        return Status::Failed;
    }
    struct AgentGuard {
        LIBSSH2_AGENT* agent;
        ~AgentGuard() {
            libssh2_agent_disconnect(agent);
            libssh2_agent_free(agent);
        }
    } guard{agent};
    if (libssh2_agent_connect(agent) != 0) {
        error = L"No SSH agent is running. Start the \"OpenSSH Authentication Agent\" service and add your key with "
                L"ssh-add (or start Pageant).";
        return Status::Failed;
    }
    if (libssh2_agent_list_identities(agent) != 0) {
        error = L"Cannot read the keys of the SSH agent.";
        return Status::Failed;
    }
    libssh2_agent_publickey* identity = nullptr;
    libssh2_agent_publickey* previous = nullptr;
    int tried = 0;
    while (libssh2_agent_get_identity(agent, &identity, previous) == 0) {
        ++tried;
        int rc = Wait([&] { return libssh2_agent_userauth(agent, user.c_str(), identity); }, cancel);
        if (rc == 0) {
            Trace(L"logged in (SSH agent key \"" + Utf8ToWide(identity->comment ? identity->comment : "") + L"\")");
            return Status::Ok;
        }
        if (rc == kCancelled || rc == kTimedOut || !session_) return Fail(rc, L"Login", error);
        previous = identity;
    }
    error = tried == 0 ? L"The SSH agent holds no keys (add one with ssh-add)."
                       : L"The server accepted none of the SSH agent's keys (" + std::to_wstring(tried) + L").";
    return Status::Failed;
}

void Client::DropSession() {
    // Closing the socket first makes every pending libssh2 send/receive fail
    // at once, so tearing the session down can't block.
    if (socket_ != INVALID_SOCKET) {
        closesocket(socket_);
        socket_ = INVALID_SOCKET;
    }
    if (sftp_) libssh2_sftp_shutdown(sftp_);
    sftp_ = nullptr;
    if (session_) libssh2_session_free(session_);
    session_ = nullptr;
}

void Client::Disconnect() {
    if (session_ && socket_ != INVALID_SOCKET) {
        libssh2_session_set_blocking(session_, 1);
        libssh2_session_set_timeout(session_, 3000);
        if (sftp_) libssh2_sftp_shutdown(sftp_);
        sftp_ = nullptr;
        libssh2_session_disconnect(session_, "MyCommander disconnected");
        Trace(L"disconnected");
    }
    DropSession();
}

Status Client::HomeDirectory(std::wstring& path, const remote::CancelFn& cancel, std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    char buffer[4096];
    int rc = Wait([&] { return libssh2_sftp_symlink_ex(sftp_, ".", 1, buffer, sizeof(buffer) - 1, LIBSSH2_SFTP_REALPATH); },
                  cancel);
    if (rc < 0) return Fail(rc, L"Home folder", error);
    path = Utf8ToWide(std::string(buffer, static_cast<size_t>(rc)));
    if (path.empty()) path = L"/";
    return Status::Ok;
}

Status Client::List(const std::wstring& dir, std::vector<remote::RemoteEntry>& entries, const remote::CancelFn& cancel,
                    std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    std::wstring folder = dir.empty() ? L"/" : dir;
    std::string path = WideToUtf8(folder);
    Trace(L"list " + folder);
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    int rc = Wait([&] {
        handle = libssh2_sftp_open_ex(sftp_, path.c_str(), static_cast<unsigned int>(path.size()), 0, 0, LIBSSH2_SFTP_OPENDIR);
        return handle ? 0 : libssh2_session_last_errno(session_);
    }, cancel);
    if (!handle) return Fail(rc, folder, error);
    entries.clear();
    std::vector<char> name(4096), longEntry(4096);
    Status status = Status::Ok;
    for (;;) {
        if (cancel && cancel()) {
            status = Status::Cancelled;
            error = L"Cancelled.";
            break;
        }
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        rc = Wait([&] {
            return libssh2_sftp_readdir_ex(handle, name.data(), name.size(), longEntry.data(), longEntry.size(), &attributes);
        }, cancel);
        if (rc == 0) break;
        if (rc < 0) {
            status = Fail(rc, folder, error);
            break;
        }
        std::string entryName(name.data(), static_cast<size_t>(rc));
        if (entryName == "." || entryName == "..") continue;
        remote::RemoteEntry e;
        e.name = Utf8ToWide(entryName);
        if (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            e.isDirectory = LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
            e.isLink = LIBSSH2_SFTP_S_ISLNK(attributes.permissions);
            e.permissions = FormatPermissions(attributes.permissions);
        }
        if (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) e.size = attributes.filesize;
        if (attributes.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) e.modifiedUtc = UnixTimeToFileTime(attributes.mtime);
        entries.push_back(std::move(e));
    }
    if (sftp_) Wait([&] { return libssh2_sftp_close_handle(handle); }, {});
    if (status != Status::Ok) return status;
    // A symbolic link shows as a folder when it points at one (Enter opens
    // it); the panel never descends into it when copying or deleting.
    for (auto& e : entries) {
        if (!e.isLink || !sftp_) continue;
        std::string target = WideToUtf8(remote::JoinRemotePath(folder, e.name));
        LIBSSH2_SFTP_ATTRIBUTES attributes{};
        rc = Wait([&] {
            return libssh2_sftp_stat_ex(sftp_, target.c_str(), static_cast<unsigned int>(target.size()), LIBSSH2_SFTP_STAT,
                                        &attributes);
        }, cancel);
        if (rc == 0) {
            if (attributes.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) e.isDirectory = LIBSSH2_SFTP_S_ISDIR(attributes.permissions);
            if ((attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) && !e.isDirectory) e.size = attributes.filesize;
        } else if (rc != LIBSSH2_ERROR_SFTP_PROTOCOL) {  // a broken link is fine; a broken session isn't
            return Fail(rc, folder, error);
        }
    }
    return Status::Ok;
}

Status Client::Download(const std::wstring& remotePath, const std::wstring& localPath, uint64_t sizeHint,
                        const remote::ProgressFn& progress, const remote::CancelFn& cancel, std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    Trace(L"download " + remotePath);
    std::string path = WideToUtf8(remotePath);
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    int rc = Wait([&] {
        handle = libssh2_sftp_open_ex(sftp_, path.c_str(), static_cast<unsigned int>(path.size()), LIBSSH2_FXF_READ, 0,
                                      LIBSSH2_SFTP_OPENFILE);
        return handle ? 0 : libssh2_session_last_errno(session_);
    }, cancel);
    if (!handle) return Fail(rc, remotePath, error);
    HANDLE file = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        Wait([&] { return libssh2_sftp_close_handle(handle); }, {});
        error = L"Cannot create " + localPath + L".";
        return Status::Failed;
    }
    std::vector<char> buffer(kChunk);
    uint64_t done = 0;
    Status status = Status::Ok;
    for (;;) {
        if (cancel && cancel()) {  // between chunks: the session stays usable
            status = Status::Cancelled;
            error = L"Cancelled.";
            break;
        }
        ssize_t got = 0;
        rc = Wait([&] {
            got = libssh2_sftp_read(handle, buffer.data(), buffer.size());
            return got >= 0 ? 0 : static_cast<int>(got);
        }, cancel);
        if (rc != 0) {
            status = Fail(rc, remotePath, error);
            break;
        }
        if (got == 0) break;
        DWORD written = 0;
        if (!WriteFile(file, buffer.data(), static_cast<DWORD>(got), &written, nullptr) || written != static_cast<DWORD>(got)) {
            status = Status::Failed;
            error = L"Cannot write " + localPath + L".";
            break;
        }
        done += static_cast<uint64_t>(got);
        if (progress) progress(done, std::max(sizeHint, done));
    }
    CloseHandle(file);
    if (sftp_) Wait([&] { return libssh2_sftp_close_handle(handle); }, {});
    return status;
}

Status Client::Upload(const std::wstring& localPath, const std::wstring& remotePath, const remote::ProgressFn& progress,
                      const remote::CancelFn& cancel, std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    Trace(L"upload " + remotePath);
    HANDLE file = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = L"Cannot read " + localPath + L".";
        return Status::Failed;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    std::string path = WideToUtf8(remotePath);
    LIBSSH2_SFTP_HANDLE* handle = nullptr;
    int rc = Wait([&] {
        handle = libssh2_sftp_open_ex(sftp_, path.c_str(), static_cast<unsigned int>(path.size()),
                                      LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | LIBSSH2_FXF_TRUNC,
                                      LIBSSH2_SFTP_S_IRUSR | LIBSSH2_SFTP_S_IWUSR | LIBSSH2_SFTP_S_IRGRP |
                                          LIBSSH2_SFTP_S_IROTH,
                                      LIBSSH2_SFTP_OPENFILE);
        return handle ? 0 : libssh2_session_last_errno(session_);
    }, cancel);
    if (!handle) {
        CloseHandle(file);
        return Fail(rc, remotePath, error);
    }
    std::vector<char> buffer(kChunk);
    uint64_t done = 0, total = static_cast<uint64_t>(size.QuadPart);
    Status status = Status::Ok;
    for (;;) {
        if (cancel && cancel()) {
            status = Status::Cancelled;
            error = L"Cancelled.";
            break;
        }
        DWORD read = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
            status = Status::Failed;
            error = L"Cannot read " + localPath + L".";
            break;
        }
        if (read == 0) break;
        size_t offset = 0;
        while (offset < read && status == Status::Ok) {
            ssize_t sent = 0;
            rc = Wait([&] {
                sent = libssh2_sftp_write(handle, buffer.data() + offset, read - offset);
                return sent >= 0 ? 0 : static_cast<int>(sent);
            }, cancel);
            if (rc != 0) status = Fail(rc, remotePath, error);
            else offset += static_cast<size_t>(sent);
        }
        if (status != Status::Ok) break;
        done += read;
        if (progress) progress(done, std::max(total, done));
    }
    CloseHandle(file);
    if (sftp_) {
        rc = Wait([&] { return libssh2_sftp_close_handle(handle); }, {});
        if (rc != 0 && status == Status::Ok) status = Fail(rc, remotePath, error);  // the close flushes the last write
    }
    return status;
}

Status Client::MakeRemoteDirectory(const std::wstring& path, const remote::CancelFn& cancel, std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    Trace(L"mkdir " + path);
    std::string p = WideToUtf8(path);
    int rc = Wait([&] {
        return libssh2_sftp_mkdir_ex(sftp_, p.c_str(), static_cast<unsigned int>(p.size()),
                                     LIBSSH2_SFTP_S_IRWXU | LIBSSH2_SFTP_S_IRGRP | LIBSSH2_SFTP_S_IXGRP |
                                         LIBSSH2_SFTP_S_IROTH | LIBSSH2_SFTP_S_IXOTH);
    }, cancel);
    return rc == 0 ? Status::Ok : Fail(rc, path, error);
}

Status Client::RemoveRemoteDirectory(const std::wstring& path, const remote::CancelFn& cancel, std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    Trace(L"rmdir " + path);
    std::string p = WideToUtf8(path);
    int rc = Wait([&] { return libssh2_sftp_rmdir_ex(sftp_, p.c_str(), static_cast<unsigned int>(p.size())); }, cancel);
    return rc == 0 ? Status::Ok : Fail(rc, path, error);
}

Status Client::DeleteRemoteFile(const std::wstring& path, const remote::CancelFn& cancel, std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    Trace(L"delete " + path);
    std::string p = WideToUtf8(path);
    int rc = Wait([&] { return libssh2_sftp_unlink_ex(sftp_, p.c_str(), static_cast<unsigned int>(p.size())); }, cancel);
    return rc == 0 ? Status::Ok : Fail(rc, path, error);
}

Status Client::RenameRemote(const std::wstring& from, const std::wstring& to, const remote::CancelFn& cancel,
                            std::wstring& error) {
    if (!sftp_) return NotConnected(error);
    Trace(L"rename " + from + L" -> " + to);
    std::string a = WideToUtf8(from), b = WideToUtf8(to);
    // Never overwrites: an existing target makes the server refuse.
    int rc = Wait([&] {
        return libssh2_sftp_rename_ex(sftp_, a.c_str(), static_cast<unsigned int>(a.size()), b.c_str(),
                                      static_cast<unsigned int>(b.size()),
                                      LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE);
    }, cancel);
    return rc == 0 ? Status::Ok : Fail(rc, from, error);
}

}  // namespace sftp
