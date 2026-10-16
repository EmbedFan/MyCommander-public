#pragma once

// FOP-024: secure delete -- overwrite a file's content (a zero pass and a
// random pass, write-through, flushed) after renaming it to a random name,
// then delete it permanently. Deliberately limited: it cannot reach SSD
// remapped blocks, journals, shadow copies or backups, and says so (the
// confirmation text lives in Strings.h). No Console/Dialog dependency.

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace mc {

enum class SecureVerdict { Allowed, ReadOnlyNeedsClearing, Refused };

struct SecureCheck {
    SecureVerdict verdict = SecureVerdict::Allowed;
    std::wstring reason;  // why Refused
};

// Pure decision on a *file's* Win32 attributes (FILE_ATTRIBUTE_* bits).
SecureCheck CheckSecureDelete(uint32_t attributes);

enum class SecureStatus { Erased, Refused, Cancelled, Failed };

struct SecureFileResult {
    SecureStatus status = SecureStatus::Failed;
    std::wstring message;
};

struct SecureHooks {
    // Polled every 64 KB chunk; true cancels.
    std::function<bool()> cancel;
    // Called after each completed pass (1 = zeros, 2 = random) while the
    // renamed file is still on disk, with its current path. For tests.
    std::function<void(int pass, const std::filesystem::path& currentPath)> afterPass;
};

// One file. Never deletes a file whose overwrite did not finish: the file is
// put back under its original name and reported Cancelled/Failed.
SecureFileResult SecureDeleteFile(const std::filesystem::path& file, const SecureHooks& hooks);

struct SecureTreeOutcome {
    int erased = 0;     // files overwritten and removed
    int links = 0;      // reparse points removed as themselves
    int refused = 0;    // files left in place by CheckSecureDelete
    bool cancelled = false;
    std::vector<std::pair<std::wstring, std::wstring>> problems;  // (path, reason) for refused/failed
};

// A file, or a folder walked recursively (never through a reparse point; a
// reparse point is removed as itself). `onFile` is called before each file
// and may be empty. A folder is removed only if it ends up empty.
SecureTreeOutcome SecureDeleteTree(const std::filesystem::path& root, const SecureHooks& hooks,
                                   const std::function<void(const std::filesystem::path&)>& onFile);

}  // namespace mc
