#pragma once

// NAV-019: show the active folder (and the cursor/first selected entry) in Windows Explorer. The decision
// and the fallback argument string are pure and tested; RevealInExplorer opens a real window and is not
// automated (like Elevation's RunElevated).

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mc {

struct RevealTarget {
    std::filesystem::path folder;
    std::optional<std::wstring> item;  // the entry to select; none = just open the folder
    bool moreThanOne = false;          // several entries were selected; only the first is revealed
};

// `names` are the entries to act on in listing order (the selection, or the cursor entry); ".." and empty
// names are ignored.
RevealTarget RevealTargetFor(const std::filesystem::path& folder, const std::vector<std::wstring>& names);

// The argument string of the fallback `explorer.exe /select,"<path>"`.
std::wstring BuildExplorerSelectArguments(const std::filesystem::path& itemPath);

// Opens Explorer: the item selected in its folder when `item` is set, else the folder. Empty string on
// success, otherwise a message. Never throws.
std::wstring RevealInExplorer(const RevealTarget& target);

}  // namespace mc
