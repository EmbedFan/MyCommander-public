#pragma once

// EXT-002 (D2): the generic copy/move/delete engine over IFileProvider, used
// whenever either side of an operation isn't the local disk (local <-> local
// stays on FileOps). Streams files in 64 KB chunks with byte progress and
// cancel -- and so FOP-015 pause, which waits inside the cancel poll. Reuses
// FileOps' callback and outcome types, so main.cpp's existing conflict and
// error dialogs drive it unchanged.
//
// Rules:
// - A file collision asks `onConflict` (Overwrite / Skip / Rename / Cancel,
//   with "all"), at any depth. A folder onto an existing folder merges.
// - A write never leaves a partial or destroyed target (IWriteStream).
// - A link is never descended into (SEC-006): copying skips a link to a folder
//   (`onNote`), deleting removes the link itself.
// - A failure asks `onError` (Retry / Skip / Cancel); Retry after
//   ConnectionLost first calls Reconnect on the providers involved.
// - Move = rename when both sides are the same provider object and it can,
//   else copy, then delete the source only if everything under it copied.

#include "FileOps.h"
#include "FileProvider.h"

#include <functional>
#include <string>
#include <vector>

namespace mc {

struct ProviderOpHandlers {
    ConflictHandler onConflict;
    ItemStartHandler onItemStart;
    CancelPoll cancelPoll;
    ErrorHandler onError;
    ByteProgressHandler onByteProgress;
    std::function<void(const std::wstring& message)> onNote;
};

inline constexpr size_t kProviderCopyChunk = 64 * 1024;

OperationOutcome ProviderCopyItems(IFileProvider& source, const std::wstring& sourceDir,
                                   const std::vector<std::wstring>& items, IFileProvider& target,
                                   const std::wstring& targetDir, const ProviderOpHandlers& handlers);

OperationOutcome ProviderMoveItems(IFileProvider& source, const std::wstring& sourceDir,
                                   const std::vector<std::wstring>& items, IFileProvider& target,
                                   const std::wstring& targetDir, const ProviderOpHandlers& handlers);

// FOP-019 (Shift+F5): one item to an explicit name inside `targetDir`.
OperationOutcome ProviderCopyItemAs(IFileProvider& source, const std::wstring& sourceDir, const std::wstring& item,
                                    IFileProvider& target, const std::wstring& targetDir, const std::wstring& newName,
                                    const ProviderOpHandlers& handlers);

// Deletes each item (recursively for folders), children first. A link is
// removed itself, never followed.
OperationOutcome ProviderDeleteItems(IFileProvider& provider, const std::wstring& dir,
                                     const std::vector<std::wstring>& items, const ProviderOpHandlers& handlers);

// FOP-019 / ZIP-011 inside a provider (Shift+F5): what the user typed for
// `itemName` (in `currentDir`) is a new name or a path relative to
// `currentDir` ('/' or '\\'); an existing folder, or a trailing separator,
// means "into it, keeping the name". Surrounding spaces and one pair of
// quotes are trimmed. Never leaves `currentDir`: an empty segment (a leading
// separator), ".", ".." or a drive-like "x:" is refused.
enum class CopyAsProblem { None, Empty, Unsafe, SameItem, IntoItself };
struct ProviderCopyAsTarget {
    std::wstring targetDir;
    std::wstring newName;
    CopyAsProblem problem = CopyAsProblem::None;
};
ProviderCopyAsTarget ResolveProviderCopyAs(IFileProvider& provider, const std::wstring& currentDir,
                                           const std::wstring& itemName, const std::wstring& typed);

// Flips `attributeBit` (one Win32 FILE_ATTRIBUTE_* bit) on each item -- the
// items themselves, not their contents (CON-005's rule). Needs CapAttributes.
OperationOutcome ProviderToggleAttributes(IFileProvider& provider, const std::wstring& dir,
                                          const std::vector<std::wstring>& items, uint32_t attributeBit,
                                          const ProviderOpHandlers& handlers);

}  // namespace mc
