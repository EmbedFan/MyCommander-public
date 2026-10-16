// EXT-004: opt-in integration harness for sftp::Client against a REAL SSH
// server (there is no practical in-process SSH server to fake). Skipped --
// exit code 0, "skipped" printed -- unless MC_SFTP_TEST_HOST is set.
//
// NEEDS A REAL SFTP SERVER -- every check in this file:
//   [server]           any server, read-only is enough (e.g. test.rebex.net):
//                      connect, host-key prompt/remember/changed-key refusal
//                      (SFTP-007), home, list, download, missing-folder
//                      error, no secret in the trace (SFTP-006)
//   [writable server]  additionally MC_SFTP_TEST_DIR: W1-W15 below
// Without a server, only SftpLogicTests and SftpPanelTests (an in-memory fake
// server) run; they cover the panel's write paths but not this client.
//
//   MC_SFTP_TEST_HOST        server name or address (required)
//   MC_SFTP_TEST_PORT        default 22
//   MC_SFTP_TEST_USER        user name (required)
//   MC_SFTP_TEST_PASSWORD    password login, or
//   MC_SFTP_TEST_KEY         private key file (+ MC_SFTP_TEST_PASSPHRASE), or
//   MC_SFTP_TEST_AGENT=1     the SSH agent
//   MC_SFTP_TEST_DIR         a writable server folder for the write tests;
//                            without it only read-only checks run (e.g. the
//                            public test.rebex.net, user demo, password password)
//   MC_SFTP_TEST_READONLY_DIR  optional: a server folder the user can list but
//                            not write, for the permission-denied check (W15)
//
// Write checks (all inside one fresh mc_it_<tick> folder, removed at the end):
// W1 mkdir, W2 upload, W3 rename, W4 download, W5 Esc mid-upload, W8 overwrite,
// W10 50 MB round trip, W9 Esc mid-download (no partial file, the old one
// intact), W11 Unicode names, W12 "- odd name.txt", W13 recursive delete,
// W14 a symlink is never followed by a recursive delete (skipped if the server
// refuses to create one), W15 permission denied, W6 delete, W7 rmdir.
//
// Host keys go to temporary known_hosts files: the real %APPDATA% and .ssh
// files are never read or written. Build: Source\SftpPlugin\BuildTests.cmd.

#include "SftpClient.h"
#include "../PluginCommon/RemoteUi.h"

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

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

std::wstring Env(const wchar_t* name) {
    wchar_t* value = nullptr;
    size_t length = 0;
    if (_wdupenv_s(&value, &length, name) != 0 || !value) return L"";
    std::wstring result = value;
    free(value);
    return result;
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

bool LocalExists(const std::wstring& path) { return GetFileAttributesW(path.c_str()) != INVALID_FILE_ATTRIBUTES; }

// Non-repeating bytes, so a chunk written to the wrong offset can't compare equal.
std::string Pattern(size_t size) {
    std::string data(size, '\0');
    uint32_t x = 2463534242u;
    for (auto& c : data) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        c = static_cast<char>(x);
    }
    return data;
}

bool FindEntry(sftp::Client& client, const std::wstring& dir, const std::wstring& name, remote::RemoteEntry* found = nullptr) {
    std::vector<remote::RemoteEntry> entries;
    std::wstring error;
    if (client.List(dir, entries, {}, error) != remote::Status::Ok) return false;
    for (const auto& e : entries)
        if (e.name == name) {
            if (found) *found = e;
            return true;
        }
    return false;
}

// The same rule as RemotePanel.cpp's DeleteTree: children first, then the
// folder; a link (even to a folder) is removed itself, never followed.
remote::Status DeleteTree(sftp::Client& client, const std::wstring& path, bool isDirectory, std::wstring& error) {
    if (!isDirectory) return client.DeleteRemoteFile(path, {}, error);
    std::vector<remote::RemoteEntry> children;
    remote::Status s = client.List(path, children, {}, error);
    if (s != remote::Status::Ok) return s;
    for (const auto& c : children) {
        s = DeleteTree(client, remote::JoinRemotePath(path, c.name), c.isDirectory && !c.isLink, error);
        if (s != remote::Status::Ok) return s;
    }
    return client.RemoveRemoteDirectory(path, {}, error);
}

}  // namespace

int wmain() {
    remote::Connection c;
    c.host = Env(L"MC_SFTP_TEST_HOST");
    if (c.host.empty()) {
        std::printf("SFTP integration tests: skipped -- they need a real SFTP server\n"
                    "  [server]           read-only checks: set MC_SFTP_TEST_HOST/USER/PASSWORD\n"
                    "  [writable server]  W1-W15: also set MC_SFTP_TEST_DIR (see the file header)\n");
        return 0;
    }
    c.user = Env(L"MC_SFTP_TEST_USER");
    std::wstring port = Env(L"MC_SFTP_TEST_PORT");
    c.port = port.empty() ? 22 : static_cast<uint16_t>(std::stoi(port));
    c.timeoutSeconds = 20;
    std::wstring secret = Env(L"MC_SFTP_TEST_PASSWORD");
    if (!Env(L"MC_SFTP_TEST_KEY").empty()) {
        c.auth = remote::AuthMethod::KeyFile;
        c.keyFile = Env(L"MC_SFTP_TEST_KEY");
        secret = Env(L"MC_SFTP_TEST_PASSPHRASE");
    } else if (Env(L"MC_SFTP_TEST_AGENT") == L"1") {
        c.auth = remote::AuthMethod::Agent;
    }
    std::wstring writable = Env(L"MC_SFTP_TEST_DIR");

    wchar_t tempDir[MAX_PATH];
    GetTempPathW(MAX_PATH, tempDir);
    std::wstring work = std::wstring(tempDir) + L"mc_sftp_it_" + std::to_wstring(GetTickCount64());
    CreateDirectoryW(work.c_str(), nullptr);
    sftp::KnownHostsFiles files{work + L"\\own_known_hosts", work + L"\\user_known_hosts"};

    std::vector<std::wstring> trace;
    auto traceFn = [&](const std::wstring& line) { trace.push_back(line); };
    std::wstring error;

    // 1. Unknown host: asked, trusted and remembered.
    sftp::HostKeyInfo seen;
    {
        int asked = 0;
        sftp::Client client([&](const sftp::HostKeyInfo& info) { ++asked; seen = info; return sftp::HostKeyDecision::AcceptAndRemember; },
                            nullptr, files);
        client.SetTrace(traceFn);
        remote::Status s = client.Connect(c, secret, {}, error);
        Check(s == remote::Status::Ok, "connect + log in");
        if (s != remote::Status::Ok) {
            std::wprintf(L"  error: %s\n", error.c_str());
            for (const auto& t : trace) std::wprintf(L"  trace: %s\n", t.c_str());
            std::printf("SFTP integration tests: %d checks, %d failures\n", checks, failures);
            return 1;
        }
        Check(asked == 1 && seen.verdict == sftp::HostKeyVerdict::Unknown && seen.fingerprint.rfind(L"SHA256:", 0) == 0,
              "first connection: the unknown host key is shown");
        Check(ReadAll(files.own).find("added by MyCommander") != std::string::npos, "Trust and remember writes known_hosts");
        std::wstring home;
        Check(client.HomeDirectory(home, {}, error) == remote::Status::Ok && !home.empty() && home[0] == L'/', "home folder");
        std::vector<remote::RemoteEntry> entries;
        Check(client.List(home, entries, {}, error) == remote::Status::Ok, "list the home folder");
        std::wprintf(L"  home %s: %zu entries\n", home.c_str(), entries.size());
        // Read-only: download the first small file found in home.
        for (const auto& e : entries) {
            if (e.isDirectory || e.size > 1000000) continue;
            std::wstring local = work + L"\\download.bin";
            Check(client.Download(remote::JoinRemotePath(home, e.name), local, e.size, {}, {}, error) == remote::Status::Ok &&
                      ReadAll(local).size() == e.size,
                  "download a file (size matches)");
            break;
        }
        std::vector<remote::RemoteEntry> none;
        Check(client.List(L"/no/such/folder/for/mycommander", none, {}, error) == remote::Status::Failed && !error.empty(),
              "a missing folder is an error, not a lost connection");
        Check(client.Connected(), "still connected after an error");

        if (writable.empty()) {
            std::printf("  [writable server] W1-W15: skipped -- set MC_SFTP_TEST_DIR to a writable server folder\n");
        } else {
            std::wstring dir = remote::JoinRemotePath(writable, L"mc_it_" + std::to_wstring(GetTickCount64()));
            Check(client.MakeRemoteDirectory(dir, {}, error) == remote::Status::Ok, "mkdir");
            std::string content(300000, 'z');
            WriteAll(work + L"\\up.bin", content);
            Check(client.Upload(work + L"\\up.bin", dir + L"/up.bin", {}, {}, error) == remote::Status::Ok, "upload");
            Check(client.RenameRemote(dir + L"/up.bin", dir + L"/renamed.bin", {}, error) == remote::Status::Ok, "rename");
            Check(client.Download(dir + L"/renamed.bin", work + L"\\back.bin", content.size(), {}, {}, error) ==
                          remote::Status::Ok &&
                      ReadAll(work + L"\\back.bin") == content,
                  "download what was uploaded (identical)");
            // Esc mid-transfer: cancelled between chunks, the session survives.
            int progressCalls = 0;
            remote::Status cancelled = client.Upload(work + L"\\up.bin", dir + L"/partial.bin",
                                                     [&](uint64_t, uint64_t) { ++progressCalls; }, [&] { return progressCalls > 0; }, error);
            Check(cancelled == remote::Status::Cancelled, "Esc stops an upload");
            std::vector<remote::RemoteEntry> listing;
            Check(client.List(dir, listing, {}, error) == remote::Status::Ok, "the session is still usable after Esc");
            client.DeleteRemoteFile(dir + L"/partial.bin", {}, error);

            // W8: an upload over an existing file replaces it completely.
            {
                std::string second = Pattern(123457);
                WriteAll(work + L"\\over.bin", second);
                remote::RemoteEntry e;
                Check(client.Upload(work + L"\\over.bin", dir + L"/renamed.bin", {}, {}, error) == remote::Status::Ok &&
                          FindEntry(client, dir, L"renamed.bin", &e) && e.size == second.size(),
                      "W8: an upload over an existing file has the new size");
                Check(client.Download(dir + L"/renamed.bin", work + L"\\back.bin", second.size(), {}, {}, error) ==
                              remote::Status::Ok &&
                          ReadAll(work + L"\\back.bin") == second,
                      "W8: ...and the new content");
                DeleteFileW((work + L"\\over.bin").c_str());
            }

            // W10: 50 MB up and down, identical, with the throughput.
            const std::wstring large = dir + L"/large.bin";
            {
                std::string big = Pattern(50u * 1024 * 1024);
                WriteAll(work + L"\\large.bin", big);
                ULONGLONG t0 = GetTickCount64();
                bool up = client.Upload(work + L"\\large.bin", large, {}, {}, error) == remote::Status::Ok;
                ULONGLONG t1 = GetTickCount64();
                bool down = up && client.Download(large, work + L"\\large_back.bin", big.size(), {}, {}, error) ==
                                      remote::Status::Ok;
                ULONGLONG t2 = GetTickCount64();
                Check(up && down && ReadAll(work + L"\\large_back.bin") == big, "W10: 50 MB up and down (identical)");
                auto mbps = [](ULONGLONG ms) { return ms ? 50.0 * 1000.0 / static_cast<double>(ms) : 0.0; };
                std::printf("  W10: upload %.1f MB/s, download %.1f MB/s\n", mbps(t1 - t0), mbps(t2 - t1));
                DeleteFileW((work + L"\\large.bin").c_str());
                DeleteFileW((work + L"\\large_back.bin").c_str());
            }

            // W9: Esc mid-download leaves no partial file and the old local file intact.
            {
                const std::wstring target = work + L"\\target.bin";
                WriteAll(target, "original");
                int calls = 0;
                bool replaced = remote::DownloadReplacing(target, L".mcsftp-part", [&](const std::wstring& temp) {
                    return client.Download(large, temp, 50u * 1024 * 1024, [&](uint64_t, uint64_t) { ++calls; },
                                           [&] { return calls > 0; }, error) == remote::Status::Ok;
                });
                Check(!replaced && ReadAll(target) == "original", "W9: Esc mid-download keeps the existing local file");
                Check(!LocalExists(target + L".mcsftp-part"), "W9: ...and leaves no .mcsftp-part file");
                Check(FindEntry(client, dir, L"large.bin"), "W9: the session is still usable after Esc");
                DeleteFileW(target.c_str());
                client.DeleteRemoteFile(large, {}, error);
            }

            // W11 / W12: names that need UTF-8 on the wire, and quoting-hostile ones.
            {
                std::string small = Pattern(4096);
                WriteAll(work + L"\\small.bin", small);
                const std::wstring unicodeName = L"árvíztűrő_тест_日本.txt";
                const std::wstring unicodeRenamed = L"日本_ő_renamed.txt";
                Check(client.Upload(work + L"\\small.bin", dir + L"/" + unicodeName, {}, {}, error) == remote::Status::Ok &&
                          FindEntry(client, dir, unicodeName),
                      "W11: a Unicode name uploads and lists unchanged");
                Check(client.Download(dir + L"/" + unicodeName, work + L"\\back.bin", small.size(), {}, {}, error) ==
                              remote::Status::Ok &&
                          ReadAll(work + L"\\back.bin") == small,
                      "W11: ...downloads");
                Check(client.RenameRemote(dir + L"/" + unicodeName, dir + L"/" + unicodeRenamed, {}, error) ==
                              remote::Status::Ok &&
                          FindEntry(client, dir, unicodeRenamed) && !FindEntry(client, dir, unicodeName),
                      "W11: ...renames");
                Check(client.DeleteRemoteFile(dir + L"/" + unicodeRenamed, {}, error) == remote::Status::Ok,
                      "W11: ...and deletes");

                const std::wstring odd = L"- odd name.txt";
                Check(client.Upload(work + L"\\small.bin", dir + L"/" + odd, {}, {}, error) == remote::Status::Ok &&
                          FindEntry(client, dir, odd),
                      "W12: \"- odd name.txt\" uploads and lists unchanged");
                Check(client.Download(dir + L"/" + odd, work + L"\\back.bin", small.size(), {}, {}, error) ==
                              remote::Status::Ok &&
                          ReadAll(work + L"\\back.bin") == small,
                      "W12: ...downloads");
                Check(client.DeleteRemoteFile(dir + L"/" + odd, {}, error) == remote::Status::Ok, "W12: ...and deletes");
            }

            // W13: recursive delete of a folder with files and a subfolder.
            {
                const std::wstring tree = dir + L"/tree";
                bool built = client.MakeRemoteDirectory(tree, {}, error) == remote::Status::Ok &&
                             client.MakeRemoteDirectory(tree + L"/sub", {}, error) == remote::Status::Ok &&
                             client.Upload(work + L"\\small.bin", tree + L"/a.txt", {}, {}, error) == remote::Status::Ok &&
                             client.Upload(work + L"\\small.bin", tree + L"/sub/b.txt", {}, {}, error) == remote::Status::Ok;
                Check(built, "W13: build a small tree");
                Check(DeleteTree(client, tree, true, error) == remote::Status::Ok && !FindEntry(client, dir, L"tree"),
                      "W13: recursive delete removes the whole tree");
            }

            // W14: a link to a folder is removed by a recursive delete, never followed.
            {
                const std::wstring keep = dir + L"/keep";
                const std::wstring linkTree = dir + L"/linktree";
                const std::wstring link = linkTree + L"/lnk";
                client.MakeRemoteDirectory(keep, {}, error);
                client.Upload(work + L"\\small.bin", keep + L"/k.txt", {}, {}, error);
                client.MakeRemoteDirectory(linkTree, {}, error);
                std::wstring linkError;
                // Servers disagree on the SSH_FXP_SYMLINK argument order; the
                // wrong order only targets an existing path and fails harmlessly.
                bool linked = client.MakeRemoteSymlink(keep, link, {}, linkError) == remote::Status::Ok ||
                              client.MakeRemoteSymlink(link, keep, {}, linkError) == remote::Status::Ok;
                remote::RemoteEntry e;
                if (!linked || !FindEntry(client, linkTree, L"lnk", &e)) {
                    std::wprintf(L"  W14: skipped -- the server refused to create a symlink (%s)\n", linkError.c_str());
                } else {
                    Check(e.isLink, "W14: the link is listed as a link");
                    Check(DeleteTree(client, linkTree, true, error) == remote::Status::Ok &&
                              !FindEntry(client, dir, L"linktree"),
                          "W14: recursive delete removes the folder holding the link");
                    Check(FindEntry(client, keep, L"k.txt"), "W14: ...and never deletes what the link points to");
                }
                DeleteTree(client, linkTree, true, error);
                DeleteTree(client, keep, true, error);
            }

            // W15: writing into a read-only folder fails with the server's message.
            std::wstring readOnly = Env(L"MC_SFTP_TEST_READONLY_DIR");
            if (readOnly.empty()) {
                std::printf("  W15: skipped -- set MC_SFTP_TEST_READONLY_DIR to a folder the user can't write\n");
            } else {
                const std::wstring denied = remote::JoinRemotePath(readOnly, L"mc_it_denied.bin");
                std::wstring deniedError;
                remote::Status deniedStatus = client.Upload(work + L"\\small.bin", denied, {}, {}, deniedError);
                Check(deniedStatus == remote::Status::Failed && !deniedError.empty(),
                      "W15: a write into a read-only folder fails");
                std::wprintf(L"  W15: %s\n", deniedError.c_str());
                if (deniedStatus == remote::Status::Ok) client.DeleteRemoteFile(denied, {}, error);
                Check(client.Connected() && FindEntry(client, writable, dir.substr(dir.rfind(L'/') + 1)),
                      "W15: ...and the session stays usable");
            }
            DeleteFileW((work + L"\\small.bin").c_str());

            Check(client.DeleteRemoteFile(dir + L"/renamed.bin", {}, error) == remote::Status::Ok, "delete");
            bool removed = client.RemoveRemoteDirectory(dir, {}, error) == remote::Status::Ok;
            Check(removed, "rmdir");
            if (!removed && DeleteTree(client, dir, true, error) == remote::Status::Ok)
                std::printf("  cleanup: the test folder was not empty; removed it recursively\n");
        }
        client.Disconnect();
    }

    // 2. Known now: no question asked.
    {
        int askedAgain = 0;
        sftp::Client client([&](const sftp::HostKeyInfo&) { ++askedAgain; return sftp::HostKeyDecision::Reject; }, nullptr, files);
        Check(client.Connect(c, secret, {}, error) == remote::Status::Ok && askedAgain == 0, "a remembered host connects silently");
    }

    // 3. A different key on record for this host (in the user's file): refused.
    {
        DeleteFileW(files.own.c_str());
        std::string host = sftp::KnownHostsName(sftp::WideToUtf8(c.host), c.port);
        // A structurally valid key of the same type, with different bytes.
        auto sshString = [](const std::string& s) {
            uint32_t n = static_cast<uint32_t>(s.size());
            return std::string{static_cast<char>(n >> 24), static_cast<char>(n >> 16), static_cast<char>(n >> 8),
                               static_cast<char>(n)} + s;
        };
        std::string type = "ssh-rsa", blob;
        for (const char* curve : {"nistp256", "nistp384", "nistp521"})
            if (seen.keyType.find(sftp::Utf8ToWide(curve)) != std::wstring::npos) type = std::string("ecdsa-sha2-") + curve;
        if (seen.keyType.find(L"ed25519") != std::wstring::npos) type = "ssh-ed25519";
        if (type == "ssh-rsa") blob = sshString(type) + sshString(std::string("\x01\x00\x01", 3)) + sshString(std::string(1, '\0') + std::string(256, 'n'));
        else if (type == "ssh-ed25519") blob = sshString(type) + sshString(std::string(32, 'e'));
        else blob = sshString(type) + sshString(type.substr(11)) + sshString(std::string(1, '\x04') + std::string(64, 'p'));
        std::string otherKey = type + " " + sftp::Base64Encode(reinterpret_cast<const unsigned char*>(blob.data()), blob.size());
        WriteAll(files.user, host + " " + otherKey + "\n");
        sftp::HostKeyInfo changed;
        sftp::Client client([&](const sftp::HostKeyInfo& info) { changed = info; return sftp::HostKeyDecision::AcceptOnce; },
                            nullptr, files);
        remote::Status s = client.Connect(c, secret, {}, error);
        Check(s == remote::Status::Cancelled && changed.verdict == sftp::HostKeyVerdict::Changed && changed.conflictLine == 1,
              "a changed host key is refused even if the prompt says yes (SFTP-007)");
    }

    // The trace never carries the secret (unless the secret is itself one of
    // the login-method words the trace names, like test.rebex.net's "password").
    bool vocabulary = secret == L"password" || secret == L"publickey" || secret == L"keyboard-interactive";
    bool leaked = false;
    for (const auto& t : trace) leaked |= !secret.empty() && !vocabulary && t.find(secret) != std::wstring::npos;
    Check(!leaked, "the password/passphrase never appears in the trace (SFTP-006)");
    for (const auto& t : trace) std::wprintf(L"  trace: %s\n", t.c_str());

    DeleteFileW(files.own.c_str());
    DeleteFileW(files.user.c_str());
    DeleteFileW((work + L"\\download.bin").c_str());
    DeleteFileW((work + L"\\up.bin").c_str());
    DeleteFileW((work + L"\\back.bin").c_str());
    RemoveDirectoryW(work.c_str());
    std::printf("SFTP integration tests%s: %d checks, %d failures\n", writable.empty() ? " (read-only)" : "", checks, failures);
    return failures == 0 ? 0 : 1;
}
