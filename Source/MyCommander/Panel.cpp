#include "Panel.h"

#include "PathUtil.h"
#include "Strings.h"

#include <algorithm>

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
    cursor_ = 0;
    if (!refreshCursorName_.empty()) {
        auto found = std::find_if(entries_.begin(), entries_.end(),
                                  [&](const Entry& e) { return e.name == refreshCursorName_; });
        if (found != entries_.end()) cursor_ = static_cast<int>(std::distance(entries_.begin(), found));
    }
    topIndex_ = std::min(topIndex_, cursor_);
    topIndex_ = std::max(topIndex_, 0);
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

void Panel::EnterSelected() {
    if (entries_.empty()) return;
    const Entry& selected = entries_[cursor_];
    if (!selected.isDirectory) return;

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
    uint64_t total = 0;
    for (const auto& entry : entries_) {
        if (entry.selected) total += entry.sizeBytes;
    }
    return total;
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
    pendingSelectName_ = selectName;
    path_ = dir;
    Refresh();
}

void Panel::NavigateTo(const fs::path& dir, const std::wstring& selectName) {
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
    if (backHistory_.empty()) return false;
    forwardHistory_.push_back(path_);
    fs::path destination = backHistory_.back();
    backHistory_.pop_back();
    SetLocation(destination, L"");
    return true;
}

bool Panel::NavigateForward() {
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

} // namespace mc
