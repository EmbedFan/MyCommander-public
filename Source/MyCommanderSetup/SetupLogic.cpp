#include "SetupLogic.h"

#include "../MyCommander/Process.h"

#include <cstdio>
#include <cwchar>

namespace mc {

std::wstring DefaultInstallPath(const std::wstring& localAppData) {
    std::wstring base = localAppData;
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    return base + L"\\Programs\\MyCommander";
}

bool IsWindowsVersionSupported(DWORD majorVersion, DWORD buildNumber) {
    constexpr DWORD kMinMajorVersion = 10;
    constexpr DWORD kMinBuildNumber = 10240;
    if (majorVersion > kMinMajorVersion) return true;
    return majorVersion == kMinMajorVersion && buildNumber >= kMinBuildNumber;
}

UninstallInfo BuildUninstallInfo(const std::wstring& installPath, const std::wstring& setupExeName,
                                  const std::wstring& version, DWORD estimatedSizeKB) {
    UninstallInfo info;
    info.displayName = L"MyCommander";
    info.displayVersion = version;
    info.publisher = L"Attila Gallai";
    info.installLocation = installPath;

    std::wstring base = installPath;
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    std::wstring copiedSetupPath = base + L"\\" + setupExeName;
    info.uninstallString = QuoteCommandLineArgument(copiedSetupPath) + L" --uninstall";
    info.displayIcon = base + L"\\MyCommander.exe,0";  // IS-0021: path,index -- unquoted is what the shell expects

    info.estimatedSizeKB = estimatedSizeKB;
    return info;
}

std::wstring TerminalFragmentFolder(const std::wstring& localAppData) {
    std::wstring base = localAppData;
    while (!base.empty() && base.back() == L'\\') base.pop_back();
    return base + L"\\Microsoft\\Windows Terminal\\Fragments\\MyCommander";
}

std::wstring TerminalFragmentPath(const std::wstring& localAppData) {
    return TerminalFragmentFolder(localAppData) + L"\\mycommander.json";
}

std::wstring JsonEscape(const std::wstring& text) {
    std::wstring out;
    out.reserve(text.size() + 8);
    for (wchar_t ch : text) {
        switch (ch) {
            case L'"': out += L"\\\""; break;
            case L'\\': out += L"\\\\"; break;
            case L'\n': out += L"\\n"; break;
            case L'\r': out += L"\\r"; break;
            case L'\t': out += L"\\t"; break;
            default:
                if (ch < 0x20) {
                    wchar_t buf[8];
                    swprintf_s(buf, L"\\u%04x", static_cast<unsigned>(ch));
                    out += buf;
                } else {
                    out += ch;
                }
        }
    }
    return out;
}

std::wstring BuildTerminalFragmentJson(const std::wstring& exePath, const std::wstring& iconPath) {
    std::wstring json;
    json += L"{\n";
    json += L"  \"profiles\": [\n";
    json += L"    {\n";
    json += L"      \"name\": \"MyCommander\",\n";
    json += L"      \"commandline\": \"" + JsonEscape(QuoteCommandLineArgument(exePath)) + L"\",\n";
    json += L"      \"icon\": \"" + JsonEscape(iconPath) + L"\"\n";
    json += L"    }\n";
    json += L"  ]\n";
    json += L"}\n";
    return json;
}

std::wstring FormatUpgradeMessage(const std::wstring& installedVersion, const std::wstring& newVersion) {
    return L"MyCommander " + installedVersion + L" is already installed. Install " + newVersion +
           L" over it?";
}

}  // namespace mc
