#pragma once

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace mc {

// Mirrors the quoting rules used by CommandLineToArgvW / the MSVC CRT (the
// standard "everyone quotes command lines wrong" algorithm), so an argument
// containing spaces, quotes, or backslashes round-trips correctly through
// CreateProcessW/ShellExecuteExW's lpParameters instead of being naively
// wrapped in quotes (SEC-004). Exposed publicly (not just used internally by
// RunAndWait below) so Elevation.h's RunElevated can quote the hidden
// command line it launches itself with the same, single implementation.
std::wstring QuoteCommandLineArgument(const std::wstring& arg);

struct ProcessRunResult {
    bool started = false;
    unsigned long exitCode = 0;
    std::wstring error;
};

// Runs `exePath` (a bare name is resolved via PATH, same as CreateProcess)
// with `args` (unquoted; this function quotes each one per the Win32
// command-line convention, SEC-004) in `workingDirectory`. Does not create a
// new console, so the child inherits this process's console — a
// console-based editor behaves exactly as it would run standalone. Blocks
// until the child exits.
ProcessRunResult RunAndWait(const std::wstring& exePath, const std::vector<std::wstring>& args,
                            const std::filesystem::path& workingDirectory);

struct DetachedProcessResult {
    bool started = false;
    HANDLE processHandle = nullptr;  // caller must CloseHandle once done watching it; nullptr if !started
    std::wstring error;
};

// IS-0002: same argument quoting/working-directory/no-new-console behavior
// as RunAndWait, but returns immediately once the child has started rather
// than waiting for it to exit. `processHandle` becomes signaled
// (WaitForSingleObject) when the child exits — the caller is responsible
// for eventually closing it. Deliberately does not create a new console
// (matching RunAndWait): a GUI-subsystem child (the common case — a text
// editor) never touches the console at all, so nothing needs to be handed
// over; a console-subsystem child sharing this console while the caller
// keeps drawing to it is an accepted, documented limitation of the
// non-blocking launch (see IS-0002's fix plan) rather than something this
// function tries to solve.
DetachedProcessResult RunDetached(const std::wstring& exePath, const std::vector<std::wstring>& args,
                                  const std::filesystem::path& workingDirectory);

} // namespace mc
