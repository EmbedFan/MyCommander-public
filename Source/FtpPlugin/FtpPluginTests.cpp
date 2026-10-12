// EXT-005: standalone regression harness for the FTP plugin's protocol layer
// -- the pure parsers, the connections file, and ftp::Client against a small
// in-memory FTP server running on a loopback thread (no real network).
// Build from a Developer Command Prompt (the plugin's own sources compiled in):
//   cl /std:c++20 /EHsc /MTd /W4 /FoBuild\ /FeBuild\FtpPluginTests.exe Source\FtpPlugin\FtpPluginTests.cpp
//      Source\FtpPlugin\FtpParse.cpp Source\FtpPlugin\FtpClient.cpp Source\FtpPlugin\FtpConnections.cpp
//      Source\PluginCommon\RemoteConnections.cpp Source\PluginCommon\RemoteUi.cpp
// Run: Build\FtpPluginTests.exe   (exit code 0 = every check passed)

#include "FtpClient.h"
#include "FtpConnections.h"
#include "FtpParse.h"
#include "FtpPluginUi.h"
#include "FakeFtpServer.h"

#include <atomic>
#include <cstdio>
#include <map>
#include <mutex>
#include <set>
#include <thread>

using fakeftp::FakeServer;
using fakeftp::FakeServerOptions;

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

ftp::ClientOptions LocalOptions(uint16_t port, const std::wstring& password = L"secret") {
    ftp::ClientOptions o;
    o.host = L"127.0.0.1";
    o.port = port;
    o.user = L"tester";
    o.password = password;
    o.timeoutSeconds = 5;
    return o;
}

std::wstring TempFile(const wchar_t* prefix) {
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    GetTempPathW(MAX_PATH, dir);
    GetTempFileNameW(dir, prefix, 0, path);
    return path;
}

std::string ReadAll(const std::wstring& path) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) return "<missing>";
    std::string data;
    char buffer[65536];
    DWORD read = 0;
    while (ReadFile(h, buffer, sizeof(buffer), &read, nullptr) && read > 0) data.append(buffer, read);
    CloseHandle(h);
    return data;
}

void WriteAll(const std::wstring& path, const std::string& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    DWORD written = 0;
    WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
}

// ---------------------------------------------------------------------------

void TestParsers() {
    ftp::ReplyReader reader;
    reader.Feed("220 Hi\r\n230-Welcome\r\n", 21);
    auto r = reader.Next();
    Check(r && r->code == 220 && r->text == "Hi", "single-line reply");
    Check(!reader.Next().has_value(), "incomplete multi-line waits");
    reader.Feed(" line two\r\n230 Done\r\n", 21);
    r = reader.Next();
    Check(r && r->code == 230 && r->text == "Welcome\n line two\nDone", "multi-line reply");

    std::string host;
    uint16_t port = 0;
    Check(ftp::ParsePasvReply("Entering Passive Mode (192,168,1,2,19,137)", host, port) && host == "192.168.1.2" &&
              port == 19 * 256 + 137, "PASV parsed");
    Check(!ftp::ParsePasvReply("Entering Passive Mode (300,1,1,1,1,1)", host, port), "PASV rejects >255");
    Check(ftp::ParseEpsvReply("Entering Extended Passive Mode (|||6446|)", port) && port == 6446, "EPSV parsed");
    Check(!ftp::ParseEpsvReply("Entering Extended Passive Mode (||6446|)", port), "EPSV malformed rejected");

    auto m = ftp::ParseMlsdLine("type=file;size=1234;modify=20260102030405;UNIX.mode=0644; my file.txt\r", true);
    Check(m && m->name == L"my file.txt" && m->size == 1234 && !m->isDirectory && m->permissions == L"rw-r--r--" &&
              m->modifiedUtc != 0, "MLSD file line");
    Check(!ftp::ParseMlsdLine("type=cdir; .", true).has_value(), "MLSD cdir skipped");
    auto md = ftp::ParseMlsdLine("Type=DIR;Modify=20260101000000; sub", true);
    Check(md && md->isDirectory && md->name == L"sub", "MLSD dir, case-insensitive facts");
    auto mu = ftp::ParseMlsdLine("type=file;size=1; \xC3\xA1rv\xC3\xADz.txt", true);
    Check(mu && mu->name == L"árvíz.txt", "MLSD UTF-8 name");
    if (mu && mu->name != L"árvíz.txt") {
        std::printf("  got:");
        for (wchar_t ch : mu->name) std::printf(" %04X", static_cast<unsigned>(ch));
        std::printf("\n  expected:");
        for (wchar_t ch : std::wstring(L"árvíz.txt")) std::printf(" %04X", static_cast<unsigned>(ch));
        std::printf("\n");
    }

    auto u = ftp::ParseListLine("-rw-r--r--   1 owner group     4096 Jan 15 12:30 name with spaces.txt", 2026, true);
    Check(u && u->name == L"name with spaces.txt" && u->size == 4096 && u->permissions == L"rw-r--r--", "Unix LIST file");
    auto ud = ftp::ParseListLine("drwxr-xr-x 2 ftp ftp 0 Mar  3  2021 old", 2026, true);
    Check(ud && ud->isDirectory && ud->name == L"old", "Unix LIST dir with year");
    auto ul = ftp::ParseListLine("lrwxrwxrwx 1 root root 7 Feb 1 10:00 latest -> v1.2.3", 2026, true);
    Check(ul && ul->isLink && ul->name == L"latest", "Unix LIST symlink");
    auto un = ftp::ParseListLine("-rw-r--r-- 1 4096 Jan 15 12:30 noowner", 2026, true);
    Check(un && un->name == L"noowner" && un->size == 4096, "Unix LIST without owner/group");
    Check(!ftp::ParseListLine("total 42", 2026, true).has_value(), "LIST total line skipped");
    Check(!ftp::ParseListLine("drwxr-xr-x 2 ftp ftp 0 Jan 01 12:00 .", 2026, true).has_value(), "LIST dot skipped");
    auto w = ftp::ParseListLine("10-08-26  09:15PM       <DIR>          My Folder", 2026, true);
    Check(w && w->isDirectory && w->name == L"My Folder", "Windows LIST dir");
    auto wf = ftp::ParseListLine("10-08-2026  09:15AM              1234 report.pdf", 2026, true);
    Check(wf && !wf->isDirectory && wf->size == 1234 && wf->name == L"report.pdf", "Windows LIST file");
    auto odd = ftp::ParseListLine("something unexpected", 2026, true);
    Check(odd && odd->name == L"something unexpected", "unknown LIST line kept as a name");

    auto features = ftp::ParseFeatures("Features:\n MLST type*;size*;\n UTF8\n EPSV\nEnd");
    Check(ftp::HasFeature(features, "MLST") && ftp::HasFeature(features, "utf8") && !ftp::HasFeature(features, "MDTM"), "FEAT");
    Check(ftp::ParseFeatures("No features").empty(), "FEAT single line");

    Check(ftp::ParseFtpTime("20260102030405").has_value() && !ftp::ParseFtpTime("2026").has_value(), "MDTM time");
    Check(ftp::JoinRemotePath(L"/", L"a") == L"/a" && ftp::JoinRemotePath(L"/a", L"b") == L"/a/b", "join path");
    Check(ftp::ParentRemotePath(L"/a/b") == L"/a" && ftp::ParentRemotePath(L"/a") == L"/" && ftp::ParentRemotePath(L"/") == L"/",
          "parent path");
    Check(ftp::ParsePwdReply("\"/home/\"\"q\"\"\" is cwd") == std::optional<std::string>("/home/\"q\""), "PWD with quotes");
    Check(ftp::MaskForLog("PASS hunter2") == "PASS ****" && ftp::MaskForLog("USER bob") == "USER bob", "PASS masked");

    auto url = ftp::ParseFtpUrl(L"ftp://bob:p%40ss@example.com:2121/pub/files");
    Check(url && url->host == L"example.com" && url->port == 2121 && url->user == L"bob" && url->password == L"p@ss" &&
              url->path == L"/pub/files", "ftp:// URL");
    auto bare = ftp::ParseFtpUrl(L"files.example.org");
    Check(bare && bare->host == L"files.example.org" && bare->port == 21 && bare->user.empty(), "bare host");
    Check(!ftp::ParseFtpUrl(L"ftp://host:99999").has_value() && !ftp::ParseFtpUrl(L"ftp://").has_value(), "bad URLs");
}

void TestConnectionsFile() {
    std::vector<std::wstring> warnings;
    auto list = ftp::ParseConnections(L"# c\n[Work]\nhost = ftp.example.com\nport = 2121\nuser = bob\nfolder = /pub\n"
                                      L"passive = no\ntimeout = 30\nsave-password = yes\npassword = oops\ncolour = red\n"
                                      L"[Broken]\nuser = x\n[Árvíz]\nhost = h\n",
                                      warnings);
    Check(list.size() == 2, "connections: two valid blocks");
    if (list.size() == 2) {
        const auto& w = list[0];
        Check(w.name == L"Work" && w.host == L"ftp.example.com" && w.port == 2121 && w.user == L"bob" && w.folder == L"/pub" &&
                  !w.passive && w.timeoutSeconds == 30 && w.savePassword, "connections: fields");
        Check(list[1].name == L"Árvíz", "connections: Unicode name");
    }
    bool passwordWarned = false, unknownWarned = false, noHostWarned = false;
    for (const auto& x : warnings) {
        passwordWarned |= x.find(L"never read") != std::wstring::npos;
        unknownWarned |= x.find(L"colour") != std::wstring::npos;
        noHostWarned |= x.find(L"no host") != std::wstring::npos;
    }
    Check(passwordWarned && unknownWarned && noHostWarned, "connections: warnings");
    std::wstring text = ftp::FormatConnections(list);
    Check(text.find(L"oops") == std::wstring::npos && text.find(L"\npassword") == std::wstring::npos, "no password written");
    warnings.clear();
    auto again = ftp::ParseConnections(text, warnings);
    Check(warnings.empty() && again.size() == 2 && again[0].port == 2121 && !again[0].passive, "connections round trip");
}

void TestClient() {
    // Login, MLSD listing, UTF-8 names, download, upload, file operations.
    {
        FakeServer server(FakeServerOptions{});
        ftp::Client client;
        std::vector<std::wstring> trace;
        client.SetTrace([&](const std::wstring& line) { trace.push_back(line); });
        std::wstring error;
        Check(client.Connect(LocalOptions(server.Port()), {}, error) == ftp::Status::Ok, "connect + login");
        if (!client.Connected()) std::wprintf(L"  connect error: %s\n", error.c_str());
        Check(client.Utf8() && client.UsesMlsd(), "FEAT: UTF8 + MLST");
        bool sawSecret = false, sawMasked = false;
        for (const auto& line : trace) {
            sawSecret |= line.find(L"secret") != std::wstring::npos;
            sawMasked |= line.find(L"PASS ****") != std::wstring::npos;
        }
        Check(!sawSecret && sawMasked, "trace never contains the password (FTP-006)");

        std::vector<ftp::RemoteEntry> entries;
        Check(client.List(L"/pub", entries, {}, error) == ftp::Status::Ok, "MLSD list");
        bool unicode = false, data = false;
        for (const auto& e : entries) {
            unicode |= e.name == L"árvíz.txt";
            data |= e.name == L"data.bin" && e.size == 200000;
        }
        Check(entries.size() == 2 && unicode && data, "MLSD entries (cdir/pdir skipped, UTF-8)");

        std::wstring local = TempFile(L"mft");
        uint64_t lastDone = 0;
        Check(client.Download(L"/pub/data.bin", local, 200000, [&](uint64_t d, uint64_t) { lastDone = d; }, {}, error) ==
                  ftp::Status::Ok, "download");
        Check(ReadAll(local) == std::string(200000, 'x') && lastDone == 200000, "downloaded bytes and progress");

        WriteAll(local, "uploaded content");
        Check(client.Upload(local, L"/pub/new.txt", {}, {}, error) == ftp::Status::Ok, "upload");
        Check(server.Files()[std::string("/pub/new.txt")] == "uploaded content", "uploaded bytes on the server");
        DeleteFileW(local.c_str());

        Check(client.MakeRemoteDirectory(L"/pub/made", {}, error) == ftp::Status::Ok && server.Dirs().count("/pub/made"), "MKD");
        Check(client.RemoveRemoteDirectory(L"/pub/made", {}, error) == ftp::Status::Ok && !server.Dirs().count("/pub/made"), "RMD");
        Check(client.RenameRemote(L"/pub/new.txt", L"/pub/renamed.txt", {}, error) == ftp::Status::Ok &&
                  server.Files().count("/pub/renamed.txt"), "RNFR/RNTO");
        Check(client.DeleteRemoteFile(L"/pub/renamed.txt", {}, error) == ftp::Status::Ok && !server.Files().count("/pub/renamed.txt"),
              "DELE");
        Check(client.DeleteRemoteFile(L"/pub/missing", {}, error) == ftp::Status::Failed && !error.empty(), "DELE failure reported");
        std::wstring cwd;
        Check(client.CurrentDirectory(cwd, {}, error) == ftp::Status::Ok && cwd == L"/pub", "PWD");
        client.Disconnect();
        Check(!client.Connected(), "disconnected");
    }
    // LIST fallback without MLSD/UTF8, and PASV fallback without EPSV.
    {
        FakeServerOptions o;
        o.mlsd = false;
        o.utf8 = false;
        o.epsv = false;
        FakeServer server(o);
        ftp::Client client;
        std::wstring error;
        Check(client.Connect(LocalOptions(server.Port()), {}, error) == ftp::Status::Ok && !client.UsesMlsd() && !client.Utf8(),
              "connect without MLST/UTF8");
        std::vector<ftp::RemoteEntry> entries;
        Check(client.List(L"/", entries, {}, error) == ftp::Status::Ok, "LIST via PASV (private PASV address ignored)");
        bool pub = false, readme = false;
        for (const auto& e : entries) {
            pub |= e.name == L"pub" && e.isDirectory;
            readme |= e.name == L"readme.txt" && e.size == 10;
        }
        Check(pub && readme, "LIST entries");
    }
    // Wrong password.
    {
        FakeServer server(FakeServerOptions{});
        ftp::Client client;
        std::wstring error;
        Check(client.Connect(LocalOptions(server.Port(), L"wrong"), {}, error) == ftp::Status::Failed &&
                  error.find(L"Login failed") != std::wstring::npos && !client.Connected(), "login failure");
    }
    // A server that goes silent: bounded by the timeout, reported as lost.
    {
        FakeServerOptions o;
        o.silentAfterLogin = true;
        FakeServer server(o);
        ftp::Client client;
        std::wstring error;
        auto options = LocalOptions(server.Port());
        options.timeoutSeconds = 1;
        ULONGLONG start = GetTickCount64();
        ftp::Status s = client.Connect(options, {}, error);
        ULONGLONG elapsed = GetTickCount64() - start;
        Check(s == ftp::Status::ConnectionLost && elapsed < 4000, "silent server times out (FTP-009)");
    }
    // A dropped control connection during a listing.
    {
        FakeServerOptions o;
        o.dropOnList = true;
        FakeServer server(o);
        ftp::Client client;
        std::wstring error;
        client.Connect(LocalOptions(server.Port()), {}, error);
        std::vector<ftp::RemoteEntry> entries;
        Check(client.List(L"/", entries, {}, error) == ftp::Status::ConnectionLost && !client.Connected(), "dropped connection");
    }
    // Cancel mid-transfer.
    {
        FakeServerOptions o;
        o.slowRetrBytes = 10 * 1024 * 1024;
        FakeServer server(o);
        ftp::Client client;
        std::wstring error;
        client.Connect(LocalOptions(server.Port()), {}, error);
        std::wstring local = TempFile(L"mfc");
        uint64_t got = 0;
        ULONGLONG start = GetTickCount64();
        ftp::Status s = client.Download(L"/slow.bin", local, 0, [&](uint64_t d, uint64_t) { got = d; },
                                        [&] { return got >= 8192; }, error);
        Check(s == ftp::Status::Cancelled && GetTickCount64() - start < 3000, "Esc stops a transfer promptly");
        DeleteFileW(local.c_str());
    }
}

void TestUiHelpers() {
    // Download to a temporary name; a failure keeps the original intact (FTP-003).
    std::wstring target = TempFile(L"mfu");
    WriteAll(target, "original");
    bool ok = ftpui::DownloadReplacing(target, [&](const std::wstring& temp) {
        WriteAll(temp, "partial");
        return false;  // the transfer failed
    });
    Check(!ok && ReadAll(target) == "original", "failed download leaves the original file");
    ok = ftpui::DownloadReplacing(target, [&](const std::wstring& temp) {
        WriteAll(temp, "new content");
        return true;
    });
    Check(ok && ReadAll(target) == "new content", "successful download replaces the file");
    DeleteFileW(target.c_str());

    Check(ftpui::FormatPanelTitle(L"bob", L"example.com", 21, L"/pub") == L"ftp://bob@example.com/pub", "panel title");
    Check(ftpui::FormatPanelTitle(L"anonymous", L"h", 2121, L"/") == L"ftp://h:2121/", "panel title: anonymous + port");
    Check(ftpui::FormatPanelTitle(L"x", L"::1", 21, L"/") == L"ftp://x@[::1]/", "panel title: IPv6 host");
}

}  // namespace

int wmain() {
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);  // the fake server opens sockets before any ftp::Client exists
    TestParsers();
    TestConnectionsFile();
    TestClient();
    TestUiHelpers();
    std::printf("FTP plugin tests: %d checks, %d failures\n", checks, failures);
    return failures == 0 ? 0 : 1;
}
