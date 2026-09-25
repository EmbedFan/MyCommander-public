#include "Process.h"

#include "PathUtil.h"

#include <windows.h>

namespace mc {

std::wstring QuoteCommandLineArgument(const std::wstring& arg) {
    if (!arg.empty() && arg.find_first_of(L" \t\n\v\"") == std::wstring::npos) {
        return arg;
    }
    std::wstring result = L"\"";
    for (auto it = arg.begin();; ++it) {
        size_t backslashes = 0;
        while (it != arg.end() && *it == L'\\') {
            ++it;
            ++backslashes;
        }
        if (it == arg.end()) {
            result.append(backslashes * 2, L'\\');
            break;
        }
        if (*it == L'"') {
            result.append(backslashes * 2 + 1, L'\\');
            result.push_back(*it);
        } else {
            result.append(backslashes, L'\\');
            result.push_back(*it);
        }
    }
    result.push_back(L'"');
    return result;
}

ProcessRunResult RunAndWait(const std::wstring& exePath, const std::vector<std::wstring>& args,
                            const std::filesystem::path& workingDirectory) {
    ProcessRunResult result;

    std::wstring commandLine = QuoteCommandLineArgument(exePath);
    for (const auto& arg : args) {
        commandLine += L' ';
        commandLine += QuoteCommandLineArgument(arg);
    }

    // CreateProcessW requires a mutable buffer for lpCommandLine.
    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE, 0, nullptr,
                             workingDirectory.c_str(), &si, &pi);
    if (!ok) {
        result.error = FormatWinError(GetLastError());
        return result;
    }

    CloseHandle(pi.hThread);
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD exitCode = 0;
    GetExitCodeProcess(pi.hProcess, &exitCode);
    CloseHandle(pi.hProcess);

    result.started = true;
    result.exitCode = exitCode;
    return result;
}

DetachedProcessResult RunDetached(const std::wstring& exePath, const std::vector<std::wstring>& args,
                                  const std::filesystem::path& workingDirectory) {
    DetachedProcessResult result;

    std::wstring commandLine = QuoteCommandLineArgument(exePath);
    for (const auto& arg : args) {
        commandLine += L' ';
        commandLine += QuoteCommandLineArgument(arg);
    }

    std::vector<wchar_t> mutableCmd(commandLine.begin(), commandLine.end());
    mutableCmd.push_back(L'\0');

    STARTUPINFOW si{};
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi{};

    BOOL ok = CreateProcessW(nullptr, mutableCmd.data(), nullptr, nullptr, /*bInheritHandles=*/TRUE, 0, nullptr,
                             workingDirectory.c_str(), &si, &pi);
    if (!ok) {
        result.error = FormatWinError(GetLastError());
        return result;
    }

    CloseHandle(pi.hThread);  // never waited on, not needed past process creation
    result.started = true;
    result.processHandle = pi.hProcess;
    return result;
}

} // namespace mc
