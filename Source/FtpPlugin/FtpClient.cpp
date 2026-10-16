#include "FtpClient.h"

#include <algorithm>
#include <vector>

#pragma comment(lib, "ws2_32.lib")

namespace ftp {

namespace {

constexpr int kTickMs = 100;
constexpr size_t kChunk = 64 * 1024;

// One WSAStartup for the process, released with the last Client.
int g_clients = 0;

std::wstring Widen(const std::string& s) { return DecodeName(s, true); }

std::wstring WsaError(const wchar_t* what, int code) {
    wchar_t* buffer = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr,
                   static_cast<DWORD>(code), 0, reinterpret_cast<LPWSTR>(&buffer), 0, nullptr);
    std::wstring text = buffer ? buffer : L"";
    if (buffer) LocalFree(buffer);
    while (!text.empty() && (text.back() == L'\n' || text.back() == L'\r' || text.back() == L' ')) text.pop_back();
    return std::wstring(what) + L": " + (text.empty() ? L"error " + std::to_wstring(code) : text);
}

void SetNonBlocking(SOCKET s) {
    u_long on = 1;
    ioctlsocket(s, FIONBIO, &on);
}

// Waits until `s` is readable/writable. Polls `cancel` every tick; gives up
// after `timeoutSeconds` of waiting (FTP-009).
Status WaitSocket(SOCKET s, bool forWrite, int timeoutSeconds, const CancelFn& cancel, std::wstring& error) {
    ULONGLONG deadline = GetTickCount64() + static_cast<ULONGLONG>(timeoutSeconds) * 1000;
    while (true) {
        if (cancel && cancel()) return Status::Cancelled;
        fd_set set;
        FD_ZERO(&set);
        FD_SET(s, &set);
        fd_set errors;
        FD_ZERO(&errors);
        FD_SET(s, &errors);
        timeval tick{0, kTickMs * 1000};
        int ready = select(0, forWrite ? nullptr : &set, forWrite ? &set : nullptr, &errors, &tick);
        if (ready == SOCKET_ERROR) {
            error = WsaError(L"Network error", WSAGetLastError());
            return Status::Failed;
        }
        if (ready > 0) return Status::Ok;
        if (GetTickCount64() >= deadline) {
            error = L"The server did not respond within " + std::to_wstring(timeoutSeconds) + L" seconds.";
            return Status::Failed;
        }
    }
}

Status ConnectTcp(const std::wstring& host, uint16_t port, int timeoutSeconds, const CancelFn& cancel, SOCKET& out,
                  std::wstring& error) {
    out = INVALID_SOCKET;
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_protocol = IPPROTO_TCP;
    ADDRINFOW* list = nullptr;
    int rc = GetAddrInfoW(host.c_str(), std::to_wstring(port).c_str(), &hints, &list);
    if (rc != 0) {
        error = WsaError((L"Cannot find host \"" + host + L"\"").c_str(), rc);
        return Status::Failed;
    }
    Status result = Status::Failed;
    for (ADDRINFOW* a = list; a; a = a->ai_next) {
        SOCKET s = socket(a->ai_family, a->ai_socktype, a->ai_protocol);
        if (s == INVALID_SOCKET) continue;
        SetNonBlocking(s);
        if (connect(s, a->ai_addr, static_cast<int>(a->ai_addrlen)) == SOCKET_ERROR &&
            WSAGetLastError() != WSAEWOULDBLOCK) {
            error = WsaError(L"Cannot connect", WSAGetLastError());
            closesocket(s);
            continue;
        }
        Status w = WaitSocket(s, /*forWrite=*/true, timeoutSeconds, cancel, error);
        int soError = 0;
        int len = sizeof(soError);
        getsockopt(s, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&soError), &len);
        if (w == Status::Ok && soError == 0) {
            out = s;
            result = Status::Ok;
            break;
        }
        if (w == Status::Ok) error = WsaError(L"Cannot connect", soError);
        closesocket(s);
        if (w == Status::Cancelled) {
            result = Status::Cancelled;
            break;
        }
    }
    FreeAddrInfoW(list);
    return result;
}

Status SendAll(SOCKET s, const char* data, size_t length, int timeoutSeconds, const CancelFn& cancel, std::wstring& error) {
    size_t sent = 0;
    while (sent < length) {
        int n = send(s, data + sent, static_cast<int>(std::min<size_t>(length - sent, kChunk)), 0);
        if (n == SOCKET_ERROR) {
            if (WSAGetLastError() != WSAEWOULDBLOCK) {
                error = WsaError(L"Send failed", WSAGetLastError());
                return Status::Failed;
            }
            Status w = WaitSocket(s, true, timeoutSeconds, cancel, error);
            if (w != Status::Ok) return w;
            continue;
        }
        sent += static_cast<size_t>(n);
    }
    return Status::Ok;
}

// The address of the control connection's peer, used for passive data
// connections instead of the address a PASV reply names (servers behind NAT
// often name their private address).
bool PeerAddress(SOCKET s, sockaddr_storage& addr, int& length) {
    length = sizeof(addr);
    return getpeername(s, reinterpret_cast<sockaddr*>(&addr), &length) == 0;
}

std::wstring AddressText(const sockaddr_storage& addr) {
    wchar_t text[INET6_ADDRSTRLEN] = {};
    if (addr.ss_family == AF_INET)
        InetNtopW(AF_INET, &reinterpret_cast<const sockaddr_in&>(addr).sin_addr, text, INET6_ADDRSTRLEN);
    else
        InetNtopW(AF_INET6, &reinterpret_cast<const sockaddr_in6&>(addr).sin6_addr, text, INET6_ADDRSTRLEN);
    return text;
}

}  // namespace

Client::Client() {
    if (g_clients++ == 0) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
    }
}

Client::~Client() {
    CloseControl();
    if (--g_clients == 0) WSACleanup();
}

void Client::Trace(const std::wstring& line) const {
    if (trace_) trace_(line);
}

void Client::CloseControl() {
    if (control_ != INVALID_SOCKET) {
        closesocket(control_);
        control_ = INVALID_SOCKET;
    }
    reader_.Clear();
}

Status Client::SendLine(const std::string& line, const CancelFn& cancel, std::wstring& error) {
    if (control_ == INVALID_SOCKET) {
        error = L"Not connected.";
        return Status::ConnectionLost;
    }
    Trace(L"> " + Widen(MaskForLog(line)));  // FTP-006: never the password
    std::string wire = line + "\r\n";
    Status s = SendAll(control_, wire.data(), wire.size(), options_.timeoutSeconds, cancel, error);
    if (s == Status::Failed) {
        CloseControl();
        return Status::ConnectionLost;
    }
    return s;
}

Status Client::ReadReply(Reply& reply, const CancelFn& cancel, std::wstring& error, int timeoutSeconds) {
    if (timeoutSeconds < 0) timeoutSeconds = options_.timeoutSeconds;
    while (true) {
        if (auto next = reader_.Next()) {
            reply = *next;
            Trace(L"< " + std::to_wstring(reply.code) + L" " + Widen(reply.text));
            if (reply.code == 421) {
                error = L"The server closed the connection: " + Widen(reply.text);
                CloseControl();
                return Status::ConnectionLost;
            }
            return Status::Ok;
        }
        if (control_ == INVALID_SOCKET) {
            error = L"Not connected.";
            return Status::ConnectionLost;
        }
        Status w = WaitSocket(control_, false, timeoutSeconds, cancel, error);
        if (w != Status::Ok) {
            if (w == Status::Failed) CloseControl();  // a silent control connection is a lost one
            return w == Status::Failed ? Status::ConnectionLost : w;
        }
        char buffer[4096];
        int n = recv(control_, buffer, sizeof(buffer), 0);
        if (n == 0 || (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
            error = L"The connection to the server was lost.";
            CloseControl();
            return Status::ConnectionLost;
        }
        if (n > 0) reader_.Feed(buffer, static_cast<size_t>(n));
    }
}

Status Client::Command(const std::string& line, Reply& reply, const CancelFn& cancel, std::wstring& error) {
    Status s = SendLine(line, cancel, error);
    if (s != Status::Ok) return s;
    return ReadReply(reply, cancel, error);
}

Status Client::Connect(const ClientOptions& options, const CancelFn& cancel, std::wstring& error) {
    Disconnect();
    options_ = options;
    utf8_ = mlsd_ = false;
    epsvWorks_ = true;
    Trace(L"Connecting to " + options.host + L":" + std::to_wstring(options.port));
    Status s = ConnectTcp(options.host, options.port, options.timeoutSeconds, cancel, control_, error);
    if (s != Status::Ok) return s;

    Reply reply;
    do {  // 120 = "service ready in nnn minutes": wait for the real greeting
        s = ReadReply(reply, cancel, error);
        if (s != Status::Ok) return s;
    } while (reply.code == 120);
    if (reply.code != 220) {
        error = L"The server refused the connection: " + Widen(reply.text);
        CloseControl();
        return Status::Failed;
    }

    // Login. The user name may contain non-ASCII: send it as UTF-8 (most
    // servers expect that; OPTS UTF8 can only come after login on some).
    s = Command("USER " + EncodeName(options.user.empty() ? L"anonymous" : options.user, true), reply, cancel, error);
    if (s != Status::Ok) return s;
    if (reply.code == 331 || reply.code == 332) {
        std::wstring password = options.password;
        if (password.empty() && (options.user.empty() || _wcsicmp(options.user.c_str(), L"anonymous") == 0))
            password = L"mycommander@";
        s = Command("PASS " + EncodeName(password, true), reply, cancel, error);
        if (s != Status::Ok) return s;
    }
    if (reply.code != 230 && reply.code != 202) {
        error = L"Login failed: " + Widen(reply.text);
        Disconnect();
        return Status::Failed;
    }

    // FEAT is optional (RFC 2389); a 5xx just means "no features".
    s = Command("FEAT", reply, cancel, error);
    if (s != Status::Ok) return s;
    std::vector<std::string> features = reply.code == 211 ? ParseFeatures(reply.text) : std::vector<std::string>{};
    mlsd_ = HasFeature(features, "MLST");
    if (HasFeature(features, "UTF8")) {
        s = Command("OPTS UTF8 ON", reply, cancel, error);
        if (s != Status::Ok) return s;
        utf8_ = true;  // advertised: names are UTF-8 even if OPTS itself was refused (already on)
    }
    s = Command("TYPE I", reply, cancel, error);
    if (s != Status::Ok) return s;
    return Status::Ok;
}

void Client::Disconnect() {
    if (control_ != INVALID_SOCKET) {
        std::wstring ignored;
        Reply reply;
        if (SendLine("QUIT", {}, ignored) == Status::Ok) ReadReply(reply, {}, ignored, 2);
    }
    CloseControl();
}

Status Client::CurrentDirectory(std::wstring& path, const CancelFn& cancel, std::wstring& error) {
    Reply reply;
    Status s = Command("PWD", reply, cancel, error);
    if (s != Status::Ok) return s;
    auto dir = reply.code == 257 ? ParsePwdReply(reply.text) : std::nullopt;
    if (!dir) {
        error = L"Cannot read the current folder: " + Widen(reply.text);
        return Status::Failed;
    }
    path = DecodeName(*dir, utf8_);
    return Status::Ok;
}

Status Client::SimpleCommand(const std::string& verb, const std::wstring& path, int okClass, const CancelFn& cancel,
                             std::wstring& error) {
    Reply reply;
    Status s = Command(verb + " " + Encode(path), reply, cancel, error);
    if (s != Status::Ok) return s;
    if (reply.code / 100 != okClass) {
        error = Widen(reply.text);
        return Status::Failed;
    }
    return Status::Ok;
}

Status Client::ChangeDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error) {
    return SimpleCommand("CWD", path, 2, cancel, error);
}

Status Client::MakeRemoteDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error) {
    return SimpleCommand("MKD", path, 2, cancel, error);
}

Status Client::RemoveRemoteDirectory(const std::wstring& path, const CancelFn& cancel, std::wstring& error) {
    return SimpleCommand("RMD", path, 2, cancel, error);
}

Status Client::DeleteRemoteFile(const std::wstring& path, const CancelFn& cancel, std::wstring& error) {
    return SimpleCommand("DELE", path, 2, cancel, error);
}

Status Client::RenameRemote(const std::wstring& from, const std::wstring& to, const CancelFn& cancel, std::wstring& error) {
    Status s = SimpleCommand("RNFR", from, 3, cancel, error);
    if (s != Status::Ok) return s;
    return SimpleCommand("RNTO", to, 2, cancel, error);
}

Status Client::OpenData(SOCKET& socketOut, bool& isListener, const CancelFn& cancel, std::wstring& error) {
    socketOut = INVALID_SOCKET;
    isListener = false;
    Reply reply;
    Status s;
    if (options_.passive) {
        sockaddr_storage peer{};
        int peerLength = 0;
        if (!PeerAddress(control_, peer, peerLength)) {
            error = L"Cannot read the server's address.";
            return Status::Failed;
        }
        uint16_t port = 0;
        if (epsvWorks_) {
            s = Command("EPSV", reply, cancel, error);
            if (s != Status::Ok) return s;
            if (reply.code == 229 && ParseEpsvReply(reply.text, port)) {
                return ConnectTcp(AddressText(peer), port, options_.timeoutSeconds, cancel, socketOut, error);
            }
            epsvWorks_ = false;  // not supported: PASV from now on
        }
        if (peer.ss_family != AF_INET) {
            error = L"The server supports neither EPSV nor IPv6 passive mode.";
            return Status::Failed;
        }
        s = Command("PASV", reply, cancel, error);
        if (s != Status::Ok) return s;
        std::string ignoredHost;
        if (reply.code != 227 || !ParsePasvReply(reply.text, ignoredHost, port)) {
            error = L"Passive mode was refused: " + Widen(reply.text);
            return Status::Failed;
        }
        return ConnectTcp(AddressText(peer), port, options_.timeoutSeconds, cancel, socketOut, error);
    }

    // Active mode: listen on the control connection's local address.
    sockaddr_storage local{};
    int localLength = sizeof(local);
    if (getsockname(control_, reinterpret_cast<sockaddr*>(&local), &localLength) != 0) {
        error = WsaError(L"Cannot prepare active mode", WSAGetLastError());
        return Status::Failed;
    }
    SOCKET listener = socket(local.ss_family, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) {
        error = WsaError(L"Cannot prepare active mode", WSAGetLastError());
        return Status::Failed;
    }
    if (local.ss_family == AF_INET) reinterpret_cast<sockaddr_in&>(local).sin_port = 0;
    else reinterpret_cast<sockaddr_in6&>(local).sin6_port = 0;
    if (bind(listener, reinterpret_cast<sockaddr*>(&local), localLength) != 0 || listen(listener, 1) != 0 ||
        getsockname(listener, reinterpret_cast<sockaddr*>(&local), &localLength) != 0) {
        error = WsaError(L"Cannot prepare active mode", WSAGetLastError());
        closesocket(listener);
        return Status::Failed;
    }
    SetNonBlocking(listener);
    std::string command;
    if (local.ss_family == AF_INET) {
        const auto& v4 = reinterpret_cast<const sockaddr_in&>(local);
        const unsigned char* b = reinterpret_cast<const unsigned char*>(&v4.sin_addr);
        unsigned port = ntohs(v4.sin_port);
        command = "PORT " + std::to_string(b[0]) + "," + std::to_string(b[1]) + "," + std::to_string(b[2]) + "," +
                  std::to_string(b[3]) + "," + std::to_string(port / 256) + "," + std::to_string(port % 256);
    } else {
        unsigned port = ntohs(reinterpret_cast<const sockaddr_in6&>(local).sin6_port);
        command = "EPRT |2|" + EncodeName(AddressText(local), false) + "|" + std::to_string(port) + "|";
    }
    s = Command(command, reply, cancel, error);
    if (s != Status::Ok || reply.code / 100 != 2) {
        if (s == Status::Ok) error = L"Active mode was refused: " + Widen(reply.text);
        closesocket(listener);
        return s == Status::Ok ? Status::Failed : s;
    }
    socketOut = listener;
    isListener = true;
    return Status::Ok;
}

void Client::DrainAfterAbort() {
    std::wstring ignored;
    Reply first;
    if (ReadReply(first, {}, ignored, 3) != Status::Ok) return;
    if (first.code == 225) return;  // "no transfer to abort": the only answer
    // A second reply may follow; silence for a second just means there isn't
    // one -- not a lost connection, so this loop never closes the socket for it.
    const ULONGLONG deadline = GetTickCount64() + 1000;
    while (GetTickCount64() < deadline) {
        if (auto next = reader_.Next()) {
            Trace(L"< " + std::to_wstring(next->code) + L" " + Widen(next->text));
            return;
        }
        if (control_ == INVALID_SOCKET) return;
        std::wstring waitError;
        if (WaitSocket(control_, false, 1, {}, waitError) != Status::Ok) return;
        char buffer[4096];
        int n = recv(control_, buffer, sizeof(buffer), 0);
        if (n == 0 || (n == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK)) {
            CloseControl();
            return;
        }
        if (n > 0) reader_.Feed(buffer, static_cast<size_t>(n));
    }
}

Status Client::Transfer(const std::string& command, const std::function<Status(SOCKET)>& body, const CancelFn& cancel,
                        std::wstring& error) {
    SOCKET data = INVALID_SOCKET;
    bool isListener = false;
    Status s = OpenData(data, isListener, cancel, error);
    if (s != Status::Ok) return s;

    Reply reply;
    s = Command(command, reply, cancel, error);
    if (s != Status::Ok || (reply.code != 125 && reply.code != 150)) {
        if (s == Status::Ok) error = Widen(reply.text);
        closesocket(data);
        return s == Status::Ok ? Status::Failed : s;
    }
    if (isListener) {
        Status w = WaitSocket(data, false, options_.timeoutSeconds, cancel, error);
        SOCKET accepted = w == Status::Ok ? accept(data, nullptr, nullptr) : INVALID_SOCKET;
        closesocket(data);
        if (accepted == INVALID_SOCKET) {
            if (w == Status::Ok) error = L"The server did not open the data connection.";
            return w == Status::Ok ? Status::Failed : w;
        }
        data = accepted;
        SetNonBlocking(data);
    }

    Status bodyStatus = body(data);
    closesocket(data);

    if (bodyStatus == Status::Cancelled) {
        // Tell the server, then drain its replies (best effort, short wait).
        std::wstring ignored;
        if (SendLine("ABOR", {}, ignored) == Status::Ok) DrainAfterAbort();
        return Status::Cancelled;
    }
    s = ReadReply(reply, cancel, error);
    if (s != Status::Ok) return s;
    if (bodyStatus != Status::Ok) return bodyStatus;
    if (reply.code / 100 != 2) {
        error = Widen(reply.text);
        return Status::Failed;
    }
    return Status::Ok;
}

Status Client::List(const std::wstring& dir, std::vector<RemoteEntry>& entries, const CancelFn& cancel,
                    std::wstring& error) {
    entries.clear();
    Status s = ChangeDirectory(dir, cancel, error);
    if (s != Status::Ok) return s;
    std::string listing;
    auto body = [&](SOCKET data) {
        char buffer[kChunk];
        while (true) {
            Status w = WaitSocket(data, false, options_.timeoutSeconds, cancel, error);
            if (w != Status::Ok) return w;
            int n = recv(data, buffer, sizeof(buffer), 0);
            if (n == 0) return Status::Ok;
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
                error = WsaError(L"Listing failed", WSAGetLastError());
                return Status::Failed;
            }
            listing.append(buffer, static_cast<size_t>(n));
        }
    };
    s = Transfer(mlsd_ ? "MLSD" : "LIST", body, cancel, error);
    if (s != Status::Ok) return s;

    SYSTEMTIME now{};
    GetSystemTime(&now);
    size_t start = 0;
    while (start < listing.size()) {
        size_t end = listing.find('\n', start);
        if (end == std::string::npos) end = listing.size();
        std::string line = listing.substr(start, end - start);
        start = end + 1;
        auto entry = mlsd_ ? ParseMlsdLine(line, utf8_) : ParseListLine(line, now.wYear, utf8_);
        if (entry) entries.push_back(std::move(*entry));
    }
    return Status::Ok;
}

Status Client::Download(const std::wstring& remotePath, const std::wstring& localPath, uint64_t sizeHint,
                        const ProgressFn& progress, const CancelFn& cancel, std::wstring& error) {
    HANDLE file = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = WsaError((L"Cannot create \"" + localPath + L"\"").c_str(), static_cast<int>(GetLastError()));
        return Status::Failed;
    }
    uint64_t done = 0;
    auto body = [&](SOCKET data) {
        std::vector<char> buffer(kChunk);
        while (true) {
            Status w = WaitSocket(data, false, options_.timeoutSeconds, cancel, error);
            if (w != Status::Ok) return w;
            int n = recv(data, buffer.data(), static_cast<int>(buffer.size()), 0);
            if (n == 0) return Status::Ok;
            if (n == SOCKET_ERROR) {
                if (WSAGetLastError() == WSAEWOULDBLOCK) continue;
                error = WsaError(L"Download failed", WSAGetLastError());
                return Status::Failed;
            }
            DWORD written = 0;
            if (!WriteFile(file, buffer.data(), static_cast<DWORD>(n), &written, nullptr) ||
                written != static_cast<DWORD>(n)) {
                error = WsaError(L"Cannot write the local file", static_cast<int>(GetLastError()));
                return Status::Failed;
            }
            done += static_cast<uint64_t>(n);
            if (progress) progress(done, std::max(done, sizeHint));
        }
    };
    Status s = Transfer("RETR " + Encode(remotePath), body, cancel, error);
    CloseHandle(file);
    return s;
}

Status Client::Upload(const std::wstring& localPath, const std::wstring& remotePath, const ProgressFn& progress,
                      const CancelFn& cancel, std::wstring& error) {
    HANDLE file = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                              FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        error = WsaError((L"Cannot open \"" + localPath + L"\"").c_str(), static_cast<int>(GetLastError()));
        return Status::Failed;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(file, &size);
    uint64_t total = static_cast<uint64_t>(size.QuadPart), done = 0;
    auto body = [&](SOCKET data) {
        std::vector<char> buffer(kChunk);
        while (true) {
            if (cancel && cancel()) return Status::Cancelled;
            DWORD read = 0;
            if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr)) {
                error = WsaError(L"Cannot read the local file", static_cast<int>(GetLastError()));
                return Status::Failed;
            }
            if (read == 0) break;
            Status w = SendAll(data, buffer.data(), read, options_.timeoutSeconds, cancel, error);
            if (w != Status::Ok) return w;
            done += read;
            if (progress) progress(done, total);
        }
        shutdown(data, SD_SEND);
        return Status::Ok;
    };
    Status s = Transfer("STOR " + Encode(remotePath), body, cancel, error);
    CloseHandle(file);
    return s;
}

}  // namespace ftp
