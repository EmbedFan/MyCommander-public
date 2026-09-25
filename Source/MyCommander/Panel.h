#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace mc {

struct Entry {
    std::wstring name;
    bool isDirectory = false;
    uint64_t sizeBytes = 0;
    bool selected = false;
    uint32_t attributes = 0;         // raw Win32 FILE_ATTRIBUTE_* bits (CON-005/UI-004), 0 if unavailable
    uint64_t lastWriteTimeUtc = 0;   // raw Win32 FILETIME, 100ns ticks since 1601-01-01 UTC (UI-004), 0 if unavailable
    bool inaccessible = false;       // LIST-006: type/size couldn't be read — e.g. a broken reparse point
    bool isReparsePoint = false;     // NAV-009: junction, symbolic link, or another reparse point
};

// UI-003: current volume metadata for a panel. The data is refreshed with
// the listing so it always corresponds to Panel::Path(), rather than only
// being available in the drive picker.
struct VolumeInfo {
    std::wstring root;
    std::wstring label;
    uint32_t driveType = 0;
    uint64_t freeBytes = 0;
    uint64_t totalBytes = 0;
    bool available = false;
    bool spaceKnown = false;
};

// LIST-002: the fields a panel's listing can be sorted by.
enum class SortKey { Name, Extension, Size, ModifiedTime };

// One navigable file-list pane: current directory, its entries, and cursor state.
class Panel {
public:
    explicit Panel(std::filesystem::path startPath, bool groupDirectoriesFirst = true,
                   bool incrementalLoading = false);

    const std::filesystem::path& Path() const { return path_; }
    const std::vector<Entry>& Entries() const { return entries_; }
    int Cursor() const { return cursor_; }
    int TopIndex() const { return topIndex_; }
    const std::wstring& StatusMessage() const { return statusMessage_; }
    const std::wstring& Filter() const { return filter_; }
    SortKey Sort() const { return sortKey_; }
    bool SortDescending() const { return sortDescending_; }
    bool GroupDirectoriesFirst() const { return groupDirectoriesFirst_; }
    const VolumeInfo& Volume() const { return volumeInfo_; }
    bool ShowingHiddenAndSystem() const { return showHiddenAndSystem_; }
    bool IsLoading() const { return loading_; }

    // LIST-006: true when the current path itself couldn't be listed at all
    // (removed drive, unreachable network share, permissions error) — as
    // opposed to a single unreadable entry within an otherwise-fine
    // listing, which is instead reflected per-entry via Entry::inaccessible.
    // Cleared by the next successful Refresh().
    bool LocationInaccessible() const { return locationInaccessible_; }

    void Refresh();
    // LIST-008: enumerate at most `maxEntries` items, returning control to
    // the input loop between batches. Refresh() is synchronous only for
    // non-incremental/test panels.
    void PumpRefresh(size_t maxEntries = 128);
    void MoveCursor(int delta, int visibleRows);
    void EnterSelected();
    void GoToParent();

    // Applies a quick name/wildcard filter to this panel's listing (SRC-003):
    // entries not matching PathUtil.h's WildcardMatch are hidden from
    // Entries(), except "..". An empty pattern shows everything again.
    void SetFilter(std::wstring pattern);

    // NAV-010: Windows hidden and system entries are omitted by default.
    // Each panel can independently reveal them with Ctrl+H.
    void ToggleHiddenAndSystem();

    // NAV-005/LIST-002/LIST-003: sets this panel's own sort key and
    // direction — each Panel instance keeps its own, independent of the
    // other panel's — and re-sorts the current listing in place (no
    // directory re-read), preserving the cursor on the same entry.
    // Directories are always grouped before files regardless of key
    // (LIST-004's existing fixed grouping); ".." always stays first.
    void SetSort(SortKey key, bool descending);

    // LIST-004: controls whether directories form their own leading group
    // for every sort key. ".." remains first in either mode.
    void SetGroupDirectoriesFirst(bool enabled);

    // Jumps directly to `dir` (e.g. from a search result, SRC-002) rather
    // than stepping via EnterSelected/GoToParent. If `selectName` is
    // non-empty, positions the cursor on the entry with that name once the
    // new directory is listed.
    void NavigateTo(const std::filesystem::path& dir, const std::wstring& selectName = L"");

    // NAV-006: each panel owns an independent, in-memory location history.
    // Back/forward navigation changes only the displayed location; it never
    // writes session state, so a crash cannot restore or repeat anything.
    bool NavigateBack();
    bool NavigateForward();
    bool CanNavigateBack() const { return !backHistory_.empty(); }
    bool CanNavigateForward() const { return !forwardHistory_.empty(); }

    // Toggles the cursor entry's selection mark, then advances the cursor.
    void ToggleCursorSelection(int visibleRows);
    void SelectAll();
    void ClearSelection();
    void InvertSelection();

    // SEL-003: sets (select=true) or clears (select=false) the selection
    // mark on every entry whose name matches `pattern` via PathUtil.h's
    // WildcardMatch (glob if the pattern contains '*'/'?', substring
    // otherwise); entries that don't match keep whatever selection state
    // they already had. Always excludes "..".
    void SelectByMask(const std::wstring& pattern, bool select);

    // IS-0004: toggles entry `index`'s selection mark (except "..") and
    // returns its new state -- used by right-click-to-select so the caller
    // can seed a drag-select's target state without a separate read.
    bool ToggleEntrySelected(int index);

    // IS-0004: forces entry `index`'s selection mark to `selected` (except
    // ".."); used by right-drag to extend a selection over every entry the
    // mouse passes over, applying the drag's target state rather than
    // re-toggling on each visit.
    void SetEntrySelected(int index, bool selected);

    // IS-0004: sets every entry whose index falls in the inclusive range
    // [min(indexA, indexB), max(indexA, indexB)] selected (except "..");
    // entries outside the range are left untouched, the same "only touch
    // what's named/ranged, never a wholesale replace" convention
    // SelectByMask above already follows.
    void SelectRange(int indexA, int indexB);

    int SelectedCount() const;
    uint64_t SelectedSizeBytes() const;

    // Names of all selected entries, or just the cursor entry if nothing is
    // selected (SEL-004). Always excludes the ".." parent entry.
    std::vector<std::wstring> SelectionOrCursor() const;

private:
    std::filesystem::path path_;
    std::vector<Entry> entries_;
    int cursor_ = 0;
    int topIndex_ = 0;
    std::wstring statusMessage_;
    std::wstring filter_;
    SortKey sortKey_ = SortKey::Name;
    bool sortDescending_ = false;
    bool groupDirectoriesFirst_ = true;
    bool showHiddenAndSystem_ = false;
    bool incrementalLoading_ = false;
    bool loading_ = false;
    bool locationInaccessible_ = false;
    VolumeInfo volumeInfo_;
    std::vector<std::filesystem::path> backHistory_;
    std::vector<std::filesystem::path> forwardHistory_;
    std::optional<std::filesystem::directory_iterator> refreshIterator_;
    std::wstring refreshCursorName_;
    // IS-0001: a name SetLocation wants selected once the *next* refresh
    // completes, taking priority over refreshCursorName_'s "preserve
    // wherever the cursor already was" default. Consumed (read then
    // cleared) by BeginRefresh()/the synchronous branch of Refresh() —
    // needed because an incremental refresh (LIST-008) doesn't finish
    // populating entries_ until one or more later PumpRefresh() calls, so
    // SetLocation can't just search entries_ for `selectName` itself right
    // after starting the refresh the way it used to.
    std::wstring pendingSelectName_;

    void ClampCursor(int visibleRows);
    void SortEntries();
    void BeginRefresh();
    void FinishRefresh();
    void AddListedEntry(const std::filesystem::directory_entry& item);
    void SetLocation(const std::filesystem::path& dir, const std::wstring& selectName);
};

} // namespace mc
