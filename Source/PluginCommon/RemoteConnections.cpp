#include "RemoteConnections.h"

#include <wincred.h>
#include <shlobj.h>

#include <algorithm>
#include <cwctype>
#include <fstream>
#include <sstream>

#pragma comment(lib, "advapi32.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace remote {

namespace {

std::wstring Trim(const std::wstring& s) {
    size_t b = s.find_first_not_of(L" \t\r\n");
    if (b == std::wstring::npos) return L"";
    size_t e = s.find_last_not_of(L" \t\r\n");
    return s.substr(b, e - b + 1);
}

std::wstring Lower(std::wstring s) {
    for (auto& c : s) c = static_cast<wchar_t>(std::towlower(c));
    return s;
}

bool ParseBool(const std::wstring& v, bool& out) {
    std::wstring l = Lower(v);
    if (l == L"yes" || l == L"true" || l == L"on" || l == L"1") { out = true; return true; }
    if (l == L"no" || l == L"false" || l == L"off" || l == L"0") { out = false; return true; }
    return false;
}

bool ParseAuth(const std::wstring& v, AuthMethod& out) {
    std::wstring l = Lower(v);
    if (l == L"password") { out = AuthMethod::Password; return true; }
    if (l == L"key" || l == L"key-file" || l == L"publickey") { out = AuthMethod::KeyFile; return true; }
    if (l == L"agent") { out = AuthMethod::Agent; return true; }
    return false;
}

const wchar_t* AuthName(AuthMethod a) {
    switch (a) {
        case AuthMethod::KeyFile: return L"key";
        case AuthMethod::Agent: return L"agent";
        default: return L"password";
    }
}

std::wstring CredentialTarget(const StoreSpec& spec, const std::wstring& connectionName) {
    return spec.credentialPrefix + connectionName;
}

}  // namespace

Connection NewConnection(const StoreSpec& spec) {
    Connection c;
    c.port = spec.defaultPort;
    c.user = spec.defaultUser;
    return c;
}

std::vector<Connection> ParseConnections(const StoreSpec& spec, const std::wstring& text,
                                         std::vector<std::wstring>& warnings) {
    const std::wstring file = spec.fileName;
    std::vector<Connection> result;
    std::optional<Connection> current;
    size_t lineNumber = 0;
    auto flush = [&]() {
        if (!current) return;
        if (current->host.empty()) {
            warnings.push_back(file + L": [" + current->name + L"] has no host; ignored.");
        } else {
            result.push_back(*current);
        }
        current.reset();
    };
    std::wistringstream in(text);
    std::wstring raw;
    while (std::getline(in, raw)) {
        ++lineNumber;
        std::wstring where = file + L" line " + std::to_wstring(lineNumber) + L": ";
        std::wstring line = Trim(raw);
        if (line.empty() || line[0] == L'#' || line[0] == L';') continue;
        if (line.front() == L'[' && line.back() == L']') {
            flush();
            current = NewConnection(spec);
            current->name = Trim(line.substr(1, line.size() - 2));
            if (current->name.empty()) current->name = L"(unnamed)";
            continue;
        }
        size_t eq = line.find(L'=');
        if (eq == std::wstring::npos || !current) {
            warnings.push_back(where + L"expected \"key = value\" inside a [name] block; ignored.");
            continue;
        }
        std::wstring key = Lower(Trim(line.substr(0, eq)));
        std::wstring value = Trim(line.substr(eq + 1));
        if (key == L"host") current->host = value;
        else if (key == L"user") current->user = value;
        else if (key == L"folder") current->folder = value;
        else if (key == L"port") {
            unsigned long port = 0;
            try { port = std::stoul(value); } catch (...) { port = 0; }
            if (port >= 1 && port <= 65535) current->port = static_cast<uint16_t>(port);
            else warnings.push_back(where + L"port must be 1-65535; using " + std::to_wstring(spec.defaultPort) + L".");
        } else if (key == L"timeout") {
            int t = 0;
            try { t = std::stoi(value); } catch (...) { t = 0; }
            if (t >= 5 && t <= 600) current->timeoutSeconds = t;
            else warnings.push_back(where + L"timeout must be 5-600 seconds; using 20.");
        } else if (key == L"passive" && spec.hasPassive) {
            if (!ParseBool(value, current->passive)) warnings.push_back(where + L"passive must be yes or no.");
        } else if (key == L"auth" && spec.hasAuth) {
            if (!ParseAuth(value, current->auth)) warnings.push_back(where + L"auth must be password, key or agent.");
        } else if (key == L"key-file" && spec.hasAuth) {
            current->keyFile = value;
        } else if (key == L"save-password") {
            if (!ParseBool(value, current->savePassword)) warnings.push_back(where + L"save-password must be yes or no.");
        } else if (key == L"password" || key == L"pass" || key == L"passphrase") {
            // FTP-006 / SFTP-006: never read a secret from this file.
            warnings.push_back(where + L"passwords are never read from this file (use \"Save the password\"); ignored.");
        } else {
            warnings.push_back(where + L"unknown key \"" + key + L"\"; ignored.");
        }
    }
    flush();
    return result;
}

std::wstring FormatConnections(const StoreSpec& spec, const std::vector<Connection>& connections) {
    std::wstring t = spec.header;
    for (const Connection& c : connections) {
        t += L"\n[" + c.name + L"]\n";
        t += L"host = " + c.host + L"\n";
        t += L"port = " + std::to_wstring(c.port) + L"\n";
        t += L"user = " + c.user + L"\n";
        if (!c.folder.empty()) t += L"folder = " + c.folder + L"\n";
        if (spec.hasPassive) t += std::wstring(L"passive = ") + (c.passive ? L"yes" : L"no") + L"\n";
        if (spec.hasAuth) {
            t += std::wstring(L"auth = ") + AuthName(c.auth) + L"\n";
            if (!c.keyFile.empty()) t += L"key-file = " + c.keyFile + L"\n";
        }
        t += L"timeout = " + std::to_wstring(c.timeoutSeconds) + L"\n";
        t += std::wstring(L"save-password = ") + (c.savePassword ? L"yes" : L"no") + L"\n";
    }
    return t;
}

std::wstring ConnectionsFilePath(const StoreSpec& spec) {
    PWSTR appData = nullptr;
    std::wstring path;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &appData)) && appData) {
        path = std::wstring(appData) + L"\\MyCommander\\" + spec.fileName;
    }
    CoTaskMemFree(appData);
    return path;
}

std::vector<Connection> LoadConnections(const StoreSpec& spec, std::vector<std::wstring>& warnings) {
    std::wstring path = ConnectionsFilePath(spec);
    if (path.empty()) return {};
    std::ifstream in(path, std::ios::binary);
    if (!in) return {};
    std::string bytes((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (bytes.size() >= 3 && static_cast<unsigned char>(bytes[0]) == 0xEF && static_cast<unsigned char>(bytes[1]) == 0xBB &&
        static_cast<unsigned char>(bytes[2]) == 0xBF)
        bytes.erase(0, 3);
    int n = MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), nullptr, 0);
    std::wstring text(static_cast<size_t>(std::max(n, 0)), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, bytes.data(), static_cast<int>(bytes.size()), text.data(), n);
    return ParseConnections(spec, text, warnings);
}

bool SaveConnections(const StoreSpec& spec, const std::vector<Connection>& connections) {
    std::wstring path = ConnectionsFilePath(spec);
    if (path.empty()) return false;
    CreateDirectoryW(path.substr(0, path.find_last_of(L'\\')).c_str(), nullptr);
    std::wstring text = FormatConnections(spec, connections);
    int n = WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0, nullptr, nullptr);
    std::string bytes(static_cast<size_t>(std::max(n, 0)), '\0');
    if (n > 0) WideCharToMultiByte(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), bytes.data(), n, nullptr, nullptr);
    // Atomic: write a temporary file next to it, then replace.
    std::wstring temp = path + L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) return false;
        out.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
        if (!out) return false;
    }
    return MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool StorePassword(const StoreSpec& spec, const std::wstring& connectionName, const std::wstring& user,
                   const std::wstring& password) {
    std::wstring target = CredentialTarget(spec, connectionName);
    CREDENTIALW cred{};
    cred.Type = CRED_TYPE_GENERIC;
    cred.TargetName = const_cast<LPWSTR>(target.c_str());
    cred.UserName = const_cast<LPWSTR>(user.c_str());
    cred.CredentialBlobSize = static_cast<DWORD>(password.size() * sizeof(wchar_t));
    cred.CredentialBlob = reinterpret_cast<LPBYTE>(const_cast<wchar_t*>(password.data()));
    cred.Persist = CRED_PERSIST_LOCAL_MACHINE;
    return CredWriteW(&cred, 0) != FALSE;
}

std::optional<std::wstring> ReadPassword(const StoreSpec& spec, const std::wstring& connectionName) {
    PCREDENTIALW cred = nullptr;
    if (!CredReadW(CredentialTarget(spec, connectionName).c_str(), CRED_TYPE_GENERIC, 0, &cred) || !cred)
        return std::nullopt;
    std::wstring password(reinterpret_cast<const wchar_t*>(cred->CredentialBlob), cred->CredentialBlobSize / sizeof(wchar_t));
    SecureZeroMemory(cred->CredentialBlob, cred->CredentialBlobSize);
    CredFree(cred);
    return password;
}

void ForgetPassword(const StoreSpec& spec, const std::wstring& connectionName) {
    CredDeleteW(CredentialTarget(spec, connectionName).c_str(), CRED_TYPE_GENERIC, 0);
}

}  // namespace remote
