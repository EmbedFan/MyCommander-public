#include "FileOps.h"

#include "PathUtil.h"
#include "Strings.h"

#include <chrono>
#include <windows.h>
#include <shellapi.h>

#pragma comment(lib, "shell32.lib")

namespace fs = std::filesystem;

namespace mc {

namespace {

// PER-005: CopyFileExW/MoveFileWithProgressW invoke their progress callback
// at a granularity driven by the OS's own internal I/O buffer size, not by
// anything this app controls — on some configurations (e.g. small buffers,
// network shares) that can mean thousands of calls per second for a large
// file. Each call previously did real work unconditionally (a std::function
// invocation into the caller's UI code, plus a PeekConsoleInputW/
// ReadConsoleInputW syscall pair for cancellation polling), so an
// unthrottled callback rate could itself measurably slow down a fast local
// transfer. Capping the *rate* at which this routine does anything (rather
// than relying on each caller to throttle its own onProgress independently)
// bounds that overhead to a small, transfer-duration-proportional constant
// regardless of how often the OS actually calls back — while still firing
// on the very first call (so a UI shows something immediately) and on
// every call once the file is fully transferred (so 100%/cancel state is
// never missed).  50 ms (20 Hz) comfortably clears PER-002's "stays
// responsive" bar for Esc-cancel latency while being far below any
// perceptible UI update rate.
constexpr double kProgressPollIntervalSeconds = 0.05;

std::wstring Widen(const std::string& s) {
    return std::wstring(s.begin(), s.end());
}

// Best-effort: an std::error_code from a std::filesystem call on Windows is
// normally in the system category, whose value() is already the underlying
// Win32 error code — 0 (not offerable for elevation) for anything else.
DWORD Win32ErrorFrom(const std::error_code& ec) {
    return ec.category() == std::system_category() ? static_cast<DWORD>(ec.value()) : 0;
}

// Shared by RenameItem and CreateNewDirectory. Returns an error message if
// `name` cannot be used as a single path segment; empty string if it's fine.
std::wstring ValidateEntryName(const std::wstring& name) {
    if (name.empty() || name == L"." || name == L"..") {
        return kStrInvalidName;
    }
    if (name.find_first_of(L"\\/:*?\"<>|") != std::wstring::npos) {
        return kStrNameReservedCharacter;
    }
    return L"";
}

// --- Copy -------------------------------------------------------------

// Shared by CopyFileExW (copy) and MoveFileWithProgressW (move fast path) —
// both take the same LPPROGRESS_ROUTINE callback shape, so one context/
// routine pair serves the byte-level progress readout (FOP-009) for either.
struct TransferCancelCtx {
    const CancelPoll* poll;
    const ByteProgressHandler* progress;
    bool cancelled = false;
    // PER-005: epoch (default-constructed) means "no call done yet", used
    // below to force the very first call through regardless of the rate cap.
    std::chrono::steady_clock::time_point lastPollTime{};
};

DWORD CALLBACK TransferProgressRoutine(LARGE_INTEGER totalFileSize, LARGE_INTEGER totalBytesTransferred, LARGE_INTEGER,
                                       LARGE_INTEGER, DWORD, DWORD, HANDLE, HANDLE, LPVOID lpData) {
    auto* ctx = reinterpret_cast<TransferCancelCtx*>(lpData);

    // PER-005: rate-limit how often this routine does any real work — see
    // kProgressPollIntervalSeconds' comment above for why. Always let the
    // first call and the completing call (transferred >= total) through so
    // callers still see an initial sample and a guaranteed 100% sample.
    auto now = std::chrono::steady_clock::now();
    bool firstCall = ctx->lastPollTime.time_since_epoch().count() == 0;
    bool isFinal = totalBytesTransferred.QuadPart >= totalFileSize.QuadPart;
    bool due = firstCall || isFinal ||
              std::chrono::duration<double>(now - ctx->lastPollTime).count() >= kProgressPollIntervalSeconds;
    if (!due) {
        return PROGRESS_CONTINUE;
    }
    ctx->lastPollTime = now;

    if (ctx->progress && *ctx->progress) {
        (*ctx->progress)(static_cast<uint64_t>(totalBytesTransferred.QuadPart),
                         static_cast<uint64_t>(totalFileSize.QuadPart));
    }
    // Bug fix: `ctx->poll` (the pointer) is always non-null once a caller
    // passes any CancelPoll by address, even a default-constructed empty
    // one (SEC-003's CopyPathElevated/MovePathElevated are the first
    // callers to do so) — without also checking *ctx->poll's own
    // std::function truthiness first, an empty CancelPoll would be called
    // unconditionally here and throw std::bad_function_call, exactly the
    // same emptiness check `ctx->progress && *ctx->progress` above already
    // gets right.
    if (ctx->poll && *ctx->poll && (*ctx->poll)()) {
        ctx->cancelled = true;
        return PROGRESS_CANCEL;
    }
    return PROGRESS_CONTINUE;
}

bool CopySingleFile(const fs::path& src, const fs::path& dst, const CancelPoll& cancelPoll,
                    const ByteProgressHandler& onProgress, bool& cancelledOut, std::wstring& errOut,
                    DWORD& win32ErrorOut) {
    TransferCancelCtx ctx{&cancelPoll, &onProgress};
    BOOL cancelFlag = FALSE;
    BOOL ok = CopyFileExW(ToLongPath(src).c_str(), ToLongPath(dst).c_str(), TransferProgressRoutine, &ctx, &cancelFlag, 0);
    if (ok) return true;
    DWORD lastError = GetLastError();
    if (ctx.cancelled || lastError == ERROR_REQUEST_ABORTED) {
        cancelledOut = true;
        return false;
    }
    win32ErrorOut = lastError;
    // ERR-001: names both sides, not just a bare Win32 message — the item
    // name a caller attaches to this (CopyItems' top-level item) can be far
    // from the exact file that failed once this runs inside a recursive
    // directory copy.
    errOut = L"\"" + src.wstring() + L"\" -> \"" + dst.wstring() + L"\": " + FormatWinError(lastError);
    return false;
}

// FOP-013: CopyFileExW (used for individual files, see CopySingleFile above)
// already applies the source file's timestamps and attributes to the new
// file on its own. A directory has no such built-in equivalent —
// fs::create_directories leaves it with fresh creation/write times and no
// attributes — so once CopyDirectoryRecursive has finished populating `dst`
// it calls this to copy `src`'s own timestamps and copyable attributes
// across. Must run after every child has been created, since writing into a
// directory bumps its last-write time right back to "now".
void CopyDirectoryMetadata(const fs::path& src, const fs::path& dst) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ToLongPath(src).c_str(), GetFileExInfoStandard, &data)) return;

    HANDLE h = CreateFileW(ToLongPath(dst).c_str(), FILE_WRITE_ATTRIBUTES, FILE_SHARE_READ | FILE_SHARE_WRITE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h != INVALID_HANDLE_VALUE) {
        SetFileTime(h, &data.ftCreationTime, &data.ftLastAccessTime, &data.ftLastWriteTime);
        CloseHandle(h);
    }

    // Only the subset SetFileAttributesW actually accepts (FILE_ATTRIBUTE_DIRECTORY and friends
    // aren't settable this way, and aren't meaningful here). A zero result means the source
    // directory carries none of these, which is the common case — leave the fresh destination
    // directory's own (already-fine) attributes alone rather than passing SetFileAttributesW 0,
    // which Windows rejects.
    const DWORD copyable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM |
                           FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED;
    DWORD attrsToApply = data.dwFileAttributes & copyable;
    if (attrsToApply != 0) {
        SetFileAttributesW(ToLongPath(dst).c_str(), attrsToApply);
    }
}

// Recursively copies `src` into `dst`, refusing to descend into reparse
// points (SEC-006) so a junction/symlink is never expanded during copy.
bool CopyDirectoryRecursive(const fs::path& src, const fs::path& dst, const CancelPoll& cancelPoll,
                            const ByteProgressHandler& onProgress, bool& cancelledOut, std::wstring& errOut,
                            DWORD& win32ErrorOut) {
    if (IsReparsePoint(src)) {
        errOut = kStrRefusingReparseCopy;
        return false;
    }
    std::error_code ec;
    fs::create_directories(ToLongPath(dst), ec);
    if (ec) {
        win32ErrorOut = Win32ErrorFrom(ec);
        errOut = FormatCannotCreateDirectory(dst.wstring(), Widen(ec.message()));
        return false;
    }

    fs::directory_iterator it(ToLongPath(src), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        win32ErrorOut = Win32ErrorFrom(ec);
        errOut = FormatCannotListDirectory(src.wstring(), Widen(ec.message()));
        return false;
    }
    for (const auto& item : it) {
        if (cancelPoll && cancelPoll()) {
            cancelledOut = true;
            return false;
        }
        if (IsReparsePoint(item.path())) {
            // Never descend into or copy a nested reparse point (SEC-006). Skipping it must not
            // abort the rest of this directory's copy.
            continue;
        }
        fs::path childDst = dst / item.path().filename();
        bool isDir = fs::is_directory(item.path(), ec);
        bool ok = isDir ? CopyDirectoryRecursive(item.path(), childDst, cancelPoll, onProgress, cancelledOut, errOut,
                                                 win32ErrorOut)
                        : CopySingleFile(item.path(), childDst, cancelPoll, onProgress, cancelledOut, errOut,
                                         win32ErrorOut);
        if (!ok) return false;
    }
    CopyDirectoryMetadata(src, dst);
    return true;
}

// --- Safety guards shared by copy and move -----------------------------

// Returns an error message if `src` cannot be placed at `dst` (type
// mismatch on overwrite, or a directory being copied/moved into itself);
// empty string if the operation may proceed.
std::wstring CheckDestinationSafety(const fs::path& src, const fs::path& destDir, bool srcIsDir, bool destExists,
                                    bool destIsDir) {
    if (srcIsDir && IsSameOrWithin(src, destDir)) {
        return kStrCannotPlaceDirectoryInsideItself;
    }
    if (destExists && srcIsDir != destIsDir) {
        return srcIsDir ? kStrCannotOverwriteFileWithDirectory : kStrCannotOverwriteDirectoryWithFile;
    }
    return L"";
}

// --- Retry (FOP-010) / elevation (SEC-003) ------------------------------

enum class RetryOutcome { Succeeded, Skipped, Cancelled, FailedNoHandler };

// Runs `attempt` (which sets `err`/`cancelled`/`win32Error` on failure) in a
// loop. Success returns immediately; a user cancellation mid-attempt (Esc
// during a long copy, unrelated to any dialog) returns Cancelled without
// consulting `onError` at all, matching how a conflict's own Cancel choice
// already stops the whole operation without recording a failure. Otherwise
// — a genuine error, not a collision (already resolved by ConflictHandler
// before this is ever called) — `onError` is consulted (or, if none was
// given, the failure is recorded immediately and reported as
// FailedNoHandler): Retry runs `attempt` again, Skip (or a previous
// "skip all", via `haveErrorBlanket`) records nothing and returns Skipped,
// ElevateAndRetry (SEC-003) runs `elevatedAttempt` once instead of `attempt`
// — success returns Succeeded exactly as a normal retry would, failure
// loops back to asking again via `attempt`'s next normal (unprivileged)
// result rather than immediately re-asking with a stale error — and
// anything else (an explicit Cancel) is treated the same as a mid-attempt
// cancellation.
RetryOutcome RunWithRetry(const std::wstring& name, const ErrorHandler& onError, bool& haveErrorBlanket,
                          OperationOutcome& outcome,
                          const std::function<bool(std::wstring&, bool&, DWORD&)>& attempt,
                          const std::function<bool(std::wstring&)>& elevatedAttempt = {}) {
    while (true) {
        std::wstring err;
        bool cancelled = false;
        DWORD win32Error = 0;
        if (attempt(err, cancelled, win32Error)) return RetryOutcome::Succeeded;
        if (cancelled) return RetryOutcome::Cancelled;

        if (!onError) {
            outcome.failed.emplace_back(name, err);
            return RetryOutcome::FailedNoHandler;
        }

        ErrorChoice choice;
        if (haveErrorBlanket) {
            choice = ErrorChoice::Skip;
        } else {
            ErrorAskResult r = onError(name, err, win32Error);
            choice = r.choice;
            if (r.applyToAll) haveErrorBlanket = true;
        }
        if (choice == ErrorChoice::Retry) continue;
        if (choice == ErrorChoice::ElevateAndRetry) {
            if (elevatedAttempt) {
                std::wstring elevatedErr;
                if (elevatedAttempt(elevatedErr)) return RetryOutcome::Succeeded;
            }
            continue;  // ask again with a fresh (still-unprivileged) attempt's own error
        }
        if (choice == ErrorChoice::Skip) {
            ++outcome.skipped;
            return RetryOutcome::Skipped;
        }
        return RetryOutcome::Cancelled;
    }
}

} // namespace

OperationOutcome CopyItems(const fs::path& sourceDir, const std::vector<std::wstring>& items, const fs::path& destDir,
                           const ConflictHandler& onConflict, const ItemStartHandler& onItemStart,
                           const CancelPoll& cancelPoll, const ErrorHandler& onError,
                           const ByteProgressHandler& onByteProgress, const CopyElevationHandler& onElevate) {
    OperationOutcome outcome;
    bool haveBlanket = false;
    ConflictChoice blanket = ConflictChoice::Cancel;
    bool haveErrorBlanket = false;
    int total = static_cast<int>(items.size());

    for (int index = 0; index < total; ++index) {
        const std::wstring& name = items[index];
        if (onItemStart && !onItemStart(name, index, total)) {
            outcome.cancelled = true;
            break;
        }

        fs::path src = sourceDir / name;
        fs::path dst = destDir / name;
        std::error_code ec;
        bool srcIsDir = fs::is_directory(ToLongPath(src), ec);
        bool destExists = fs::exists(ToLongPath(dst), ec);
        bool destIsDir = destExists && fs::is_directory(ToLongPath(dst), ec);

        fs::path finalDst = dst;
        if (destExists) {
            ConflictChoice choice;
            if (haveBlanket) {
                choice = blanket;
            } else {
                ConflictAskResult r = onConflict(dst);
                choice = r.choice;
                if (r.applyToAll) {
                    haveBlanket = true;
                    blanket = choice;
                }
                if (choice == ConflictChoice::Rename) finalDst = destDir / r.renameTo;
            }
            if (choice == ConflictChoice::Cancel) {
                outcome.cancelled = true;
                break;
            }
            if (choice == ConflictChoice::Skip) {
                ++outcome.skipped;
                continue;
            }
            if (choice == ConflictChoice::Rename) {
                destExists = fs::exists(ToLongPath(finalDst), ec);
                destIsDir = destExists && fs::is_directory(ToLongPath(finalDst), ec);
            }
        }

        std::wstring safetyError = CheckDestinationSafety(src, destDir, srcIsDir, destExists, destIsDir);
        if (!safetyError.empty()) {
            outcome.failed.emplace_back(name, safetyError);
            continue;
        }

        std::function<bool(std::wstring&)> elevatedAttempt;
        if (onElevate) {
            elevatedAttempt = [&](std::wstring& err) { return onElevate(src, finalDst, err); };
        }
        RetryOutcome result = RunWithRetry(
            name, onError, haveErrorBlanket, outcome,
            [&](std::wstring& err, bool& cancelled, DWORD& win32Error) {
                return srcIsDir ? CopyDirectoryRecursive(src, finalDst, cancelPoll, onByteProgress, cancelled, err,
                                                         win32Error)
                                : CopySingleFile(src, finalDst, cancelPoll, onByteProgress, cancelled, err, win32Error);
            },
            elevatedAttempt);
        if (result == RetryOutcome::Succeeded) {
            ++outcome.succeeded;
        } else if (result == RetryOutcome::Cancelled) {
            outcome.cancelled = true;
            break;
        }
        // Skipped / FailedNoHandler: outcome already updated inside RunWithRetry; move on to the next item.
    }
    return outcome;
}

OperationOutcome MoveItems(const fs::path& sourceDir, const std::vector<std::wstring>& items, const fs::path& destDir,
                           const ConflictHandler& onConflict, const ItemStartHandler& onItemStart,
                           const CancelPoll& cancelPoll, const ErrorHandler& onError,
                           const ByteProgressHandler& onByteProgress, const MoveElevationHandler& onElevate) {
    OperationOutcome outcome;
    bool haveBlanket = false;
    ConflictChoice blanket = ConflictChoice::Cancel;
    bool haveErrorBlanket = false;
    int total = static_cast<int>(items.size());

    std::error_code sameDirEc;
    bool sameDirectory = fs::equivalent(ToLongPath(sourceDir), ToLongPath(destDir), sameDirEc) && !sameDirEc;

    for (int index = 0; index < total; ++index) {
        const std::wstring& name = items[index];
        if (onItemStart && !onItemStart(name, index, total)) {
            outcome.cancelled = true;
            break;
        }
        if (sameDirectory) {
            outcome.failed.emplace_back(name, kStrSourceAndDestinationSame);
            continue;
        }

        fs::path src = sourceDir / name;
        fs::path dst = destDir / name;
        std::error_code ec;
        bool srcIsDir = fs::is_directory(ToLongPath(src), ec);
        bool destExists = fs::exists(ToLongPath(dst), ec);
        bool destIsDir = destExists && fs::is_directory(ToLongPath(dst), ec);

        fs::path finalDst = dst;
        bool overwrite = false;
        if (destExists) {
            ConflictChoice choice;
            if (haveBlanket) {
                choice = blanket;
            } else {
                ConflictAskResult r = onConflict(dst);
                choice = r.choice;
                if (r.applyToAll) {
                    haveBlanket = true;
                    blanket = choice;
                }
                if (choice == ConflictChoice::Rename) finalDst = destDir / r.renameTo;
            }
            if (choice == ConflictChoice::Cancel) {
                outcome.cancelled = true;
                break;
            }
            if (choice == ConflictChoice::Skip) {
                ++outcome.skipped;
                continue;
            }
            if (choice == ConflictChoice::Overwrite) {
                overwrite = true;
            } else if (choice == ConflictChoice::Rename) {
                destExists = fs::exists(ToLongPath(finalDst), ec);
                destIsDir = destExists && fs::is_directory(ToLongPath(finalDst), ec);
            }
        }

        std::wstring safetyError = CheckDestinationSafety(src, destDir, srcIsDir, destExists, destIsDir);
        if (!safetyError.empty()) {
            outcome.failed.emplace_back(name, safetyError);
            continue;
        }
        if (overwrite && destIsDir) {
            outcome.failed.emplace_back(name, kStrCannotOverwriteDirectoryByMovingIntoIt);
            continue;
        }

        std::function<bool(std::wstring&)> elevatedAttempt;
        if (onElevate) {
            elevatedAttempt = [&](std::wstring& err) { return onElevate(src, finalDst, overwrite, err); };
        }
        RetryOutcome result = RunWithRetry(
            name, onError, haveErrorBlanket, outcome,
            [&](std::wstring& err, bool& cancelled, DWORD& win32Error) {
                DWORD flags = MOVEFILE_COPY_ALLOWED | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
                // MoveFileWithProgressW rather than plain MoveFileExW: same flags and same fast
                // same-volume-rename/cross-volume-file-copy behavior, but also drives
                // TransferProgressRoutine so a cross-volume file move gets the same FOP-009
                // byte-progress/cancel responsiveness a Copy already has (a same-volume rename is
                // still effectively instant, so the callback firing once at completion is harmless).
                TransferCancelCtx ctx{&cancelPoll, &onByteProgress};
                if (MoveFileWithProgressW(ToLongPath(src).c_str(), ToLongPath(finalDst).c_str(), TransferProgressRoutine,
                                          &ctx, flags)) {
                    return true;
                }
                DWORD lastError = GetLastError();
                if (ctx.cancelled || lastError == ERROR_REQUEST_ABORTED) {
                    cancelled = true;
                    return false;
                }

                // MoveFileWithProgress handles cross-volume moves for files via MOVEFILE_COPY_ALLOWED,
                // but not for directories; fall back to a recursive copy plus source removal in that
                // case. A retry here redoes the whole subtree rather than resuming a partial copy —
                // CopySingleFile overwrites by default, so that's safe, just not maximally efficient.
                if (!srcIsDir) {
                    win32Error = lastError;
                    err = L"\"" + src.wstring() + L"\" -> \"" + finalDst.wstring() + L"\": " +
                          FormatWinError(lastError);
                    return false;
                }
                if (!CopyDirectoryRecursive(src, finalDst, cancelPoll, onByteProgress, cancelled, err, win32Error))
                    return false;

                std::error_code delEc;
                fs::remove_all(ToLongPath(src), delEc);
                if (delEc) {
                    win32Error = Win32ErrorFrom(delEc);
                    err = FormatCopiedButCouldNotRemoveSource(finalDst.wstring(), src.wstring(), Widen(delEc.message()));
                    return false;
                }
                return true;
            },
            elevatedAttempt);
        if (result == RetryOutcome::Succeeded) {
            ++outcome.succeeded;
        } else if (result == RetryOutcome::Cancelled) {
            outcome.cancelled = true;
            break;
        }
    }
    return outcome;
}

DestinationState CheckCopyMoveDestination(const fs::path& destDir) {
    std::error_code ec;
    if (!fs::exists(ToLongPath(destDir), ec)) return DestinationState::DoesNotExist;
    if (!fs::is_directory(ToLongPath(destDir), ec)) return DestinationState::ExistsButNotDirectory;
    return DestinationState::ReadyAsDirectory;
}

CreateDestinationResult CreateDestinationDirectory(const fs::path& destDir) {
    CreateDestinationResult result;
    std::error_code ec;
    fs::create_directories(ToLongPath(destDir), ec);
    if (ec) {
        // ERR-001: names the path and the operation, not a bare Win32 message.
        result.error = FormatCannotCreateDirectory(destDir.wstring(), Widen(ec.message()));
        return result;
    }
    result.ok = true;
    return result;
}

RenameResult RenameItem(const fs::path& directory, const std::wstring& oldName, const std::wstring& newName) {
    RenameResult result;
    result.error = ValidateEntryName(newName);
    if (!result.error.empty()) return result;

    fs::path src = directory / oldName;
    fs::path dst = directory / newName;
    std::error_code ec;
    if (oldName != newName && fs::exists(ToLongPath(dst), ec)) {
        result.error = FormatItemAlreadyExists(newName);
        return result;
    }
    if (!MoveFileExW(ToLongPath(src).c_str(), ToLongPath(dst).c_str(), 0)) {
        // ERR-001: names both the item and the operation rather than a bare
        // Win32 message.
        result.error = FormatCannotRename(oldName, newName, FormatWinError(GetLastError()));
        return result;
    }
    result.ok = true;
    return result;
}

CreateDirectoryResult CreateNewDirectory(const fs::path& parentDir, const std::wstring& name) {
    CreateDirectoryResult result;
    result.error = ValidateEntryName(name);
    if (!result.error.empty()) return result;

    fs::path target = parentDir / name;
    std::error_code ec;
    if (fs::exists(ToLongPath(target), ec)) {
        result.error = FormatItemAlreadyExists(name);
        return result;
    }
    fs::create_directory(ToLongPath(target), ec);
    if (ec) {
        // ERR-001: names the path being created, not just a bare Win32 message.
        result.error = FormatCannotCreate(target.wstring(), Widen(ec.message()));
        return result;
    }
    result.ok = true;
    return result;
}

CreateFileResult CreateNewFile(const fs::path& parentDir, const std::wstring& name) {
    CreateFileResult result;
    result.error = ValidateEntryName(name);
    if (!result.error.empty()) return result;

    fs::path target = parentDir / name;
    // CREATE_NEW both creates the file and atomically refuses to overwrite
    // an existing entry of the same name (no separate exists-check/create
    // race the way CreateNewDirectory's fs::exists()-then-create_directory
    // has).
    HANDLE h = CreateFileW(ToLongPath(target).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD err = GetLastError();
        if (err == ERROR_FILE_EXISTS || err == ERROR_ALREADY_EXISTS) {
            result.error = FormatItemAlreadyExists(name);
        } else {
            // ERR-001: names the path being created, not just a bare Win32 message.
            result.error = FormatCannotCreate(target.wstring(), FormatWinError(err));
        }
        return result;
    }
    CloseHandle(h);
    result.ok = true;
    return result;
}

namespace {

// Recursively deletes `target` without ever traversing into a reparse
// point's contents: a symlink or junction is removed as a single link
// (SEC-006). Best-effort clears the read-only attribute before deleting so
// ordinary read-only files don't block an explicit delete. PER-002: polls
// `cancelPoll` before descending into each entry of a directory, the same
// responsiveness `CopyDirectoryRecursive` gives a large recursive copy, so
// Esc interrupts a large permanent delete mid-tree rather than only
// between top-level selected items.
bool RemoveTreeSafely(const fs::path& target, const CancelPoll& cancelPoll, bool& cancelledOut,
                      std::wstring& errOut, DWORD& win32ErrorOut) {
    fs::path longTarget = ToLongPath(target);
    std::error_code ec;

    // ERR-001: every errOut assignment below names `target` — the exact
    // item that failed — not just a bare Win32 message. This recurses, so
    // without that, a deep nested-file failure would only ever be
    // attributed to the top-level item DeleteItems started from.
    if (IsReparsePoint(target)) {
        if (fs::is_directory(longTarget, ec)) {
            if (RemoveDirectoryW(longTarget.c_str())) return true;
        } else if (DeleteFileW(longTarget.c_str())) {
            return true;
        }
        win32ErrorOut = GetLastError();
        errOut = FormatCannotDelete(target.wstring(), FormatWinError(win32ErrorOut));
        return false;
    }

    if (fs::is_directory(longTarget, ec)) {
        fs::directory_iterator it(longTarget, fs::directory_options::skip_permission_denied, ec);
        if (!ec) {
            for (const auto& item : it) {
                if (cancelPoll && cancelPoll()) {
                    cancelledOut = true;
                    return false;
                }
                if (!RemoveTreeSafely(item.path(), cancelPoll, cancelledOut, errOut, win32ErrorOut)) return false;
            }
        }
        if (RemoveDirectoryW(longTarget.c_str())) return true;
        SetFileAttributesW(longTarget.c_str(), FILE_ATTRIBUTE_NORMAL);
        if (RemoveDirectoryW(longTarget.c_str())) return true;
        win32ErrorOut = GetLastError();
        errOut = FormatCannotDelete(target.wstring(), FormatWinError(win32ErrorOut));
        return false;
    }

    SetFileAttributesW(longTarget.c_str(), FILE_ATTRIBUTE_NORMAL);
    if (DeleteFileW(longTarget.c_str())) return true;
    win32ErrorOut = GetLastError();
    errOut = FormatCannotDelete(target.wstring(), FormatWinError(win32ErrorOut));
    return false;
}

} // namespace

OperationOutcome DeleteItems(const fs::path& directory, const std::vector<std::wstring>& items, bool permanent,
                             const ItemStartHandler& onItemStart, const CancelPoll& cancelPoll,
                             const ErrorHandler& onError, const DeleteElevationHandler& onElevate) {
    OperationOutcome outcome;

    if (!permanent) {
        // SHFileOperationW takes a double-null-terminated multi-string of full
        // paths and does not accept the \\?\ long-path prefix.
        std::wstring multiPath;
        for (const auto& name : items) {
            multiPath += (directory / name).wstring();
            multiPath += L'\0';
        }
        multiPath += L'\0';

        SHFILEOPSTRUCTW op{};
        op.wFunc = FO_DELETE;
        op.pFrom = multiPath.c_str();
        op.fFlags = FOF_ALLOWUNDO | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
        int result = SHFileOperationW(&op);
        if (op.fAnyOperationsAborted) {
            outcome.cancelled = true;
        } else if (result == 0) {
            outcome.succeeded = static_cast<int>(items.size());
        } else {
            for (const auto& name : items) {
                outcome.failed.emplace_back(name, FormatRecycleBinDeleteFailed(result));
            }
        }
        return outcome;
    }

    bool haveErrorBlanket = false;
    int total = static_cast<int>(items.size());
    for (int index = 0; index < total; ++index) {
        const std::wstring& name = items[index];
        if (onItemStart && !onItemStart(name, index, total)) {
            outcome.cancelled = true;
            break;
        }
        fs::path target = directory / name;
        std::function<bool(std::wstring&)> elevatedAttempt;
        if (onElevate) {
            elevatedAttempt = [&](std::wstring& err) { return onElevate(target, err); };
        }
        RetryOutcome result = RunWithRetry(
            name, onError, haveErrorBlanket, outcome,
            [&](std::wstring& err, bool& cancelled, DWORD& win32Error) {
                return RemoveTreeSafely(target, cancelPoll, cancelled, err, win32Error);
            },
            elevatedAttempt);
        if (result == RetryOutcome::Succeeded) {
            ++outcome.succeeded;
        } else if (result == RetryOutcome::Cancelled) {
            outcome.cancelled = true;
            break;
        }
    }
    return outcome;
}

namespace {

DWORD AttributeBit(FileAttributeFlag flag) {
    switch (flag) {
        case FileAttributeFlag::ReadOnly:
            return FILE_ATTRIBUTE_READONLY;
        case FileAttributeFlag::Hidden:
            return FILE_ATTRIBUTE_HIDDEN;
        case FileAttributeFlag::System:
            return FILE_ATTRIBUTE_SYSTEM;
        case FileAttributeFlag::Archive:
            return FILE_ATTRIBUTE_ARCHIVE;
    }
    return 0;
}

// Shared by ToggleAttributes' normal (unprivileged) attempt and
// ToggleAttributeElevated below, so both apply the exact same read-flip-
// write logic to a single, already-resolved path.
bool ToggleOneAttribute(const fs::path& longPath, DWORD bit, std::wstring& err, DWORD& win32ErrorOut) {
    DWORD attrs = GetFileAttributesW(longPath.c_str());
    if (attrs == INVALID_FILE_ATTRIBUTES) {
        win32ErrorOut = GetLastError();
        err = FormatWinError(win32ErrorOut);
        return false;
    }

    DWORD newAttrs = (attrs & bit) ? (attrs & ~bit) : (attrs | bit);
    if (newAttrs == 0) newAttrs = FILE_ATTRIBUTE_NORMAL;  // Windows rejects an all-clear attribute set.

    if (!SetFileAttributesW(longPath.c_str(), newAttrs)) {
        win32ErrorOut = GetLastError();
        err = FormatWinError(win32ErrorOut);
        return false;
    }
    return true;
}

} // namespace

OperationOutcome ToggleAttributes(const fs::path& directory, const std::vector<std::wstring>& items,
                                  FileAttributeFlag flag, const ItemStartHandler& onItemStart,
                                  const ErrorHandler& onError, const AttributeElevationHandler& onElevate) {
    OperationOutcome outcome;
    DWORD bit = AttributeBit(flag);
    bool haveErrorBlanket = false;
    int total = static_cast<int>(items.size());

    for (int index = 0; index < total; ++index) {
        const std::wstring& name = items[index];
        if (onItemStart && !onItemStart(name, index, total)) {
            outcome.cancelled = true;
            break;
        }
        fs::path target = directory / name;
        fs::path longPath = ToLongPath(target);
        std::function<bool(std::wstring&)> elevatedAttempt;
        if (onElevate) {
            elevatedAttempt = [&](std::wstring& err) { return onElevate(target, flag, err); };
        }
        RetryOutcome result = RunWithRetry(
            name, onError, haveErrorBlanket, outcome,
            [&](std::wstring& err, bool& /*cancelled*/, DWORD& win32Error) {
                return ToggleOneAttribute(longPath, bit, err, win32Error);
            },
            elevatedAttempt);
        if (result == RetryOutcome::Succeeded) {
            ++outcome.succeeded;
        } else if (result == RetryOutcome::Cancelled) {
            outcome.cancelled = true;
            break;
        }
    }
    return outcome;
}

// --- SEC-003: single already-resolved elevated primitives -----------------

bool CopyPathElevated(const fs::path& src, const fs::path& dst, std::wstring& err, DWORD& win32Error) {
    std::error_code ec;
    bool srcIsDir = fs::is_directory(ToLongPath(src), ec);
    bool cancelled = false;
    CancelPoll noCancel;
    ByteProgressHandler noProgress;
    return srcIsDir ? CopyDirectoryRecursive(src, dst, noCancel, noProgress, cancelled, err, win32Error)
                    : CopySingleFile(src, dst, noCancel, noProgress, cancelled, err, win32Error);
}

bool MovePathElevated(const fs::path& src, const fs::path& dst, bool overwrite, std::wstring& err,
                      DWORD& win32Error) {
    DWORD flags = MOVEFILE_COPY_ALLOWED | (overwrite ? MOVEFILE_REPLACE_EXISTING : 0);
    if (MoveFileExW(ToLongPath(src).c_str(), ToLongPath(dst).c_str(), flags)) return true;
    DWORD lastError = GetLastError();

    // Same cross-volume-directory fallback MoveItems' own attempt uses.
    std::error_code ec;
    if (!fs::is_directory(ToLongPath(src), ec)) {
        win32Error = lastError;
        err = L"\"" + src.wstring() + L"\" -> \"" + dst.wstring() + L"\": " + FormatWinError(lastError);
        return false;
    }
    bool cancelled = false;
    CancelPoll noCancel;
    ByteProgressHandler noProgress;
    if (!CopyDirectoryRecursive(src, dst, noCancel, noProgress, cancelled, err, win32Error)) return false;
    std::error_code delEc;
    fs::remove_all(ToLongPath(src), delEc);
    if (delEc) {
        win32Error = Win32ErrorFrom(delEc);
        err = FormatCopiedButCouldNotRemoveSource(dst.wstring(), src.wstring(), Widen(delEc.message()));
        return false;
    }
    return true;
}

bool DeletePathElevated(const fs::path& target, std::wstring& err, DWORD& win32Error) {
    bool cancelled = false;
    CancelPoll noCancel;
    return RemoveTreeSafely(target, noCancel, cancelled, err, win32Error);
}

bool ToggleAttributeElevated(const fs::path& target, FileAttributeFlag flag, std::wstring& err, DWORD& win32Error) {
    return ToggleOneAttribute(ToLongPath(target), AttributeBit(flag), err, win32Error);
}

} // namespace mc
