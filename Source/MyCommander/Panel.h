#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include "FileProvider.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <vector>

namespace mc {

class Console;

// EXT-002: asked before a panel leaves a provider (ZIP: save changes?).
// Returning false keeps the panel where it is.
using ProviderLeaveHook = std::function<bool(IFileProvider& provider)>;

// FEA-0002 (MC Plugin), Phase 3, §8: a small, abstract interface a `Panel`
// gains an optional, non-owning pointer to (PluginManager owns the
// lifetime of whatever implements this, via its own panel-content adapter
// that forwards to a plugin's C exports -- no plugin/DLL dependency leaks
// into Panel.h itself). When set, `main.cpp`'s DrawPanel renders this
// panel's content area (the body below the path/volume header lines --
// which still show the panel's own real path/title, since §8 explicitly
// leaves panel *navigation* untouched) through Draw() instead of the
// panel's own Entries()/cursor, and every key/click landed on this panel
// is offered to OnKey/OnMouseClick instead of the panel's own built-in
// navigation/selection handling.
class IPanelContent {
public:
    virtual ~IPanelContent() = default;
    virtual void Draw(Console& console, SHORT x, SHORT y, SHORT width, SHORT height, bool active) = 0;
    // `unicodeChar` (IS-0018): the key event's typed character (0 if none),
    // for keys whose meaning is a character rather than a physical key --
    // e.g. '+' is Shift+3 on the Hungarian layout. Host-internal only: the
    // plugin adapter ignores it, so the plugin ABI is unchanged.
    virtual void OnKey(WORD virtualKeyCode, DWORD controlKeyState, wchar_t unicodeChar) = 0;
    virtual void OnMouseClick(SHORT relativeX, SHORT relativeY, bool doubleClick) = 0;
    virtual void OnMouseEvent(SHORT x, SHORT y, DWORD buttons, DWORD flags, DWORD) {
        if ((buttons & FROM_LEFT_1ST_BUTTON_PRESSED) && (flags == 0 || flags == DOUBLE_CLICK))
            OnMouseClick(x, y, flags == DOUBLE_CLICK);
    }
};

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
// LIST-011 adds Attributes: grouped by the first set attribute in the order read-only, hidden, system,
// archive, reparse point (see AttributeSortRank), then by name.
enum class SortKey { Name, Extension, Size, ModifiedTime, Attributes };

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
    // UI-017: Full (today's detailed rows) vs. Brief (names only, several
    // columns); a per-panel display preference, independent of sort order,
    // selection, or the quick filter. View > Full/List (Ctrl+F7/Ctrl+F8).
    bool BriefView() const { return briefView_; }
    void SetBriefView(bool brief) { briefView_ = brief; }

    // LIST-006: true when the current path itself couldn't be listed at all
    // (removed drive, unreachable network share, permissions error) — as
    // opposed to a single unreadable entry within an otherwise-fine
    // listing, which is instead reflected per-entry via Entry::inaccessible.
    // Cleared by the next successful Refresh().
    bool LocationInaccessible() const { return locationInaccessible_; }

    void Refresh();
    // LIST-007: an automatic refresh after an external change. Like Refresh(),
    // but keeps the selection (by name; vanished names are dropped), the
    // cursor (by name, else the nearest index) and the scroll position. The
    // quick filter, sort order and hidden/system setting are panel state and
    // are kept by Refresh() itself.
    void RefreshPreservingState();
    // NAV-016: exchanges this panel's whole listing -- folder, back/forward
    // history, entries with their selection, cursor and scroll position, quick
    // filter, sort key and direction, hidden/system and Full/Brief preferences,
    // and an in-progress incremental load -- with `other`'s. The preferences
    // that are the same for both panels (group-directories-first, incremental
    // loading) stay put. Returns false and changes nothing if either panel
    // shows plugin content or a provider (an archive/server): those are tied
    // to a side by the plugin manager and the provider's own hooks.
    bool SwapListingWith(Panel& other);
    // Incremented each time a listing starts; lets a caller tell that a
    // refresh began since it last looked, so change signals older than that
    // listing are already covered by it (LIST-007).
    uint64_t RefreshStarts() const { return refreshStarts_; }
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

    // IS-0017 (preview): shows `dir`'s listing without recording history,
    // so the user's own back/forward trail is untouched by a tree preview.
    void PreviewLocation(const std::filesystem::path& dir);

    // NAV-006: each panel owns an independent, in-memory location history.
    // Back/forward navigation changes only the displayed location; it never
    // writes session state, so a crash cannot restore or repeat anything.
    bool NavigateBack();
    bool NavigateForward();
    bool CanNavigateBack() const { return !backHistory_.empty(); }
    bool CanNavigateForward() const { return !forwardHistory_.empty(); }

    // Toggles the cursor entry's selection mark, then advances the cursor.
    void ToggleCursorSelection(int visibleRows);
    // IS-0015: Shift+Up/Down -- `delta` is +1 (Down) or -1 (Up). Down toggles
    // the entry being left, then moves (same as ToggleCursorSelection above);
    // Up moves first, then toggles the entry just arrived at. This pairing
    // makes the gesture fully reversible: moving one way then immediately
    // back over the same entries toggles each one exactly twice (a net
    // no-op), so shrinking a selection all the way back always clears every
    // entry it covered -- no remembered anchor, no entry that can get stuck
    // permanently selected regardless of how far you shrink back.
    void ToggleSelectionAndMove(int delta, int visibleRows);
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

    // FEA-0002 Phase 3, §8: installs/removes this panel's plugin content
    // (non-owning -- PluginManager owns the adapter's lifetime, and clears
    // this back to nullptr, e.g. at plugin unload, via SetContent(nullptr)
    // rather than this Panel ever reaching back into PluginManager itself).
    // `contentTitle` replaces this panel's own path in its header line
    // while `content` is set; empty/ignored once it's cleared again.
    void SetContent(IPanelContent* content, std::wstring contentTitle = L"") {
        content_ = content;
        contentTitle_ = std::move(contentTitle);
    }
    IPanelContent* Content() const { return content_; }
    const std::wstring& ContentTitle() const { return contentTitle_; }

    // EXT-002 (D2): provider mode. The panel lists `provider` at `startPath`
    // through the same entries/sort/filter/selection/cursor code as a local
    // folder; Path() keeps the local folder underneath, which is where the
    // panel returns on leaving (and all Session ever stores, NAV-008).
    // `returnSelectName` is selected in the local listing on leaving (the
    // archive the user came from). ".." at the provider's root, Back with no
    // provider history, NavigateTo/PreviewLocation of a local folder all
    // leave -- each through `onLeave` first.
    void OpenProvider(std::shared_ptr<IFileProvider> provider, std::wstring startPath,
                      std::wstring returnSelectName = L"", ProviderLeaveHook onLeave = {});
    // Leaves provider mode; unless `force`, only when `onLeave` agrees.
    // Returns whether the panel left.
    bool CloseProvider(bool force = false);
    bool InProvider() const { return provider_ != nullptr; }
    IFileProvider* Provider() const { return provider_.get(); }
    const std::shared_ptr<IFileProvider>& ProviderShared() const { return provider_; }
    const std::wstring& ProviderPath() const { return providerPath_; }
    // Join(ProviderPath(), name) in provider mode, else the local path.
    std::wstring ItemLocation(const std::wstring& name) const;
    // The header text: DisplayRoot() + provider path, or the local path.
    std::wstring LocationText() const;
    // Moves within the provider (records history like NavigateTo).
    void NavigateProvider(const std::wstring& path, const std::wstring& selectName = L"");

private:
    std::shared_ptr<IFileProvider> provider_;
    std::wstring providerPath_;
    std::wstring providerReturnName_;
    ProviderLeaveHook providerLeave_;
    std::vector<std::wstring> providerBack_;
    std::vector<std::wstring> providerForward_;
    void RefreshFromProvider();
    void SetProviderLocation(const std::wstring& path, const std::wstring& selectName);
    bool LeaveProviderFor(bool refreshLocal);

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
    bool briefView_ = false;
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
    // IS-0019: the panel's row count as last passed to MoveCursor/ClampCursor
    // (0 = not known yet), so a refresh that restores the cursor far down a
    // long listing can scroll it into view without main.cpp's help.
    int lastVisibleRows_ = 0;
    // LIST-007: state RefreshPreservingState() asks the next completed listing
    // to restore (see ApplyPreservedState).
    bool preserveState_ = false;
    std::set<std::wstring> keptSelection_;
    int keptCursorIndex_ = 0;
    int keptTopIndex_ = 0;
    uint64_t refreshStarts_ = 0;

    IPanelContent* content_ = nullptr;  // FEA-0002 Phase 3, §8: non-owning, see SetContent
    std::wstring contentTitle_;

    void ClampCursor(int visibleRows);
    void PlaceCursorOn(const std::wstring& name);
    void SortEntries();
    void BeginRefresh();
    void FinishRefresh();
    void ApplyPreservedState(const std::wstring& cursorName);
    void AddListedEntry(const std::filesystem::directory_entry& item);
    void SetLocation(const std::filesystem::path& dir, const std::wstring& selectName);
};

} // namespace mc
