#pragma once

// EXT-005: an in-memory FTP server on a loopback thread, shared by
// FtpPluginTests.cpp (protocol) and FtpPanelTests.cpp (plugin UI). Test-only.

#include "FtpClient.h"

#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>

namespace fakeftp {

// A tiny in-memory FTP server for the client tests. Blocking sockets on its
// own thread; one control connection at a time.

struct FakeServerOptions {
    bool mlsd = true;
    bool utf8 = true;
    bool epsv = true;
    std::string password = "secret";
    bool silentAfterLogin = false;   // stop answering after 230 (timeout test)
    bool dropOnList = false;         // close the control connection on LIST/MLSD
    size_t slowRetrBytes = 0;        // RETR sends this many bytes slowly (cancel test)
};

class FakeServer {
public:
    explicit FakeServer(FakeServerOptions options)
        : options_(std::move(options)), listener_(socket(AF_INET, SOCK_STREAM, IPPROTO_TCP)) {
        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        bind(listener_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
        int len = sizeof(addr);
        getsockname(listener_, reinterpret_cast<sockaddr*>(&addr), &len);
        port_ = ntohs(addr.sin_port);
        listen(listener_, 4);
        dirs_.insert("/");
        dirs_.insert("/pub");
        files_["/readme.txt"] = "hello ftp\n";
        files_["/pub/data.bin"] = std::string(200000, 'x');
        files_["/pub/\xC3\xA1rv\xC3\xADz.txt"] = "unicode name";  // "árvíz.txt" in UTF-8
        thread_ = std::thread([this] { Run(); });
    }
    ~FakeServer() {
        stop_ = true;
        closesocket(listener_);
        if (thread_.joinable()) thread_.join();
    }
    uint16_t Port() const { return port_; }
    std::map<std::string, std::string> Files() {
        std::lock_guard<std::mutex> lock(mutex_);
        return files_;
    }
    std::set<std::string> Dirs() {
        std::lock_guard<std::mutex> lock(mutex_);
        return dirs_;
    }
    std::string LastPassword() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lastPassword_;
    }

private:
    FakeServerOptions options_;
    SOCKET listener_ = INVALID_SOCKET;
    uint16_t port_ = 0;
    std::thread thread_;
    std::atomic<bool> stop_{false};
    std::mutex mutex_;
    std::map<std::string, std::string> files_;
    std::set<std::string> dirs_;
    std::string lastPassword_;

    static void SendText(SOCKET s, const std::string& text) { send(s, text.data(), static_cast<int>(text.size()), 0); }

    static std::string Join(const std::string& cwd, const std::string& arg) {
        if (arg.empty()) return cwd;
        if (arg[0] == '/') return arg;
        return cwd == "/" ? "/" + arg : cwd + "/" + arg;
    }

    void Run() {
        while (!stop_) {
            SOCKET c = accept(listener_, nullptr, nullptr);
            if (c == INVALID_SOCKET) return;
            Serve(c);
            closesocket(c);
        }
    }

    // Passive listener for one data connection.
    SOCKET pasv_ = INVALID_SOCKET;

    SOCKET AcceptData() {
        SOCKET d = accept(pasv_, nullptr, nullptr);
        closesocket(pasv_);
        pasv_ = INVALID_SOCKET;
        return d;
    }

    void Serve(SOCKET c) {
        SendText(c, "220 Fake FTP ready\r\n");
        std::string buffer, cwd = "/", renameFrom;
        bool loggedIn = false;
        while (true) {
            size_t eol;
            while ((eol = buffer.find("\r\n")) == std::string::npos) {
                char chunk[1024];
                int n = recv(c, chunk, sizeof(chunk), 0);
                if (n <= 0) return;
                buffer.append(chunk, static_cast<size_t>(n));
            }
            std::string line = buffer.substr(0, eol);
            buffer.erase(0, eol + 2);
            size_t sp = line.find(' ');
            std::string verb = line.substr(0, sp), arg = sp == std::string::npos ? "" : line.substr(sp + 1);
            for (auto& ch : verb) ch = static_cast<char>(toupper(static_cast<unsigned char>(ch)));

            if (loggedIn && options_.silentAfterLogin) continue;  // never answer again
            std::lock_guard<std::mutex> lock(mutex_);
            if (verb == "USER") SendText(c, "331 Password please\r\n");
            else if (verb == "PASS") {
                lastPassword_ = arg;
                if (arg == options_.password) { loggedIn = true; SendText(c, "230 Logged in\r\n"); }
                else SendText(c, "530 Login incorrect\r\n");
            } else if (verb == "FEAT") {
                std::string f = "211-Features:\r\n";
                if (options_.mlsd) f += " MLST type*;size*;modify*;\r\n";
                if (options_.utf8) f += " UTF8\r\n";
                f += " SIZE\r\n211 End\r\n";
                SendText(c, f);
            } else if (verb == "OPTS" || verb == "TYPE") SendText(c, "200 OK\r\n");
            else if (verb == "PWD") SendText(c, "257 \"" + cwd + "\" is current\r\n");
            else if (verb == "CWD") {
                std::string target = Join(cwd, arg);
                if (dirs_.count(target)) { cwd = target; SendText(c, "250 OK\r\n"); }
                else SendText(c, "550 No such folder\r\n");
            } else if (verb == "EPSV" || verb == "PASV") {
                if (verb == "EPSV" && !options_.epsv) { SendText(c, "500 Unknown\r\n"); continue; }
                pasv_ = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
                sockaddr_in a{};
                a.sin_family = AF_INET;
                a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
                bind(pasv_, reinterpret_cast<sockaddr*>(&a), sizeof(a));
                int len = sizeof(a);
                getsockname(pasv_, reinterpret_cast<sockaddr*>(&a), &len);
                listen(pasv_, 1);
                unsigned p = ntohs(a.sin_port);
                if (verb == "EPSV") SendText(c, "229 Extended (|||" + std::to_string(p) + "|)\r\n");
                else SendText(c, "227 Passive (10,0,0,99," + std::to_string(p / 256) + "," + std::to_string(p % 256) + ")\r\n");
            } else if (verb == "MLSD" || verb == "LIST") {
                if (options_.dropOnList) return;
                SendText(c, "150 Here it comes\r\n");
                SOCKET d = AcceptData();
                std::string out;
                std::string prefix = cwd == "/" ? "/" : cwd + "/";
                for (const auto& dir : dirs_) {
                    if (dir.size() > prefix.size() && dir.rfind(prefix, 0) == 0 && dir.find('/', prefix.size()) == std::string::npos) {
                        std::string name = dir.substr(prefix.size());
                        out += verb == "MLSD" ? "type=dir;modify=20260101120000; " + name + "\r\n"
                                              : "drwxr-xr-x 2 ftp ftp 0 Jan 01 12:00 " + name + "\r\n";
                    }
                }
                for (const auto& [path, content] : files_) {
                    if (path.rfind(prefix, 0) == 0 && path.find('/', prefix.size()) == std::string::npos) {
                        std::string name = path.substr(prefix.size());
                        out += verb == "MLSD" ? "type=file;size=" + std::to_string(content.size()) + ";modify=20260102030405; " + name + "\r\n"
                                              : "-rw-r--r-- 1 ftp ftp " + std::to_string(content.size()) + " Jan 02 03:04 " + name + "\r\n";
                    }
                }
                if (verb == "MLSD") out += "type=cdir; .\r\ntype=pdir; ..\r\n";
                SendText(d, out);
                closesocket(d);
                SendText(c, "226 Done\r\n");
            } else if (verb == "RETR") {
                std::string path = Join(cwd, arg);
                auto it = files_.find(path);
                if (it == files_.end() && options_.slowRetrBytes == 0) { SendText(c, "550 No such file\r\n"); continue; }
                SendText(c, "150 Sending\r\n");
                SOCKET d = AcceptData();
                if (options_.slowRetrBytes) {
                    std::string piece(1024, 'y');
                    for (size_t sent = 0; sent < options_.slowRetrBytes && !stop_; sent += piece.size()) {
                        if (send(d, piece.data(), static_cast<int>(piece.size()), 0) <= 0) break;
                        Sleep(20);
                    }
                    closesocket(d);
                    SendText(c, "426 Aborted\r\n");
                    continue;
                }
                SendText(d, it->second);
                closesocket(d);
                SendText(c, "226 Done\r\n");
            } else if (verb == "STOR") {
                SendText(c, "150 Ready\r\n");
                SOCKET d = AcceptData();
                std::string content;
                char chunk[4096];
                int n;
                while ((n = recv(d, chunk, sizeof(chunk), 0)) > 0) content.append(chunk, static_cast<size_t>(n));
                closesocket(d);
                files_[Join(cwd, arg)] = content;
                SendText(c, "226 Stored\r\n");
            } else if (verb == "DELE") {
                SendText(c, files_.erase(Join(cwd, arg)) ? "250 Deleted\r\n" : "550 No such file\r\n");
            } else if (verb == "MKD") {
                dirs_.insert(Join(cwd, arg));
                SendText(c, "257 \"" + Join(cwd, arg) + "\" created\r\n");
            } else if (verb == "RMD") {
                SendText(c, dirs_.erase(Join(cwd, arg)) ? "250 Removed\r\n" : "550 No such folder\r\n");
            } else if (verb == "RNFR") {
                renameFrom = Join(cwd, arg);
                SendText(c, files_.count(renameFrom) ? "350 Ready\r\n" : "550 No such file\r\n");
            } else if (verb == "RNTO") {
                auto node = files_.extract(renameFrom);
                // Like most servers, RNTO onto an existing file replaces it.
                if (node) { files_[Join(cwd, arg)] = std::move(node.mapped()); SendText(c, "250 Renamed\r\n"); }
                else SendText(c, "503 RNFR first\r\n");
            } else if (verb == "ABOR") SendText(c, "226 Aborted\r\n");
            else if (verb == "QUIT") { SendText(c, "221 Bye\r\n"); return; }
            else SendText(c, "502 Not implemented\r\n");
        }
    }
};

}  // namespace fakeftp
