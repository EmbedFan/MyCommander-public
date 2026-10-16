#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <filesystem>
#include <string>
#include <vector>

namespace mc {

// NAV-015: Share Folder / Unshare Folder (Network menu, menu-only). Creating
// or removing an SMB share needs administrator rights (SEC-002 forbids
// elevating for *ordinary* operations, but this genuinely isn't one) -- each
// action is one explicit, confirmed, elevated operation through the same
// Elevation.h pattern FileOps.h's own elevated primitives already use.
// Listing a folder's existing shares (NetShareEnum) needs no admin rights at
// all, so main.cpp's Unshare flow never elevates just to show the list.

struct ShareInfo {
    std::wstring shareName;
    std::filesystem::path path;
    std::wstring description;
    bool readOnly = false;
    bool isSpecial = false;  // IS-0022: administrative (C$, ADMIN$, IPC$) -- never marked in a listing
};

// IS-0022: lexical, case-insensitive comparison of a share's recorded path with a
// folder; `\\?\` / `\\?\UNC\` prefixes and trailing separators are ignored. Pure.
bool SharePathMatches(const std::wstring& sharePath, const std::filesystem::path& folder);

// IS-0022: Share Folder and Unshare Folder act on the same folder -- the cursor
// entry when it is a real folder (not ".."), else the panel's own folder. Pure.
std::filesystem::path ShareTargetFolder(const std::filesystem::path& panelPath, const std::wstring& cursorName,
                                        bool cursorIsDirectory);

// IS-0022 / LIST-013: the folders that are the root of an ordinary share, for the
// `>` marker. Built from a share list; never touches the disk.
class SharedFolderSet {
public:
    SharedFolderSet() = default;
    explicit SharedFolderSet(const std::vector<ShareInfo>& shares);
    bool Contains(const std::filesystem::path& folder) const;
    bool Empty() const { return keys_.empty(); }
    size_t Size() const { return keys_.size(); }

private:
    std::vector<std::wstring> keys_;  // sorted, unique
};

// Every share on this machine (one NetShareEnum call, no admin rights needed);
// false when the call failed, so a caller can keep its previous result.
bool EnumerateAllShares(std::vector<ShareInfo>& out);

// Windows share-name rules: 1-80 characters, none of \ / : * ? " < > | or a
// trailing '$' is still allowed (that just hides it from casual browsing) --
// pure, so it's directly unit-testable.
bool IsValidShareName(const std::wstring& name);
// The folder's own name, as Share Folder pre-fills the name field with.
std::wstring DefaultShareName(const std::filesystem::path& folder);

// Every share (of any path) actually pointing at `folder`, via NetShareEnum
// (level 2) -- no admin rights required for this call.
std::vector<ShareInfo> EnumerateSharesFor(const std::filesystem::path& folder);

// The plain, unelevated attempt main.cpp tries first (so a machine with
// looser local policy, or this process somehow already elevated, doesn't
// need a UAC prompt it turns out not to need).
bool AddShareDirect(const std::wstring& shareName, const std::filesystem::path& path, const std::wstring& description,
                    bool readOnly, DWORD& win32Error);
bool DeleteShareDirect(const std::wstring& shareName, DWORD& win32Error);

// Called by the elevated helper child (Elevation.h's RunAsElevatedHelperIfRequested).
bool AddShareElevated(const std::wstring& shareName, const std::filesystem::path& path,
                      const std::wstring& description, bool readOnly, std::wstring& err, DWORD& win32Error);
bool DeleteShareElevated(const std::wstring& shareName, std::wstring& err, DWORD& win32Error);

} // namespace mc
