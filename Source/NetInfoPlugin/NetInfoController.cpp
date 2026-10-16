#include "NetInfoController.h"

#include <windows.h>

namespace netinfo {

namespace {

constexpr wchar_t kTitle[] = L"Network info";
constexpr uint64_t kWifiRefreshMs = 30000;  // while the panel is open, Wi-Fi signals move

}  // namespace

Controller::Controller(IPanelHost& host, std::shared_ptr<INetInfoSource> source)
    : host_(host), source_(std::move(source)) {}

Controller::~Controller() { Close(); }

void Controller::Toggle() {
    if (open_) Close();
    else Open();
}

void Controller::Open() {
    const int active = host_.ActiveSide() == 0 ? 0 : 1;
    int target = 1 - active;  // the panel that is not active
    if (host_.IsInProvider(target)) {
        if (host_.IsInProvider(active)) {
            host_.ShowMessage(kTitle, {L"Both panels show an archive or a server.", L"Leave one of them first."});
            return;
        }
        int choice = host_.ShowChoice(kTitle, {L"The other panel shows an archive or a server.", L"Use this panel instead?"},
                                      {{L'U', L"Use this panel"}, {L'C', L"Cancel"}});
        if (choice != 0) return;
        target = active;
    }
    if (!host_.RequestContent(target, kTitle)) {
        host_.ShowMessage(kTitle, {L"The panel could not be taken over.", L"Another view is installed on it."});
        return;
    }
    side_ = target;
    savedPath_ = host_.PanelPath(side_);  // the panel keeps its folder and cursor underneath; remembered for the check on close
    view_ = ViewState{};
    rows_.clear();
    snapshot_ = Snapshot{};
    rowsDirty_ = true;
    rowsVersion_ = 0;
    statusNote_.clear();
    scanner_ = std::make_unique<Scanner>(source_);
    open_ = true;
    scanner_->StartAll();
    host_.Log(2, std::wstring(L"Network info opened on the ") + (side_ == 0 ? L"left" : L"right") + L" panel");
}

void Controller::Close() {
    if (!open_) return;
    open_ = false;
    // Workers hold only their own state and cannot change the panel any more, so closing never waits for a slow
    // Windows call: a short grace period, then the scanner is dropped.
    if (scanner_ && !scanner_->Stop(50)) host_.Log(2, L"Network info closed while a scan was still running");
    scanner_.reset();
    host_.ReleaseContent(side_);
    host_.RefreshPanel(side_);  // the listing may have changed while it was hidden; the host restores the cursor by name
    if (host_.PanelPath(side_) != savedPath_)
        host_.Log(1, L"The hidden panel's folder changed while Network info was open");
    host_.Log(2, L"Network info closed");
}

void Controller::RefreshRows() {
    if (!scanner_) return;
    const uint64_t version = scanner_->Version();
    if (!rowsDirty_ && version == rowsVersion_) return;
    snapshot_ = scanner_->Get();
    BuildOptions options;
    options.expanded = view_.expanded;
    std::vector<Row> fresh = BuildRows(snapshot_, options);
    view_.cursor = RestoreCursor(rows_, view_.cursor, fresh);
    rows_ = std::move(fresh);
    rowsVersion_ = version;
    rowsDirty_ = false;
}

void Controller::OnDraw(int side, int width, int height, bool active, uint64_t nowMs) {
    if (!open_ || side != side_ || !scanner_) return;
    scanner_->Tick(nowMs, kWifiRefreshMs);
    RefreshRows();
    visibleRows_ = height > 1 ? height - 1 : 1;
    EnsureVisible(rows_, view_, visibleRows_);
    const std::wstring status = statusNote_.empty() ? DefaultStatusText(snapshot_) : statusNote_;
    for (const DrawLine& line : RenderPanel(rows_, view_, width, height, active, status))
        host_.PutText(side_, 0, line.y, line.text, line.color);
}

void Controller::OnKey(int side, uint16_t vk, uint32_t controlKeyState) {
    if (!open_ || side != side_ || !scanner_) return;
    RefreshRows();
    statusNote_.clear();
    const bool ctrl = (controlKeyState & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    if (ctrl && vk == 'R') {
        Apply({ActionKind::Rescan, -1});
        return;
    }
    Apply(HandleKey(rows_, view_, vk, visibleRows_));
}

void Controller::OnClick(int side, int /*x*/, int y, bool doubleClick) {
    if (!open_ || side != side_ || !scanner_) return;
    RefreshRows();
    statusNote_.clear();
    Apply(HandleClick(rows_, view_, y, doubleClick, visibleRows_));
}

void Controller::Apply(const Action& action) {
    switch (action.kind) {
        case ActionKind::None: return;
        case ActionKind::Close: Close(); return;
        case ActionKind::Rescan:
            scanner_->StartAll();
            statusNote_ = L"Rescanning...";
            return;
        case ActionKind::ToggleMachine: {
            if (action.rowIndex < 0 || action.rowIndex >= static_cast<int>(rows_.size())) return;
            const Row& row = rows_[static_cast<size_t>(action.rowIndex)];
            if (row.kind != RowKind::Machine) return;
            if (view_.expanded.erase(row.key) == 0) {
                view_.expanded.insert(row.key);
                if (row.index >= 0 && row.index < static_cast<int>(snapshot_.machines.size())) {
                    const Machine& m = snapshot_.machines[static_cast<size_t>(row.index)];
                    if (m.shares == LoadState::NotLoaded || m.shares == LoadState::Failed)
                        scanner_->StartShares(m.host.empty() ? m.name : m.host);
                }
            }
            rowsDirty_ = true;
            return;
        }
        case ActionKind::OpenShare: OpenShareAt(action.rowIndex); return;
        case ActionKind::Details: {
            if (action.rowIndex < 0 || action.rowIndex >= static_cast<int>(rows_.size())) return;
            std::vector<std::wstring> lines = DetailLines(snapshot_, rows_[static_cast<size_t>(action.rowIndex)]);
            if (!lines.empty()) host_.ShowMessage(kTitle, lines);
            return;
        }
    }
}

void Controller::OpenShareAt(int rowIndex) {
    if (rowIndex < 0 || rowIndex >= static_cast<int>(rows_.size())) return;
    const Row& row = rows_[static_cast<size_t>(rowIndex)];
    std::wstring path;
    switch (row.kind) {
        case RowKind::LocalShare:
            if (row.index >= 0 && row.index < static_cast<int>(snapshot_.localShares.size()))
                path = ShareOpenPath(snapshot_.localShares[static_cast<size_t>(row.index)], nullptr);
            break;
        case RowKind::MappedDrive:
            if (row.index >= 0 && row.index < static_cast<int>(snapshot_.mapped.size()))
                path = snapshot_.mapped[static_cast<size_t>(row.index)].local + L"\\";
            break;
        case RowKind::MachineShare:
            if (row.index >= 0 && row.index < static_cast<int>(snapshot_.machines.size())) {
                const Machine& m = snapshot_.machines[static_cast<size_t>(row.index)];
                if (row.share >= 0 && row.share < static_cast<int>(m.shareList.size()))
                    path = ShareOpenPath(m.shareList[static_cast<size_t>(row.share)], &m);
            }
            break;
        default: return;
    }
    if (path.empty()) {
        statusNote_ = L"Only folder shares can be opened";
        return;
    }
    if (!host_.CanNavigate()) {
        host_.ShowMessage(kTitle, {L"This MyCommander cannot open a folder from a plugin panel.", L"Path: " + path});
        return;
    }
    if (!host_.NavigatePanel(1 - side_, path)) {
        host_.ShowMessage(kTitle, {L"The other panel is showing an archive, a server or another view.",
                                   L"Leave it first, then try again.", L"Path: " + path});
        return;
    }
    statusNote_ = L"Opened " + path + L" in the other panel";
}

}  // namespace netinfo
