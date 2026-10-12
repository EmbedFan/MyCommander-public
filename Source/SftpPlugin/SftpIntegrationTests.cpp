// EXT-004: opt-in integration harness for sftp::Client against a REAL SSH
// server (there is no practical in-process SSH server to fake). Skipped --
// exit code 0, "skipped" printed -- unless MC_SFTP_TEST_HOST is set.
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
//
// Host keys go to temporary known_hosts files: the real %APPDATA% and .ssh
// files are never read or written. Build: Source\SftpPlugin\BuildTests.cmd.

#include "SftpClient.h"

#include <cstdio>
#include <cstdlib>
#include <string>

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

}  // namespace

int wmain() {
    remote::Connection c;
    c.host = Env(L"MC_SFTP_TEST_HOST");
    if (c.host.empty()) {
        std::printf("SFTP integration tests: skipped (set MC_SFTP_TEST_HOST, see the file header)\n");
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

        if (!writable.empty()) {
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
            Check(client.DeleteRemoteFile(dir + L"/renamed.bin", {}, error) == remote::Status::Ok, "delete");
            Check(client.RemoveRemoteDirectory(dir, {}, error) == remote::Status::Ok, "rmdir");
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
