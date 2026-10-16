// NAV-017: the recently visited local folders, across both panels, newest
// first. Pure logic (no Console/Dialog, no filesystem access): the list
// operations and the small text file it is saved in. `main.cpp` records the
// panels' locations into it and owns the popup (Alt+F12).
//
// Not NAV-006's per-panel back/forward (linear, per panel) and not NAV-007's
// favorites (named, added by hand): this is "that folder I was in ten minutes
// ago, in either panel".
//
// Stored in its own %APPDATA%\MyCommander\recent-folders.txt, UTF-8, one path
// per line, newest first -- written at the normal exit point like the command
// history (CLI-004). It is navigation history only: nothing in it can be
// replayed as an operation (ERR-006's reasoning), and paths are never logged
// (ERR-005).
#pragma once

#include <cstddef>
#include <filesystem>
#include <string>
#include <vector>

namespace mc {

constexpr size_t kMaxRecentLocations = 30;

class RecentLocations {
public:
    explicit RecentLocations(size_t cap = kMaxRecentLocations) : cap_(cap) {}

    // Puts `folder` at the front; a folder already in the list is moved there
    // (no duplicate). Comparison is lexical: case-insensitive, '/' and '\'
    // alike, a trailing separator ignored (a drive root keeps its one). An
    // empty path is ignored. Trims the list to the cap.
    void Note(const std::filesystem::path& folder);

    // Removes `folder` (same comparison). False if it was not in the list.
    bool Remove(const std::filesystem::path& folder);

    void Clear() { entries_.clear(); }

    // Replaces the list with `newestFirst` (de-duplicated and capped the same
    // way), e.g. from the file.
    void Replace(const std::vector<std::filesystem::path>& newestFirst);

    // Newest first.
    const std::vector<std::filesystem::path>& Entries() const { return entries_; }

private:
    size_t cap_;
    std::vector<std::filesystem::path> entries_;
};

// The lexical comparison key used by the list (exposed for tests).
std::wstring RecentLocationKey(const std::filesystem::path& folder);

// One path per line, newest first. Blank lines and lines starting with '#' are
// ignored; duplicates collapse to the first (newest) occurrence; the result is
// trimmed to `cap`.
std::vector<std::filesystem::path> ParseRecentFile(const std::wstring& text, size_t cap = kMaxRecentLocations);
std::wstring FormatRecentFile(const std::vector<std::filesystem::path>& newestFirst);

std::filesystem::path RecentFoldersFilePath();
// A missing or unreadable file is the expected first run: an empty list.
std::vector<std::filesystem::path> LoadRecentFoldersFrom(const std::filesystem::path& path);
std::vector<std::filesystem::path> LoadRecentFolders();
// Atomic where possible; failure is silent to the caller (a convenience, never
// a reason to interrupt a normal exit).
bool SaveRecentFoldersTo(const std::filesystem::path& path, const std::vector<std::filesystem::path>& newestFirst);
bool SaveRecentFolders(const std::vector<std::filesystem::path>& newestFirst);

}  // namespace mc
