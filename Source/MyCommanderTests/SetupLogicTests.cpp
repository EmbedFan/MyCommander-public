#include "TestFramework.h"

#include "../MyCommanderSetup/SetupLogic.h"

#include <cstdlib>
#include <string>

using mc::BuildUninstallInfo;
using mc::DefaultInstallPath;
using mc::FormatUpgradeMessage;
using mc::IsWindowsVersionSupported;
using mc::UninstallInfo;

TEST_CASE(DefaultInstallPath_AppendsProgramsMyCommander_FEA0006) {
    CHECK(DefaultInstallPath(L"C:\\Users\\alice\\AppData\\Local") ==
          L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander");
}

TEST_CASE(DefaultInstallPath_TrimsTrailingBackslash_FEA0006) {
    CHECK(DefaultInstallPath(L"C:\\Users\\alice\\AppData\\Local\\") ==
          L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander");
}

TEST_CASE(IsWindowsVersionSupported_Windows10RTM_IsSupported_FEA0006) {
    CHECK(IsWindowsVersionSupported(10, 10240));
}

TEST_CASE(IsWindowsVersionSupported_Windows11_IsSupported_FEA0006) {
    // Windows 11 still reports major version 10 through RtlGetVersion; its
    // build numbers (22000+) are what actually distinguish it.
    CHECK(IsWindowsVersionSupported(10, 22000));
}

TEST_CASE(IsWindowsVersionSupported_Windows8Point1_IsNotSupported_FEA0006) {
    CHECK(!IsWindowsVersionSupported(6, 9600));
}

TEST_CASE(IsWindowsVersionSupported_Windows10BelowMinimumBuild_IsNotSupported_FEA0006) {
    CHECK(!IsWindowsVersionSupported(10, 10000));
}

TEST_CASE(IsWindowsVersionSupported_FutureMajorVersion_IsSupported_FEA0006) {
    CHECK(IsWindowsVersionSupported(11, 0));
}

TEST_CASE(BuildUninstallInfo_PopulatesEveryField_FEA0006) {
    UninstallInfo info =
        BuildUninstallInfo(L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander",
                            L"MyCommanderSetup.exe", L"0.1.60.245", 3343);

    CHECK(info.displayName == L"MyCommander");
    CHECK(info.displayVersion == L"0.1.60.245");
    CHECK(info.publisher == L"Attila Gallai");
    CHECK(info.installLocation == L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander");
    CHECK(info.estimatedSizeKB == 3343);
}

TEST_CASE(BuildUninstallInfo_UninstallStringPointsAtCopiedSetupExeWithUninstallFlag_FEA0006) {
    UninstallInfo info =
        BuildUninstallInfo(L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander",
                            L"MyCommanderSetup.exe", L"0.1.60.245", 0);

    // No surrounding quotes: QuoteCommandLineArgument (Process.h) only
    // quotes when actually needed (whitespace/quotes present), matching the
    // CommandLineToArgvW round-trip convention - a space-free path is
    // already unambiguous.
    CHECK(info.uninstallString ==
          L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander\\MyCommanderSetup.exe --uninstall");
}

TEST_CASE(BuildUninstallInfo_QuotesAnInstallPathContainingSpaces_FEA0006) {
    UninstallInfo info = BuildUninstallInfo(L"C:\\Program Files\\My Commander", L"MyCommanderSetup.exe",
                                             L"0.1.60.245", 0);

    CHECK(info.uninstallString == L"\"C:\\Program Files\\My Commander\\MyCommanderSetup.exe\" --uninstall");
}

TEST_CASE(BuildUninstallInfo_TrimsTrailingBackslashOnInstallPath_FEA0006) {
    UninstallInfo info = BuildUninstallInfo(L"C:\\Programs\\MyCommander\\", L"MyCommanderSetup.exe",
                                             L"0.1.60.245", 0);

    CHECK(info.uninstallString == L"C:\\Programs\\MyCommander\\MyCommanderSetup.exe --uninstall");
}

TEST_CASE(FormatUpgradeMessage_MentionsBothVersions_FEA0006) {
    std::wstring message = FormatUpgradeMessage(L"0.1.59.240", L"0.1.60.245");
    CHECK(message.find(L"0.1.59.240") != std::wstring::npos);
    CHECK(message.find(L"0.1.60.245") != std::wstring::npos);
}

// --- IS-0021: the application icon in the installer -------------------------------------------------------------

TEST_CASE(BuildUninstallInfo_DisplayIconPointsAtTheInstalledExecutablesFirstIcon_IS0021) {
    UninstallInfo info = BuildUninstallInfo(L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander",
                                            L"MyCommanderSetup.exe", L"2.2.0", 100);
    CHECK(info.displayIcon == L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander\\MyCommander.exe,0");

    UninstallInfo spaced = BuildUninstallInfo(L"C:\\Program Files\\My Commander\\", L"MyCommanderSetup.exe", L"2.2.0", 1);
    CHECK(spaced.displayIcon == L"C:\\Program Files\\My Commander\\MyCommander.exe,0");  // trailing "\" trimmed, not quoted

    UninstallInfo unicode = BuildUninstallInfo(L"C:\\Users\\\u00c1rv\u00edzt\u0171r\u0151\\MyCommander", L"MyCommanderSetup.exe",
                                               L"2.2.0", 1);
    CHECK(unicode.displayIcon == L"C:\\Users\\\u00c1rv\u00edzt\u0171r\u0151\\MyCommander\\MyCommander.exe,0");
}

TEST_CASE(TerminalFragmentPath_IsUnderTheFragmentsFolderForTheApp_IS0021) {
    CHECK(mc::TerminalFragmentFolder(L"C:\\Users\\alice\\AppData\\Local") ==
          L"C:\\Users\\alice\\AppData\\Local\\Microsoft\\Windows Terminal\\Fragments\\MyCommander");
    CHECK(mc::TerminalFragmentFolder(L"C:\\Users\\alice\\AppData\\Local\\") ==
          L"C:\\Users\\alice\\AppData\\Local\\Microsoft\\Windows Terminal\\Fragments\\MyCommander");  // trailing "\" trimmed
    CHECK(mc::TerminalFragmentPath(L"C:\\Users\\alice\\AppData\\Local") ==
          L"C:\\Users\\alice\\AppData\\Local\\Microsoft\\Windows Terminal\\Fragments\\MyCommander\\mycommander.json");
}

TEST_CASE(JsonEscape_EscapesQuotesBackslashesAndControlCharacters_IS0021) {
    CHECK(mc::JsonEscape(L"plain") == L"plain");
    CHECK(mc::JsonEscape(L"C:\\a\\b") == L"C:\\\\a\\\\b");
    CHECK(mc::JsonEscape(L"say \"hi\"") == L"say \\\"hi\\\"");
    CHECK(mc::JsonEscape(L"a\tb\nc\rd") == L"a\\tb\\nc\\rd");
    CHECK(mc::JsonEscape(std::wstring(1, L'\x01')) == L"\\u0001");
    CHECK(mc::JsonEscape(L"\u00e1rv\u00edzt\u0171r\u0151 \u65e5\u672c") == L"\u00e1rv\u00edzt\u0171r\u0151 \u65e5\u672c");  // kept as is
}

namespace {

// The value of "key": "..." in `json`, JSON-unescaped, so a test can prove the text round-trips exactly.
std::wstring JsonStringValue(const std::wstring& json, const std::wstring& key) {
    const std::wstring marker = L"\"" + key + L"\": \"";
    size_t at = json.find(marker);
    if (at == std::wstring::npos) return L"<missing>";
    std::wstring out;
    for (size_t i = at + marker.size(); i < json.size(); ++i) {
        wchar_t ch = json[i];
        if (ch == L'"') return out;
        if (ch == L'\\' && i + 1 < json.size()) {
            wchar_t next = json[++i];
            switch (next) {
                case L'n': out += L'\n'; break;
                case L'r': out += L'\r'; break;
                case L't': out += L'\t'; break;
                case L'u':
                    out += static_cast<wchar_t>(std::wcstoul(json.substr(i + 1, 4).c_str(), nullptr, 16));
                    i += 4;
                    break;
                default: out += next;
            }
        } else {
            out += ch;
        }
    }
    return L"<unterminated>";
}

}  // namespace

TEST_CASE(BuildTerminalFragmentJson_RoundTripsThePathsAndNamesTheProfile_IS0021) {
    const std::wstring exe = L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander\\MyCommander.exe";
    const std::wstring icon = L"C:\\Users\\alice\\AppData\\Local\\Programs\\MyCommander\\mycommander.ico";
    const std::wstring json = mc::BuildTerminalFragmentJson(exe, icon);

    CHECK(json.find(L"\"profiles\": [") != std::wstring::npos);
    CHECK(JsonStringValue(json, L"name") == L"MyCommander");
    CHECK(JsonStringValue(json, L"commandline") == exe);
    CHECK(JsonStringValue(json, L"icon") == icon);
    CHECK(json.find(L"\\\\Programs\\\\") != std::wstring::npos);  // the backslashes really are escaped in the text
}

TEST_CASE(BuildTerminalFragmentJson_QuotesAnExecutablePathWithSpacesAndKeepsUnicode_IS0021) {
    const std::wstring exe = L"C:\\Program Files\\\u00c1rv\u00edzt\u0171r\u0151 Tools\\MyCommander.exe";
    const std::wstring icon = L"C:\\Program Files\\\u00c1rv\u00edzt\u0171r\u0151 Tools\\mycommander.ico";
    const std::wstring json = mc::BuildTerminalFragmentJson(exe, icon);

    // The command line must keep the path in quotes (it has spaces); the icon path is a plain value.
    CHECK(JsonStringValue(json, L"commandline") == L"\"" + exe + L"\"");
    CHECK(JsonStringValue(json, L"icon") == icon);
    CHECK(json.find(L"\u00c1rv\u00edzt\u0171r\u0151") != std::wstring::npos);  // not turned into \u escapes
}
