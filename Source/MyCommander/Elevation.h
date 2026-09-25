#pragma once

#include "FileOps.h"

#ifndef NOMINMAX
#define NOMINMAX  // avoid windows.h's max()/min() macros clobbering std::max/std::min
#endif
#include <windows.h>

#include <filesystem>
#include <optional>

namespace mc {

// SEC-003: the one operation an elevated helper invocation performs.
enum class ElevatedAction { Copy, Move, DeletePermanent, ToggleAttribute };

struct ElevatedRequest {
    ElevatedAction action;
    std::filesystem::path source;                            // all actions
    std::filesystem::path destination;                       // Copy/Move only
    bool overwrite = false;                                   // Move only
    FileAttributeFlag attributeFlag = FileAttributeFlag::ReadOnly;  // ToggleAttribute only
};

// Encodes `request` as this same executable's own hidden command-line
// arguments (SEC-004: each argument safely quoted via Process.h's
// QuoteCommandLineArgument) — exposed separately from RunElevated so both
// it and RunAsElevatedHelperIfRequested below agree on exactly one format.
std::wstring BuildElevatedHelperParameters(const ElevatedRequest& request);

// SEC-001/SEC-002/SEC-003: relaunches the CURRENT executable as a
// short-lived, hidden, single-purpose elevated child process — via
// ShellExecuteExW's "runas" verb, which is what actually triggers the UAC
// consent prompt — that performs exactly the one operation `request`
// describes and exits. This (calling) process is never itself elevated,
// and the elevated child never does anything beyond that one operation.
// Blocks until the child exits. Returns the Win32 error code the child
// reported via its exit code (ERROR_SUCCESS/0 on success); returns
// ERROR_CANCELLED if the user declined the UAC prompt (nothing is spawned
// in that case) or ShellExecuteExW otherwise failed to launch it.
DWORD RunElevated(const ElevatedRequest& request);

// Called at the very start of wmain, before Console/Config/Session/
// CommandHistory/Log or anything else initializes: if `argv` encodes one of
// BuildElevatedHelperParameters' hidden invocations, performs exactly that
// one operation — no UI, no console output, no other file touched — and
// returns the Win32 result to exit with immediately (ERROR_SUCCESS/0 on
// success). std::nullopt if this isn't an elevated-helper invocation, so
// the normal app continues starting up as usual.
std::optional<DWORD> RunAsElevatedHelperIfRequested(int argc, wchar_t** argv);

} // namespace mc
