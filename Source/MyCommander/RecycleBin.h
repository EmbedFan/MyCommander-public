#pragma once

// FOP-023: the current user's Recycle Bin as a narrow, testable interface.
// `OpenShellRecycleBin` is the real implementation and the only code in the
// application that touches the shell's Recycle Bin folder (IShellFolder2,
// the "undelete" verb); everything else here is pure and takes plain data, so
// tests (and FOP-022's delete undo) use a fake IRecycleBin. The bin of the
// current user only: no elevation, other users' items are never listed
// (SEC-002). Restoring never overwrites (CheckRestore refuses first).

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mc {

struct RecycleBinItem {
    std::wstring displayName;             // the original name, with extension
    std::filesystem::path originalPath;   // original folder + name
    uint64_t deletedUtc = 0;              // FILETIME ticks (UTC) of the deletion
    uint64_t sizeBytes = 0;               // as the bin reports it (a folder: its contents, where known)
    bool isDirectory = false;
    size_t handle = 0;                    // implementation-defined, valid until the next List()
};

class IRecycleBin {
public:
    virtual ~IRecycleBin() = default;
    // Newest deletion first. On failure returns what it could and sets `error`.
    virtual std::vector<RecycleBinItem> List(std::wstring& error) = 0;
    // Puts one listed item back at its original location. Never overwrites.
    virtual bool Restore(const RecycleBinItem& item, std::wstring& error) = 0;
};

// The real bin. Null (with `error`) when the shell folder can't be opened.
std::unique_ptr<IRecycleBin> OpenShellRecycleBin(std::wstring& error);

struct RestoreCheck {
    bool ok = false;
    std::wstring message;  // why not, in a sentence
};

// Refuses when the original full path already holds something (the lookup is
// injected, so this is pure). A missing parent folder is allowed: the shell
// recreates it, and a failure is reported by Restore.
RestoreCheck CheckRestore(const RecycleBinItem& item, const std::function<bool(const std::filesystem::path&)>& exists);

// "name | original folder | 2026-10-15 13:07 | 12.3 KB" for the list screen.
std::wstring FormatRecycleBinRow(const RecycleBinItem& item);

// Sorts newest deletion first, ties by name.
void SortNewestFirst(std::vector<RecycleBinItem>& items);

// FOP-022: the bin entry a delete recorded earlier. Matches `originalPath`
// (case-insensitive), deleted at or after `notBeforeUtc`; for a file, also the
// recorded size when `matchSize`. The newest such deletion wins. Null if none.
const RecycleBinItem* FindInBin(const std::vector<RecycleBinItem>& items, const std::filesystem::path& originalPath,
                                uint64_t notBeforeUtc, uint64_t sizeBytes, bool matchSize);

}  // namespace mc
