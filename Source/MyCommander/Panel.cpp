#include "Panel.h"

#include "PathUtil.h"
#include "Strings.h"

#include <algorithm>
#include <numeric>

namespace fs = std::filesystem;

namespace mc {

namespace {

// LIST-002: the extension used for SortKey::Extension — same "no dot,
// nothing for a directory or dotfile" convention as main.cpp's UI-004
// FormatType, so entries sort the way their displayed type column reads.
std::wstring ExtensionKey(const Entry& e) {
    if (e.isDirectory) return L"";
    size_t dot = e.name.find_last_of(L'.');
    if (dot == std::wstring::npos || dot == 0) return L"";
    return e.name.substr(dot + 1);
}

VolumeInfo QueryVolumeInfo(const fs::path& path) {
    VolumeInfo info;
    std::wstring source = path.wstring();
    if (source.empty()) return info;

    // A panel may be on a long path or a UNC share, so don't use MAX_PATH for
    // the volume-root query. GetVolumePathNameW returns the share root for a
    // UNC location, which is the volume identity the user needs.
    std::vector<wchar_t> root(32768, L'\0');
    if (!GetVolumePathNameW(source.c_str(), root.data(), static_cast<DWORD>(root.size()))) return info;
    info.root = root.data();
    info.driveType = GetDriveTypeW(info.root.c_str());

    std::vector<wchar_t> label(32768, L'\0');
    if (GetVolumeInformationW(info.root.c_str(), label.data(), static_cast<DWORD>(label.size()), nullptr, nullptr,
                              nullptr, nullptr, 0)) {
        info.label = label.data();
    }
    ULARGE_INTEGER freeBytes{}, totalBytes{};
    if (GetDiskFreeSpaceExW(info.root.c_str(), &freeBytes, &totalBytes, nullptr)) {
        info.freeBytes = freeBytes.QuadPart;
        info.totalBytes = totalBytes.QuadPart;
        info.spaceKnown = true;
    }
    info.available = true;
    return info;
}

// LIST-011: the group of an entry in the attribute sort -- the position of the first set attribute in the
// order read-only, hidden, system, archive, reparse point; an entry with none sorts last.
int AttributeSortRank(const Entry& e) {
    const uint32_t a = e.attributes;
    if (a & FILE_ATTRIBUTE_READONLY) return 0;
    if (a & FILE_ATTRIBUTE_HIDDEN) return 1;
    if (a & FILE_ATTRIBUTE_SYSTEM) return 2;
    if (a & FILE_ATTRIBUTE_ARCHIVE) return 3;
    if ((a & FILE_ATTRIBUTE_REPARSE_POINT) || e.isReparsePoint) return 4;
    return 5;
}

// Three-way comparison for SortKey, ignoring direction and the
// directories-first/".." grouping Panel::SortEntries applies around this.
// Ties within a key (e.g. two files of the same size) fall back to name so
// the order is always fully determined, never left to std::sort's
// unspecified tie-breaking.
int CompareByKey(const Entry& a, const Entry& b, SortKey key) {
    switch (key) {
        case SortKey::Extension: {
            int c = _wcsicmp(ExtensionKey(a).c_str(), ExtensionKey(b).c_str());
            if (c != 0) return c;
            break;
        }
        case SortKey::Size:
            if (a.sizeBytes != b.sizeBytes) return a.sizeBytes < b.sizeBytes ? -1 : 1;
            break;
        case SortKey::ModifiedTime:
            if (a.lastWriteTimeUtc != b.lastWriteTimeUtc) return a.lastWriteTimeUtc < b.lastWriteTimeUtc ? -1 : 1;
            break;
        case SortKey::Attributes: {
            const int ra = AttributeSortRank(a), rb = AttributeSortRank(b);
            if (ra != rb) return ra < rb ? -1 : 1;
            break;
        }
        case SortKey::Name:
            break;
    }
    return _wcsicmp(a.name.c_str(), b.name.c_str());
}

}  // namespace

Panel::Panel(fs::path startPath, bool groupDirectoriesFirst, bool incrementalLoading)
    : path_(std::move(startPath)), groupDirectoriesFirst_(groupDirectoriesFirst),
      incrementalLoading_(incrementalLoading) {
    Refresh();
}

void Panel::Refresh() {
    ++refreshStarts_;
    if (provider_) {
        RefreshFromProvider();
        return;
    }
    if (incrementalLoading_) {
        BeginRefresh();
        return;
    }
    // IS-0001: a pending SetLocation selection takes priority over just
    // preserving wherever the cursor already was.
    std::wstring previousCursorName = pendingSelectName_;
    pendingSelectName_.clear();
    if (previousCursorName.empty() && cursor_ >= 0 && cursor_ < static_cast<int>(entries_.size())) {
        previousCursorName = entries_[cursor_].name;
    }

    entries_.clear();
    statusMessage_.clear();
    volumeInfo_ = QueryVolumeInfo(path_);

    if (!IsFilesystemRoot(path_)) {
        entries_.push_back(Entry{L"..", true, 0});
    }

    std::error_code ec;
    fs::directory_iterator it(ToLongPath(path_), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        // LIST-006: the whole location is unavailable (removed drive,
        // unreachable network share, permissions error) — locationInaccessible_
        // lets main.cpp show this clearly in the panel body itself, not just
        // the shared bottom status bar (which only ever reflects whichever
        // panel is currently active).
        statusMessage_ = kStrCannotListDirectory + ToWideMessage(ec);
        locationInaccessible_ = true;
    } else {
        locationInaccessible_ = false;
        for (const auto& item : it) {
            std::error_code typeEc;
            bool isDir = item.is_directory(typeEc);
            uint64_t size = 0;
            std::error_code sizeEc;
            if (!isDir) {
                size = item.file_size(sizeEc);
            }
            // LIST-006: a broken reparse point (junction/symlink whose target
            // no longer exists) fails is_directory()/file_size() — mark it
            // rather than silently showing it as an ordinary 0-byte file.
            bool inaccessible = static_cast<bool>(typeEc) || static_cast<bool>(sizeEc);
            DWORD attrs = GetAttributesOrInvalid(item.path());
            uint64_t mtime = GetLastWriteTimeOrZero(item.path());
            bool hiddenOrSystem = attrs != INVALID_FILE_ATTRIBUTES &&
                                  (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
            if (hiddenOrSystem && !showHiddenAndSystem_) continue;
            entries_.push_back(Entry{item.path().filename().wstring(), isDir, inaccessible ? 0 : size,
                                     /*selected=*/false, attrs == INVALID_FILE_ATTRIBUTES ? 0u : static_cast<uint32_t>(attrs),
                                     mtime, inaccessible,
                                     attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
        }
    }

    if (!filter_.empty()) {
        entries_.erase(std::remove_if(entries_.begin(), entries_.end(),
                                      [&](const Entry& e) { return e.name != L".." && !WildcardMatch(e.name, filter_); }),
                       entries_.end());
    }

    SortEntries();
    PlaceCursorOn(previousCursorName);
    ApplyPreservedState(previousCursorName);
}

// NAV-016: see Panel.h. Every member that describes *what the panel shows* is
// exchanged; a member added to Panel later that belongs to the listing must be
// added here (the swap tests enumerate the observable state).
bool Panel::SwapListingWith(Panel& other) {
    if (this == &other) return true;
    if (content_ != nullptr || other.content_ != nullptr) return false;
    if (InProvider() || other.InProvider()) return false;

    using std::swap;
    swap(path_, other.path_);
    swap(entries_, other.entries_);
    swap(cursor_, other.cursor_);
    swap(topIndex_, other.topIndex_);
    swap(statusMessage_, other.statusMessage_);
    swap(filter_, other.filter_);
    swap(sortKey_, other.sortKey_);
    swap(sortDescending_, other.sortDescending_);
    swap(showHiddenAndSystem_, other.showHiddenAndSystem_);
    swap(briefView_, other.briefView_);
    swap(loading_, other.loading_);
    swap(locationInaccessible_, other.locationInaccessible_);
    swap(volumeInfo_, other.volumeInfo_);
    swap(backHistory_, other.backHistory_);
    swap(forwardHistory_, other.forwardHistory_);
    swap(refreshIterator_, other.refreshIterator_);
    swap(refreshCursorName_, other.refreshCursorName_);
    swap(pendingSelectName_, other.pendingSelectName_);
    swap(lastVisibleRows_, other.lastVisibleRows_);
    swap(preserveState_, other.preserveState_);
    swap(keptSelection_, other.keptSelection_);
    swap(keptCursorIndex_, other.keptCursorIndex_);
    swap(keptTopIndex_, other.keptTopIndex_);
    swap(refreshStarts_, other.refreshStarts_);
    return true;
}

// LIST-007: see Panel.h. Captured here, applied by whichever completion path
// (synchronous Refresh() or FinishRefresh()) ends the next listing.
void Panel::RefreshPreservingState() {
    keptSelection_.clear();
    for (const Entry& e : entries_) {
        if (e.selected && e.name != L"..") keptSelection_.insert(e.name);
    }
    keptCursorIndex_ = cursor_;
    keptTopIndex_ = topIndex_;
    preserveState_ = true;
    Refresh();
}

void Panel::ApplyPreservedState(const std::wstring& cursorName) {
    if (!preserveState_) return;
    preserveState_ = false;
    for (Entry& e : entries_) {
        if (keptSelection_.count(e.name) != 0) e.selected = true;
    }
    keptSelection_.clear();

    const int count = static_cast<int>(entries_.size());
    if (count == 0) {
        cursor_ = 0;
        topIndex_ = 0;
        return;
    }
    bool cursorFound = false;
    if (!cursorName.empty() && cursor_ >= 0 && cursor_ < count) {
        const std::wstring& n = entries_[cursor_].name;
        cursorFound = CompareStringOrdinal(n.c_str(), static_cast<int>(n.size()), cursorName.c_str(),
                                           static_cast<int>(cursorName.size()), TRUE) == CSTR_EQUAL;
    }
    if (!cursorFound) cursor_ = std::clamp(keptCursorIndex_, 0, count - 1);

    topIndex_ = std::clamp(keptTopIndex_, 0, count - 1);
    if (cursor_ < topIndex_) topIndex_ = cursor_;
    if (lastVisibleRows_ > 0 && cursor_ >= topIndex_ + lastVisibleRows_) topIndex_ = cursor_ - lastVisibleRows_ + 1;
}

// IS-0019: puts the cursor on `name` (exact spelling preferred, else a
// case-insensitive match -- NTFS names are case-insensitive, and a typed or
// derived name may differ in case), or on the first entry; then scrolls the
// view so the cursor is visible, centering it when it was below the view.
void Panel::PlaceCursorOn(const std::wstring& name) {
    cursor_ = 0;
    if (!name.empty()) {
        int caseInsensitive = -1;
        for (int i = 0; i < static_cast<int>(entries_.size()); ++i) {
            const std::wstring& n = entries_[i].name;
            if (n == name) {
                caseInsensitive = i;
                break;
            }
            if (caseInsensitive < 0 && CompareStringOrdinal(n.c_str(), static_cast<int>(n.size()), name.c_str(),
                                                            static_cast<int>(name.size()), TRUE) == CSTR_EQUAL) {
                caseInsensitive = i;
            }
        }
        if (caseInsensitive >= 0) cursor_ = caseInsensitive;
    }
    topIndex_ = std::max(0, std::min(topIndex_, cursor_));
    if (lastVisibleRows_ > 0 && cursor_ >= topIndex_ + lastVisibleRows_) {
        topIndex_ = std::max(0, cursor_ - lastVisibleRows_ / 2);
    }
}

void Panel::BeginRefresh() {
    // IS-0001: a pending SetLocation selection takes priority over just
    // preserving wherever the cursor already was — FinishRefresh() applies
    // whichever name ends up in refreshCursorName_ once loading completes.
    refreshCursorName_ = pendingSelectName_;
    pendingSelectName_.clear();
    if (refreshCursorName_.empty() && cursor_ >= 0 && cursor_ < static_cast<int>(entries_.size())) {
        refreshCursorName_ = entries_[cursor_].name;
    }

    entries_.clear();
    statusMessage_.clear();
    volumeInfo_ = QueryVolumeInfo(path_);
    refreshIterator_.reset();
    loading_ = false;
    if (!IsFilesystemRoot(path_)) entries_.push_back(Entry{L"..", true, 0});

    std::error_code ec;
    fs::directory_iterator iterator(ToLongPath(path_), fs::directory_options::skip_permission_denied, ec);
    if (ec) {
        statusMessage_ = kStrCannotListDirectory + ToWideMessage(ec);
        locationInaccessible_ = true;
        FinishRefresh();
        return;
    }

    locationInaccessible_ = false;
    refreshIterator_ = std::move(iterator);
    loading_ = true;
}

void Panel::PumpRefresh(size_t maxEntries) {
    if (!loading_ || !refreshIterator_) return;

    size_t processed = 0;
    while (processed < maxEntries && *refreshIterator_ != fs::directory_iterator{}) {
        AddListedEntry(**refreshIterator_);
        std::error_code ec;
        refreshIterator_->increment(ec);
        if (ec) {
            *refreshIterator_ = fs::directory_iterator{};
            break;
        }
        ++processed;
    }
    if (*refreshIterator_ == fs::directory_iterator{}) FinishRefresh();
}

void Panel::AddListedEntry(const fs::directory_entry& item) {
    std::error_code typeEc;
    bool isDir = item.is_directory(typeEc);
    uint64_t size = 0;
    std::error_code sizeEc;
    if (!isDir) size = item.file_size(sizeEc);
    bool inaccessible = static_cast<bool>(typeEc) || static_cast<bool>(sizeEc);
    DWORD attrs = GetAttributesOrInvalid(item.path());
    uint64_t mtime = GetLastWriteTimeOrZero(item.path());
    bool hiddenOrSystem = attrs != INVALID_FILE_ATTRIBUTES &&
                          (attrs & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
    if (hiddenOrSystem && !showHiddenAndSystem_) return;
    std::wstring name = item.path().filename().wstring();
    if (!filter_.empty() && !WildcardMatch(name, filter_)) return;
    entries_.push_back(Entry{std::move(name), isDir, inaccessible ? 0 : size,
                             /*selected=*/false, attrs == INVALID_FILE_ATTRIBUTES ? 0u : static_cast<uint32_t>(attrs),
                             mtime, inaccessible,
                             attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_REPARSE_POINT) != 0});
}

void Panel::FinishRefresh() {
    refreshIterator_.reset();
    loading_ = false;
    SortEntries();
    PlaceCursorOn(refreshCursorName_);
    ApplyPreservedState(refreshCursorName_);
    refreshCursorName_.clear();
}

void Panel::SortEntries() {
    std::sort(entries_.begin(), entries_.end(), [this](const Entry& a, const Entry& b) {
        if (a.name == L"..") return true;
        if (b.name == L"..") return false;
        if (groupDirectoriesFirst_ && a.isDirectory != b.isDirectory) return a.isDirectory > b.isDirectory;
        int cmp = CompareByKey(a, b, sortKey_);
        return sortDescending_ ? cmp > 0 : cmp < 0;
    });
}

void Panel::SetSort(SortKey key, bool descending) {
    sortKey_ = key;
    sortDescending_ = descending;

    std::wstring previousCursorName;
    if (cursor_ >= 0 && cursor_ < static_cast<int>(entries_.size())) {
        previousCursorName = entries_[cursor_].name;
    }

    SortEntries();

    cursor_ = 0;
    if (!previousCursorName.empty()) {
        auto found = std::find_if(entries_.begin(), entries_.end(),
                                   [&](const Entry& e) { return e.name == previousCursorName; });
        if (found != entries_.end()) {
            cursor_ = static_cast<int>(std::distance(entries_.begin(), found));
        }
    }
    topIndex_ = std::min(topIndex_, cursor_);
    topIndex_ = std::max(topIndex_, 0);
}

void Panel::ClampCursor(int visibleRows) {
    if (visibleRows > 0) lastVisibleRows_ = visibleRows;  // IS-0019
    if (entries_.empty()) {
        cursor_ = 0;
        topIndex_ = 0;
        return;
    }
    cursor_ = std::clamp(cursor_, 0, static_cast<int>(entries_.size()) - 1);
    if (cursor_ < topIndex_) topIndex_ = cursor_;
    if (cursor_ >= topIndex_ + visibleRows) topIndex_ = cursor_ - visibleRows + 1;
    topIndex_ = std::max(0, topIndex_);
}

void Panel::MoveCursor(int delta, int visibleRows) {
    cursor_ += delta;
    ClampCursor(visibleRows);
}

// IS-0015 (corrected): the first version of this used a remembered anchor
// and re-selected the [anchor, cursor] range on every call. That design has
// a permanent bug: the anchor entry (wherever the gesture started) is always
// one of the range's own endpoints, so it's included in *every* non-empty
// range for the gesture's whole lifetime -- there is no sequence of further
// Shift+Up/Down presses that ever deselects it again, in either direction.
// Reported by the user as "last item never deselected even up or down."
//
// Replaced with a plain, symmetric toggle instead -- no anchor, no
// persistent state at all. Down toggles the entry being left, then moves
// (exactly Insert's own existing ToggleCursorSelection); Up moves first,
// then toggles the entry just arrived at. This pairing is what makes the
// gesture fully reversible: moving forward then immediately back over the
// same entries toggles each one exactly twice (a net no-op), so shrinking
// a selection all the way back always clears every entry it covered,
// including the one the gesture started on.
void Panel::ToggleSelectionAndMove(int delta, int visibleRows) {
    if (delta > 0) {
        ToggleCursorSelection(visibleRows);  // toggle current, then move -- unchanged existing behavior
        return;
    }
    if (delta < 0) {
        MoveCursor(delta, visibleRows);
        if (entries_.empty()) return;
        Entry& entry = entries_[cursor_];
        if (entry.name != L"..") entry.selected = !entry.selected;
    }
}

void Panel::EnterSelected() {
    if (entries_.empty()) return;
    const Entry& selected = entries_[cursor_];
    if (!selected.isDirectory) return;

    if (provider_) {
        if (selected.name == L"..") {
            GoToParent();
            return;
        }
        // A link to a folder inside a provider opens (the remote panels'
        // rule); copy and delete still never descend into it (SEC-006).
        NavigateProvider(provider_->Join(providerPath_, selected.name));
        return;
    }

    if (selected.name == L"..") {
        GoToParent();
        return;
    }

    // NAV-009: opening a junction/symlink directory could turn a user
    // repeatedly pressing Enter into a path-growing loop (for example, a
    // link back to an ancestor). It remains visibly listed and can still be
    // acted on as a link, but the panel never traverses into it.
    if (selected.isReparsePoint) {
        statusMessage_ = kStrCannotEnterReparsePoint + selected.name;
        return;
    }

    fs::path target = path_ / selected.name;
    std::error_code ec;
    fs::path checkTarget = ToLongPath(target);
    if (!fs::exists(checkTarget, ec) || !fs::is_directory(checkTarget, ec)) {
        statusMessage_ = kStrCannotEnter + selected.name;
        return;
    }
    NavigateTo(target);
}

void Panel::SetGroupDirectoriesFirst(bool enabled) {
    if (groupDirectoriesFirst_ == enabled) return;
    groupDirectoriesFirst_ = enabled;
    SetSort(sortKey_, sortDescending_);
}

void Panel::GoToParent() {
    if (provider_) {
        if (provider_->IsRoot(providerPath_)) {
            CloseProvider();
            return;
        }
        NavigateProvider(provider_->Parent(providerPath_), ProviderLeafName(*provider_, providerPath_));
        return;
    }
    if (IsFilesystemRoot(path_)) return;
    NavigateTo(path_.parent_path());
}

void Panel::ToggleCursorSelection(int visibleRows) {
    if (entries_.empty()) return;
    Entry& entry = entries_[cursor_];
    if (entry.name != L"..") {
        entry.selected = !entry.selected;
    }
    MoveCursor(1, visibleRows);
}

void Panel::SelectAll() {
    for (auto& entry : entries_) {
        if (entry.name != L"..") entry.selected = true;
    }
}

void Panel::ClearSelection() {
    for (auto& entry : entries_) {
        entry.selected = false;
    }
}

void Panel::InvertSelection() {
    for (auto& entry : entries_) {
        if (entry.name != L"..") entry.selected = !entry.selected;
    }
}

void Panel::SelectByMask(const std::wstring& pattern, bool select) {
    for (auto& entry : entries_) {
        if (entry.name == L"..") continue;
        if (WildcardMatch(entry.name, pattern)) entry.selected = select;
    }
}

bool Panel::ToggleEntrySelected(int index) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return false;
    Entry& entry = entries_[index];
    if (entry.name == L"..") return false;
    entry.selected = !entry.selected;
    return entry.selected;
}

void Panel::SetEntrySelected(int index, bool selected) {
    if (index < 0 || index >= static_cast<int>(entries_.size())) return;
    Entry& entry = entries_[index];
    if (entry.name == L"..") return;
    entry.selected = selected;
}

void Panel::SelectRange(int indexA, int indexB) {
    int from = std::max(0, std::min(indexA, indexB));
    int to = std::min(static_cast<int>(entries_.size()) - 1, std::max(indexA, indexB));
    for (int i = from; i <= to; ++i) {
        if (entries_[i].name != L"..") entries_[i].selected = true;
    }
}

int Panel::SelectedCount() const {
    return static_cast<int>(std::count_if(entries_.begin(), entries_.end(),
                                           [](const Entry& e) { return e.selected; }));
}

uint64_t Panel::SelectedSizeBytes() const {
    return std::accumulate(entries_.begin(), entries_.end(), uint64_t{0},
                           [](uint64_t sum, const Entry& entry) { return entry.selected ? sum + entry.sizeBytes : sum; });
}

void Panel::SetFilter(std::wstring pattern) {
    filter_ = std::move(pattern);
    Refresh();
}

void Panel::ToggleHiddenAndSystem() {
    showHiddenAndSystem_ = !showHiddenAndSystem_;
    Refresh();
}

void Panel::SetLocation(const fs::path& dir, const std::wstring& selectName) {
    // IS-0001: set *before* Refresh(), not searched for afterward — an
    // incremental refresh (LIST-008) hasn't populated entries_ yet by the
    // time Refresh() returns, so searching entries_ here would silently
    // find nothing. BeginRefresh()/Refresh()'s synchronous branch pick this
    // up and FinishRefresh() (or Refresh() itself, synchronously) applies
    // it once the listing is actually ready.
    // IS-0019: with no explicit name, moving to an ancestor (Backspace, Enter
    // on "..", Back, a typed parent path, the hotlist) selects the folder the
    // panel just came out of -- computed before path_ changes.
    pendingSelectName_ = selectName.empty() ? ChildNameToward(dir, path_) : selectName;
    path_ = dir;
    Refresh();
}

void Panel::PreviewLocation(const fs::path& dir) {
    if (provider_ && !LeaveProviderFor(false)) return;
    SetLocation(dir, L"");
}

void Panel::NavigateTo(const fs::path& dir, const std::wstring& selectName) {
    if (provider_ && !LeaveProviderFor(false)) return;
    // NAV-009: this central guard covers navigation initiated by search
    // results and drive/path commands as well as EnterSelected().
    if (IsReparsePoint(dir)) {
        statusMessage_ = kStrCannotEnterReparsePoint + dir.filename().wstring();
        return;
    }
    if (dir == path_) {
        SetLocation(dir, selectName);
        return;
    }
    backHistory_.push_back(path_);
    forwardHistory_.clear();
    SetLocation(dir, selectName);
}

bool Panel::NavigateBack() {
    if (provider_) {
        if (providerBack_.empty()) return CloseProvider();
        providerForward_.push_back(providerPath_);
        std::wstring destination = providerBack_.back();
        providerBack_.pop_back();
        SetProviderLocation(destination, L"");
        return true;
    }
    if (backHistory_.empty()) return false;
    forwardHistory_.push_back(path_);
    fs::path destination = backHistory_.back();
    backHistory_.pop_back();
    SetLocation(destination, L"");
    return true;
}

bool Panel::NavigateForward() {
    if (provider_) {
        if (providerForward_.empty()) return false;
        providerBack_.push_back(providerPath_);
        std::wstring destination = providerForward_.back();
        providerForward_.pop_back();
        SetProviderLocation(destination, L"");
        return true;
    }
    if (forwardHistory_.empty()) return false;
    backHistory_.push_back(path_);
    fs::path destination = forwardHistory_.back();
    forwardHistory_.pop_back();
    SetLocation(destination, L"");
    return true;
}

std::vector<std::wstring> Panel::SelectionOrCursor() const {
    std::vector<std::wstring> names;
    for (const auto& entry : entries_) {
        if (entry.selected && entry.name != L"..") names.push_back(entry.name);
    }
    if (names.empty() && cursor_ >= 0 && cursor_ < static_cast<int>(entries_.size())) {
        const Entry& entry = entries_[cursor_];
        if (entry.name != L"..") names.push_back(entry.name);
    }
    return names;
}

void Panel::OpenProvider(std::shared_ptr<IFileProvider> provider, std::wstring startPath, std::wstring returnSelectName,
                         ProviderLeaveHook onLeave) {
    provider_ = std::move(provider);
    providerReturnName_ = std::move(returnSelectName);
    providerLeave_ = std::move(onLeave);
    providerBack_.clear();
    providerForward_.clear();
    refreshIterator_.reset();
    loading_ = false;
    cursor_ = 0;
    topIndex_ = 0;
    SetProviderLocation(startPath, L"");
}

bool Panel::LeaveProviderFor(bool refreshLocal) {
    if (!provider_) return true;
    if (providerLeave_ && !providerLeave_(*provider_)) return false;
    std::wstring returnName = providerReturnName_;
    provider_.reset();
    providerLeave_ = nullptr;
    providerPath_.clear();
    providerReturnName_.clear();
    providerBack_.clear();
    providerForward_.clear();
    if (refreshLocal) {
        pendingSelectName_ = returnName;
        Refresh();
    }
    return true;
}

bool Panel::CloseProvider(bool force) {
    if (!provider_) return true;
    if (force) providerLeave_ = nullptr;
    return LeaveProviderFor(true);
}

std::wstring Panel::ItemLocation(const std::wstring& name) const {
    if (provider_) return provider_->Join(providerPath_, name);
    return (path_ / name).wstring();
}

std::wstring Panel::LocationText() const {
    if (provider_) return provider_->DisplayRoot() + providerPath_;
    return path_.wstring();
}

void Panel::NavigateProvider(const std::wstring& path, const std::wstring& selectName) {
    if (!provider_) return;
    if (!provider_->SamePath(path, providerPath_)) {
        providerBack_.push_back(providerPath_);
        providerForward_.clear();
    }
    SetProviderLocation(path, selectName);
}

void Panel::SetProviderLocation(const std::wstring& path, const std::wstring& selectName) {
    std::wstring select = selectName;
    // Coming back up out of a subfolder selects it (IS-0019's rule).
    if (select.empty() && !providerPath_.empty() && provider_ &&
        ProviderPathIsSameOrWithin(*provider_, path, providerPath_) && !provider_->SamePath(path, providerPath_)) {
        std::wstring child = providerPath_;
        while (!provider_->IsRoot(child) && !provider_->SamePath(provider_->Parent(child), path)) {
            std::wstring parent = provider_->Parent(child);
            if (parent == child) break;
            child = parent;
        }
        select = ProviderLeafName(*provider_, child);
    }
    pendingSelectName_ = select;
    providerPath_ = path;
    Refresh();
}

void Panel::RefreshFromProvider() {
    std::wstring previousCursorName = pendingSelectName_;
    pendingSelectName_.clear();
    if (previousCursorName.empty() && cursor_ >= 0 && cursor_ < static_cast<int>(entries_.size()))
        previousCursorName = entries_[cursor_].name;

    entries_.clear();
    statusMessage_.clear();
    volumeInfo_ = VolumeInfo{};
    // ".." is always there: at the provider's root it leaves the provider.
    entries_.push_back(Entry{L"..", true, 0});

    std::vector<ProviderEntry> listed;
    std::wstring error;
    ProviderStatus status = provider_->List(providerPath_, listed, {}, error);
    if (status != ProviderStatus::Ok) {
        statusMessage_ = kStrCannotListDirectory + (error.empty() ? std::wstring(ProviderStatusText(status)) : error);
        locationInaccessible_ = true;
    } else {
        locationInaccessible_ = false;
        for (auto& p : listed) {
            bool hiddenOrSystem = (p.attributes & (FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM)) != 0;
            if (hiddenOrSystem && !showHiddenAndSystem_) continue;
            if (!filter_.empty() && !WildcardMatch(p.name, filter_)) continue;
            Entry e;
            e.name = std::move(p.name);
            e.isDirectory = p.isDirectory;
            e.sizeBytes = p.isDirectory ? 0 : p.size;
            e.attributes = p.attributes;
            e.lastWriteTimeUtc = p.modifiedUtc;
            e.isReparsePoint = p.isLink;
            entries_.push_back(std::move(e));
        }
    }
    SortEntries();
    PlaceCursorOn(previousCursorName);
    ApplyPreservedState(previousCursorName);
}

} // namespace mc
