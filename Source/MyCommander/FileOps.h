#pragma once

#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>
#include <windows.h>

namespace mc {

enum class ConflictChoice { Overwrite, Skip, Rename, Cancel };

struct ConflictAskResult {
    ConflictChoice choice = ConflictChoice::Cancel;
    std::wstring renameTo;    // only read when choice == Rename
    bool applyToAll = false;  // apply this choice to every remaining collision in this operation
};

// Asked once per name collision at `destination`, unless a previous answer
// in the same operation set applyToAll (FOP-010).
using ConflictHandler = std::function<ConflictAskResult(const std::filesystem::path& destination)>;

// The Windows file attributes a user can toggle from the UI (CON-005).
// Deliberately excludes FILE_ATTRIBUTE_DIRECTORY and friends, which aren't
// meaningfully user-togglable. Declared up front since SEC-003's elevation
// handler types below need it.
enum class FileAttributeFlag { ReadOnly, Hidden, System, Archive };

// FOP-010: the choices offered when an item fails partway through an
// operation for a reason other than a name collision (already resolved via
// ConflictHandler above) or a user cancellation — access denied, a sharing
// violation because something else has the file open, a full disk, and the
// like, any of which may be transient and worth retrying after the cause
// is fixed. SEC-003: ElevateAndRetry is offered only when the failure was
// specifically access-denied (see main.cpp's AskFileError) and an
// elevation handler was supplied to the operation in progress.
enum class ErrorChoice { Retry, Skip, Cancel, ElevateAndRetry };

struct ErrorAskResult {
    ErrorChoice choice = ErrorChoice::Cancel;
    bool applyToAll = false;  // apply Skip to every remaining failure in this operation without asking again
};

// Asked once per failed item, unless a previous answer set applyToAll.
// Omitted (the default-constructed, empty std::function each of CopyItems/
// MoveItems/DeleteItems below defaults to) preserves the original
// behavior: record the failure in the outcome and move on without asking.
// SEC-003: `win32Error` is the raw Win32 error code behind `errorMessage`
// (0 if not applicable/known), so a caller can decide whether to offer an
// elevated retry — normally only for ERROR_ACCESS_DENIED — without parsing
// `errorMessage`'s free-form, possibly-localized text.
using ErrorHandler =
    std::function<ErrorAskResult(const std::wstring& itemName, const std::wstring& errorMessage, DWORD win32Error)>;

// Called before each top-level item starts. Returning false cancels the
// operation before that item is touched (FOP-009, FOP-012).
using ItemStartHandler = std::function<bool(const std::wstring& currentItem, int doneCount, int totalCount)>;

// Polled frequently while a single large file is copying, so Esc can
// interrupt mid-file without threads. Returning true cancels immediately.
using CancelPoll = std::function<bool()>;

// FOP-009: periodic byte-level progress for the file currently being
// transferred — transferredBytes/totalBytes describe that one file (via
// CopyFileExW/MoveFileWithProgressW's own progress routine), not the whole
// multi-item operation; ItemStartHandler above already reports the current
// item's name and its done/total position among all items. For a directory
// item, this fires once per nested file as CopyDirectoryRecursive works
// through it, so it always reflects whichever single file is actively
// streaming right now.
using ByteProgressHandler = std::function<void(uint64_t transferredBytes, uint64_t totalBytes)>;

// SEC-003: called only when RunWithRetry's `onError` answered a failure
// with ErrorChoice::ElevateAndRetry — must attempt the equivalent operation
// with elevated privileges by whatever means the caller wants (Elevation.h's
// RunElevated, in practice) and report success/failure the same way the
// normal in-process attempt does. FileOps.h/.cpp has no opinion on *how*
// elevation is performed and no dependency on Elevation.h — that
// indirection is what keeps every operation below fully unit-testable
// without ever risking a real UAC prompt from an automated test: a test
// simply never supplies one of these (they all default to empty), so
// ElevateAndRetry is never offered by a test's own onError in the first
// place. Each operation gets its own handler shape matching exactly the
// parameters it would otherwise redo in-process.
using CopyElevationHandler =
    std::function<bool(const std::filesystem::path& src, const std::filesystem::path& dst, std::wstring& err)>;
using MoveElevationHandler = std::function<bool(const std::filesystem::path& src, const std::filesystem::path& dst,
                                                bool overwrite, std::wstring& err)>;
using DeleteElevationHandler = std::function<bool(const std::filesystem::path& target, std::wstring& err)>;
using AttributeElevationHandler =
    std::function<bool(const std::filesystem::path& target, FileAttributeFlag flag, std::wstring& err)>;

struct OperationOutcome {
    int succeeded = 0;
    int skipped = 0;
    bool cancelled = false;
    std::vector<std::pair<std::wstring, std::wstring>> failed;  // (item name, error message)
};

// Copies each of `items` (names within `sourceDir`) into `destDir`. A
// per-item failure that isn't a name collision or a cancellation offers
// Retry/Skip/Cancel via `onError` (FOP-010) if one is given. `onByteProgress`
// (FOP-009), if given, is called periodically while each file is copying.
// `onElevate` (SEC-003), if given, is offered as an extra choice when a
// failure is specifically access-denied.
OperationOutcome CopyItems(const std::filesystem::path& sourceDir, const std::vector<std::wstring>& items,
                           const std::filesystem::path& destDir, const ConflictHandler& onConflict,
                           const ItemStartHandler& onItemStart, const CancelPoll& cancelPoll,
                           const ErrorHandler& onError = ErrorHandler{},
                           const ByteProgressHandler& onByteProgress = ByteProgressHandler{},
                           const CopyElevationHandler& onElevate = CopyElevationHandler{});

// Moves each of `items` into `destDir`. Uses a rename/MoveFileWithProgress
// fast path when possible, falling back to copy-then-delete-source across
// volumes or when merging into an existing directory. See CopyItems for
// `onError`/`onByteProgress`; see CopyItems for `onElevate` (SEC-003).
OperationOutcome MoveItems(const std::filesystem::path& sourceDir, const std::vector<std::wstring>& items,
                           const std::filesystem::path& destDir, const ConflictHandler& onConflict,
                           const ItemStartHandler& onItemStart, const CancelPoll& cancelPoll,
                           const ErrorHandler& onError = ErrorHandler{},
                           const ByteProgressHandler& onByteProgress = ByteProgressHandler{},
                           const MoveElevationHandler& onElevate = MoveElevationHandler{});

// EXT-001/CON-004: main.cpp (Core UI) previously called std::filesystem::
// exists/is_directory/create_directories directly to check and prepare a
// Copy/Move destination — the one place the UI layer reached past FileOps
// into the file system itself, blurring the "core UI" / "file-operation
// services" boundary EXT-001 asks to be defined. These two entry points
// move the actual file-system touches here; the caller still owns every UI
// decision (whether to ask the user, what dialog to show for each outcome)
// via the returned enum/result rather than FileOps making any UI choice
// itself, consistent with how every other operation below only ever
// reports outcomes/errors and never draws anything.
enum class DestinationState { ReadyAsDirectory, DoesNotExist, ExistsButNotDirectory };

// Classifies `destDir` for a pending Copy/Move: already usable, missing
// (the caller may offer to create it), or occupied by something that isn't
// a directory (the caller should refuse).
DestinationState CheckCopyMoveDestination(const std::filesystem::path& destDir);

struct CreateDestinationResult {
    bool ok = false;
    std::wstring error;
};

// Creates `destDir` (and any missing parent directories). Only meaningful
// after CheckCopyMoveDestination returned DoesNotExist and the caller has
// already confirmed creating it with the user.
CreateDestinationResult CreateDestinationDirectory(const std::filesystem::path& destDir);

struct RenameResult {
    bool ok = false;
    std::wstring error;
};

// Renames one entry within its own directory. Refuses to overwrite an
// existing item — rename never silently clobbers.
RenameResult RenameItem(const std::filesystem::path& directory, const std::wstring& oldName,
                        const std::wstring& newName);

struct CreateDirectoryResult {
    bool ok = false;
    std::wstring error;
};

// Creates a new, empty directory named `name` inside `parentDir`. Refuses to
// overwrite an existing entry of the same name (FOP-004).
CreateDirectoryResult CreateNewDirectory(const std::filesystem::path& parentDir, const std::wstring& name);

struct CreateFileResult {
    bool ok = false;
    std::wstring error;
};

// Creates a new, empty file named `name` inside `parentDir` (FOP-005).
// Refuses to overwrite an existing entry of the same name, same as
// CreateNewDirectory.
CreateFileResult CreateNewFile(const std::filesystem::path& parentDir, const std::wstring& name);

// Deletes each of `items` from `directory`. `permanent` bypasses the Recycle
// Bin (FOP-007); the non-permanent path uses SHFileOperationW, which does
// not support the long-path \\?\ prefix, so very long paths may need the
// permanent path instead. `onItemStart`/`cancelPoll`/`onError`/`onElevate`
// (PER-002/FOP-010/SEC-003) only apply to the permanent path, which deletes
// item-by-item; the Recycle Bin path is a single batched shell call with no
// per-item progress or retry point — it's typically fast (moving to the
// Recycle Bin, not erasing content) so the lack of mid-operation
// responsiveness there is much less consequential than for a large
// permanent delete.
OperationOutcome DeleteItems(const std::filesystem::path& directory, const std::vector<std::wstring>& items,
                             bool permanent, const ItemStartHandler& onItemStart = ItemStartHandler{},
                             const CancelPoll& cancelPoll = CancelPoll{}, const ErrorHandler& onError = ErrorHandler{},
                             const DeleteElevationHandler& onElevate = DeleteElevationHandler{});

// Toggles `flag` on each of `items` (names within `directory`): an item that
// currently has the attribute set has it cleared, and vice versa. Never
// leaves an item with zero attributes (Windows requires at least
// FILE_ATTRIBUTE_NORMAL when nothing else is set). Returns the same
// OperationOutcome shape as Copy/Move/Delete (ERR-003: `onError` offers
// Retry/Skip/Skip all/Cancel for a failure, same as those; PER-002:
// `onItemStart` gives the same live-progress-plus-Esc-cancel
// responsiveness on a large selection that Copy/Move/permanent-Delete
// already have — checked once per item, no separate `cancelPoll`, since
// unlike a large file copy or a deep recursive delete, toggling one
// item's attributes is two near-instant Win32 calls with no long
// in-between to poll during). `skipped` requires `onError` to be given,
// since there's no other source of a meaningful "skip" here (no name
// collisions to resolve for an attribute toggle). `onElevate` (SEC-003) as
// above.
OperationOutcome ToggleAttributes(const std::filesystem::path& directory, const std::vector<std::wstring>& items,
                                  FileAttributeFlag flag, const ItemStartHandler& onItemStart = ItemStartHandler{},
                                  const ErrorHandler& onError = ErrorHandler{},
                                  const AttributeElevationHandler& onElevate = AttributeElevationHandler{});

// SEC-003: performs exactly one already-fully-resolved primitive — no
// conflict resolution, no multi-item iteration, no progress callbacks —
// reusing the exact same safety guards (reparse-point refusal, recursion
// guards, FOP-013 metadata copy, etc.) Copy/Move/Delete/ToggleAttributes'
// own internals use. This is what actually runs inside the short-lived
// elevated helper process Elevation.h spawns (its
// RunAsElevatedHelperIfRequested) — never anything more than the one
// operation requested, and never anything at all unless the user explicitly
// chose "Elevate & Retry" on a specific failed item (SEC-001/SEC-002: the
// main, long-running MyCommander process is never itself elevated).
// `win32Error` is set on failure (0 on success) — RunAsElevatedHelperIfRequested
// reports it back to the unprivileged parent as this helper process's exit
// code, so a failure the elevated attempt itself hits for a *new* reason
// (rather than the original access-denied) is still reported precisely
// rather than falling back to a generic code.
bool CopyPathElevated(const std::filesystem::path& src, const std::filesystem::path& dst, std::wstring& err,
                      DWORD& win32Error);
bool MovePathElevated(const std::filesystem::path& src, const std::filesystem::path& dst, bool overwrite,
                      std::wstring& err, DWORD& win32Error);
bool DeletePathElevated(const std::filesystem::path& target, std::wstring& err, DWORD& win32Error);
bool ToggleAttributeElevated(const std::filesystem::path& target, FileAttributeFlag flag, std::wstring& err,
                             DWORD& win32Error);

} // namespace mc
