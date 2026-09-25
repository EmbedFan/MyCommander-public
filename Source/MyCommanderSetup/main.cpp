// FEA-0006: MyCommanderSetup, MyCommander's native C++ installer. Console,
// keyboard-only prompts - the same "raw Win32, no GUI toolkit" character as
// the main app (DEC-002) - rather than a separate windowed wizard. See
// AIPrompt/features/FEA-0006-plan.md for the full design; INST-001 through
// INST-006 are the requirement rows this implements.
//
// Deliberately does not depend on Console.h/Dialog.h/Config.h/etc. - this
// is a handful of plain prompts, not a full-screen buffered UI, so pulling
// in the double-buffer renderer would be solving a problem this project
// doesn't have. Reuses PathUtil.h (long-path/error-formatting helpers) and
// Process.h (QuoteCommandLineArgument, for the UninstallString registry
// value) directly, matching MyCommanderTests' own precedent of compiling
// MyCommander's pure modules into a second project.

#include "SetupLogic.h"
#include "resource.h"

#include "../MyCommander/PathUtil.h"
#include "../MyCommander/Process.h"
#include "../MyCommander/Version.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <winternl.h>

#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <filesystem>
#include <iterator>
#include <optional>
#include <string>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "advapi32.lib")

namespace {

namespace fs = std::filesystem;

constexpr wchar_t kUninstallKeyPath[] = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\MyCommander";
constexpr wchar_t kSetupExeName[] = L"MyCommanderSetup.exe";
constexpr wchar_t kShortcutName[] = L"MyCommander.lnk";

// MC_VERSION_STR is a narrow (char*) macro (Version.h's own comment
// explains why it can't be fused into a wide literal); every string this
// project otherwise works in is wide, so widen it once here the same way
// main.cpp's own --version output does (via the %S/narrow-string printf
// specifier), rather than repeating the conversion at each call site.
std::wstring CurrentVersion() {
    wchar_t buf[64];
    swprintf_s(buf, L"%S", MC_VERSION_STR);
    return buf;
}

void Say(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfwprintf(stdout, fmt, args);
    va_end(args);
    fflush(stdout);
}

void SayError(const wchar_t* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfwprintf(stderr, fmt, args);
    va_end(args);
    fflush(stderr);
}

// Reads one line from stdin, trimmed of its trailing newline. Deliberately
// plain fgetws/stdio rather than std::wcin - keeps every bit of this
// project's console I/O on one consistent API (matching main.cpp's own
// wprintf-for---version precedent) instead of risking the classic
// std::wcout-to-a-Windows-console wide-mode pitfall.
std::wstring PromptLine(const wchar_t* prompt) {
    Say(L"%s", prompt);
    wchar_t buf[1024];
    if (!fgetws(buf, static_cast<int>(std::size(buf)), stdin)) return L"";
    std::wstring line(buf);
    while (!line.empty() && (line.back() == L'\n' || line.back() == L'\r')) line.pop_back();
    return line;
}

bool AnsweredYes(const std::wstring& answer) {
    return answer.empty() || answer[0] == L'y' || answer[0] == L'Y';
}

std::wstring GetKnownFolder(REFKNOWNFOLDERID id) {
    PWSTR path = nullptr;
    std::wstring result;
    if (SUCCEEDED(SHGetKnownFolderPath(id, 0, nullptr, &path)) && path) {
        result = path;
    }
    if (path) CoTaskMemFree(path);
    return result;
}

std::wstring RunningExePath() {
    wchar_t buf[MAX_PATH];
    DWORD len = GetModuleFileNameW(nullptr, buf, MAX_PATH);
    return (len > 0 && len < MAX_PATH) ? std::wstring(buf, len) : std::wstring();
}

// The *true* OS version (RtlGetVersion), not GetVersionEx's compatibility-
// shim lie on newer Windows. Loaded via GetProcAddress rather than linking
// ntdll.lib directly - the standard, portable way to reach this specific
// function from ordinary user-mode code.
bool GetRealOSVersion(DWORD& majorVersion, DWORD& buildNumber) {
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (!ntdll) return false;
    using RtlGetVersionFn = LONG(WINAPI*)(PRTL_OSVERSIONINFOW);
    auto rtlGetVersion = reinterpret_cast<RtlGetVersionFn>(GetProcAddress(ntdll, "RtlGetVersion"));
    if (!rtlGetVersion) return false;

    RTL_OSVERSIONINFOW info{};
    info.dwOSVersionInfoSize = sizeof(info);
    if (rtlGetVersion(&info) != 0) return false;
    majorVersion = info.dwMajorVersion;
    buildNumber = info.dwBuildNumber;
    return true;
}

bool ExtractResourceToFile(int resourceId, const fs::path& destPath) {
    HMODULE self = GetModuleHandleW(nullptr);
    HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(resourceId), RT_RCDATA);
    if (!res) return false;
    HGLOBAL data = LoadResource(self, res);
    if (!data) return false;
    DWORD size = SizeofResource(self, res);
    const void* ptr = LockResource(data);
    if (!ptr || size == 0) return false;

    std::error_code ec;
    fs::create_directories(destPath.parent_path(), ec);

    HANDLE file = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    BOOL ok = WriteFile(file, ptr, size, &written, nullptr);
    CloseHandle(file);
    return ok && written == size;
}

bool WriteRegString(HKEY key, const wchar_t* name, const std::wstring& value) {
    DWORD bytes = static_cast<DWORD>((value.size() + 1) * sizeof(wchar_t));
    return RegSetValueExW(key, name, 0, REG_SZ, reinterpret_cast<const BYTE*>(value.c_str()), bytes) ==
           ERROR_SUCCESS;
}

bool WriteRegDword(HKEY key, const wchar_t* name, DWORD value) {
    return RegSetValueExW(key, name, 0, REG_DWORD, reinterpret_cast<const BYTE*>(&value), sizeof(value)) ==
           ERROR_SUCCESS;
}

std::optional<std::wstring> ReadRegString(HKEY key, const wchar_t* name) {
    DWORD type = 0;
    DWORD size = 0;
    if (RegQueryValueExW(key, name, nullptr, &type, nullptr, &size) != ERROR_SUCCESS || type != REG_SZ ||
        size == 0) {
        return std::nullopt;
    }
    std::wstring value(size / sizeof(wchar_t), L'\0');
    if (RegQueryValueExW(key, name, nullptr, &type, reinterpret_cast<BYTE*>(value.data()), &size) !=
        ERROR_SUCCESS) {
        return std::nullopt;
    }
    while (!value.empty() && value.back() == L'\0') value.pop_back();
    return value;
}

DWORD ComputeInstalledSizeKB(const fs::path& dir) {
    std::error_code ec;
    uintmax_t totalBytes = 0;
    for (auto& entry : fs::recursive_directory_iterator(dir, ec)) {
        std::error_code sizeEc;
        if (entry.is_regular_file(sizeEc)) totalBytes += entry.file_size(sizeEc);
    }
    return static_cast<DWORD>(totalBytes / 1024);
}

// FEA-0006 Sec 6: created via IShellLinkW/IPersistFile - the same two COM
// interfaces main.cpp's own drag-and-drop/elevation code already links
// shell32/ole32 for. No app icon exists yet (flagged in the feature plan),
// so the shortcut shows MyCommander.exe's own default icon for now.
bool CreateDesktopShortcut(const fs::path& targetExe, const fs::path& workingDir) {
    std::wstring desktop = GetKnownFolder(FOLDERID_Desktop);
    if (desktop.empty()) return false;
    fs::path linkPath = fs::path(desktop) / kShortcutName;

    bool ok = false;
    IShellLinkW* shellLink = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                    reinterpret_cast<void**>(&shellLink)))) {
        shellLink->SetPath(targetExe.c_str());
        shellLink->SetWorkingDirectory(workingDir.c_str());
        shellLink->SetDescription(L"MyCommander - Terminal File Commander");

        IPersistFile* persistFile = nullptr;
        if (SUCCEEDED(shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile)))) {
            ok = SUCCEEDED(persistFile->Save(linkPath.c_str(), TRUE));
            persistFile->Release();
        }
        shellLink->Release();
    }
    return ok;
}

struct PrereqResult {
    bool ok = true;
    std::wstring reason;
};

// FEA-0006 Sec 3: run first, before touching the filesystem or registry.
PrereqResult CheckPrerequisites(const std::wstring& localAppData) {
    DWORD major = 0;
    DWORD build = 0;
    if (!GetRealOSVersion(major, build) || !mc::IsWindowsVersionSupported(major, build)) {
        return {false, L"MyCommander requires Windows 10 (build 10240) or later."};
    }
    if (localAppData.empty()) {
        return {false, L"Could not determine the local application data folder."};
    }

    ULARGE_INTEGER freeBytes{};
    if (GetDiskFreeSpaceExW(localAppData.c_str(), &freeBytes, nullptr, nullptr)) {
        constexpr ULONGLONG kMinFreeBytes = 50ULL * 1024 * 1024;  // 50 MB - generous; app + docs are a few MB
        if (freeBytes.QuadPart < kMinFreeBytes) {
            return {false, L"Not enough free disk space to install MyCommander."};
        }
    }
    return {true, L""};
}

int RunInstall() {
    std::wstring version = CurrentVersion();
    Say(L"MyCommander Setup %s\n\n", version.c_str());

    std::wstring localAppData = GetKnownFolder(FOLDERID_LocalAppData);
    PrereqResult prereq = CheckPrerequisites(localAppData);
    if (!prereq.ok) {
        SayError(L"Error: %s\n", prereq.reason.c_str());
        return 1;
    }

    // FEA-0006 Sec 3: an existing installation offers Upgrade/Cancel rather
    // than silently doubling up shortcuts/registry entries.
    HKEY existingKey = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKeyPath, 0, KEY_READ, &existingKey) == ERROR_SUCCESS) {
        std::wstring existingVersion = ReadRegString(existingKey, L"DisplayVersion").value_or(L"unknown");
        RegCloseKey(existingKey);
        Say(L"%s\n", mc::FormatUpgradeMessage(existingVersion, version).c_str());
        std::wstring answer = PromptLine(L"Continue? [Y/n]: ");
        if (!AnsweredYes(answer)) {
            Say(L"Cancelled.\n");
            return 1;
        }
    }

    std::wstring defaultInstallPath = mc::DefaultInstallPath(localAppData);
    std::wstring typed = PromptLine((L"Install folder [" + defaultInstallPath + L"]: ").c_str());
    std::wstring installPath = typed.empty() ? defaultInstallPath : typed;

    std::error_code ec;
    fs::create_directories(installPath, ec);
    if (ec) {
        SayError(L"Error: could not create %s: %s\n", installPath.c_str(), mc::ToWideMessage(ec).c_str());
        return 1;
    }

    bool extractedOk = ExtractResourceToFile(IDR_PAYLOAD_MYCOMMANDER, fs::path(installPath) / L"MyCommander.exe");
    extractedOk &= ExtractResourceToFile(IDR_PAYLOAD_USERGUIDE, fs::path(installPath) / L"USERGUIDE.md");
    extractedOk &= ExtractResourceToFile(IDR_PAYLOAD_LICENSE, fs::path(installPath) / L"LICENSE");
    if (!extractedOk) {
        SayError(L"Error: failed to extract application files to %s.\n", installPath.c_str());
        return 1;
    }

    // Reused as the uninstaller (see RunUninstall/BuildUninstallInfo) -
    // Apps & Features' UninstallString must point at something that still
    // exists after the original setup download is gone.
    fs::path copiedSetupPath = fs::path(installPath) / kSetupExeName;
    if (!CopyFileW(RunningExePath().c_str(), copiedSetupPath.c_str(), FALSE)) {
        SayError(L"Warning: could not copy the uninstaller into the install folder.\n");
    }

    std::wstring shortcutAnswer = PromptLine(L"Create a desktop shortcut? [Y/n]: ");
    if (AnsweredYes(shortcutAnswer)) {
        if (!CreateDesktopShortcut(fs::path(installPath) / L"MyCommander.exe", installPath)) {
            SayError(L"Warning: could not create the desktop shortcut.\n");
        }
    }

    mc::UninstallInfo info =
        mc::BuildUninstallInfo(installPath, kSetupExeName, version, ComputeInstalledSizeKB(installPath));

    HKEY key = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &key, nullptr) ==
        ERROR_SUCCESS) {
        WriteRegString(key, L"DisplayName", info.displayName);
        WriteRegString(key, L"DisplayVersion", info.displayVersion);
        WriteRegString(key, L"Publisher", info.publisher);
        WriteRegString(key, L"InstallLocation", info.installLocation);
        WriteRegString(key, L"UninstallString", info.uninstallString);
        WriteRegDword(key, L"EstimatedSize", info.estimatedSizeKB);
        WriteRegDword(key, L"NoModify", 1);
        WriteRegDword(key, L"NoRepair", 1);
        RegCloseKey(key);
    } else {
        SayError(L"Warning: could not register MyCommander with Windows (Apps & Features).\n");
    }

    Say(L"\nMyCommander installed to %s\n", installPath.c_str());
    return 0;
}

// FEA-0006 Sec 7. The running copy of MyCommanderSetup.exe (the one
// Apps & Features launched via UninstallString) is locked while it's
// executing, so its own file - and therefore its containing folder - can't
// be removed synchronously here. This is a per-user, non-elevated install
// (INST-003), so MOVEFILE_DELAY_UNTIL_REBOOT isn't usable either (it needs
// SE_RESTORE_NAME, effectively administrator rights). Instead: delete
// everything else immediately, then hand the final cleanup to a short
// detached helper that waits for this process to exit before removing the
// now-empty-but-for-itself folder - the standard, well-known workaround for
// a non-elevated self-deleting installer.
void ScheduleSelfDelete(const std::wstring& installPath) {
    std::wstring commandLine =
        L"cmd.exe /c ping -n 2 127.0.0.1 >nul & rmdir /s /q " + mc::QuoteCommandLineArgument(installPath);

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, commandLine.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW | DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread);
        CloseHandle(pi.hProcess);
    }
}

int RunUninstall() {
    HKEY key = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKeyPath, 0, KEY_READ, &key) != ERROR_SUCCESS) {
        SayError(L"MyCommander does not appear to be installed.\n");
        return 1;
    }
    std::wstring installPath = ReadRegString(key, L"InstallLocation").value_or(L"");
    RegCloseKey(key);
    if (installPath.empty()) {
        SayError(L"Could not determine the install location.\n");
        return 1;
    }

    std::wstring desktop = GetKnownFolder(FOLDERID_Desktop);
    if (!desktop.empty()) {
        std::error_code ec;
        fs::remove(fs::path(desktop) / kShortcutName, ec);
    }

    RegDeleteTreeW(HKEY_CURRENT_USER, kUninstallKeyPath);

    std::wstring selfPath = RunningExePath();
    std::error_code ec;
    for (auto& entry : fs::directory_iterator(installPath, ec)) {
        if (_wcsicmp(entry.path().c_str(), selfPath.c_str()) == 0) continue;  // this running copy - see below
        std::error_code removeEc;
        fs::remove_all(entry.path(), removeEc);
    }

    Say(L"MyCommander has been uninstalled.\n");
    ScheduleSelfDelete(installPath);
    return 0;
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // needed for CreateDesktopShortcut's IShellLinkW/COM use

    int result = (argc == 2 && std::wcscmp(argv[1], L"--uninstall") == 0) ? RunUninstall() : RunInstall();

    CoUninitialize();
    return result;
}
