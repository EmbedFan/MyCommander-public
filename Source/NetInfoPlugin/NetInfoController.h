#pragma once

// FEA-0010: the panel's behaviour -- which panel it takes, what Esc and the toggle do, what Enter on a row does,
// how the hidden file panel is checked and refreshed when the network panel closes. It talks to the application
// only through IPanelHost, so NetInfoControllerTests.cpp can drive it with a fake host. NetInfoPlugin.cpp is the
// thin adapter from McPluginHost to IPanelHost plus the exports.

#include "NetInfoModel.h"
#include "NetInfoScanner.h"

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace netinfo {

class IPanelHost {
public:
    virtual ~IPanelHost() = default;
    virtual int ActiveSide() = 0;  // 0 = left, 1 = right
    virtual bool IsInProvider(int side) = 0;
    virtual bool RequestContent(int side, const std::wstring& title) = 0;
    virtual void ReleaseContent(int side) = 0;
    virtual std::wstring PanelPath(int side) = 0;
    virtual void RefreshPanel(int side) = 0;
    virtual bool CanNavigate() = 0;  // false on a host older than API 1.13
    virtual bool NavigatePanel(int side, const std::wstring& path) = 0;
    virtual void ShowMessage(const std::wstring& title, const std::vector<std::wstring>& lines) = 0;
    // Returns the index of the chosen option, -1 on Esc.
    virtual int ShowChoice(const std::wstring& title, const std::vector<std::wstring>& lines,
                           const std::vector<std::pair<wchar_t, std::wstring>>& options) = 0;
    virtual void PutText(int side, int x, int y, const std::wstring& text, DrawColor color) = 0;
    virtual void Log(int level, const std::wstring& message) = 0;  // 0 error, 1 warning, 2 info, 3 debug
};

class Controller {
public:
    Controller(IPanelHost& host, std::shared_ptr<INetInfoSource> source);
    ~Controller();

    bool IsOpen() const { return open_; }
    int Side() const { return side_; }
    // Menu item / shortcut: opens on the panel that is not active, or closes when already open.
    void Toggle();
    void Close();
    void Shutdown() { Close(); }

    void OnDraw(int side, int width, int height, bool active, uint64_t nowMs);
    void OnKey(int side, uint16_t virtualKey, uint32_t controlKeyState);
    void OnClick(int side, int x, int y, bool doubleClick);

    bool Busy() const { return scanner_ && scanner_->Busy(); }
    // For tests.
    Snapshot CurrentSnapshot() const { return snapshot_; }
    const std::vector<Row>& Rows() const { return rows_; }
    const ViewState& View() const { return view_; }
    const std::wstring& SavedPath() const { return savedPath_; }
    const std::wstring& StatusNote() const { return statusNote_; }

private:
    void Open();
    void RefreshRows();
    void Apply(const Action& action);
    void OpenShareAt(int rowIndex);

    IPanelHost& host_;
    std::shared_ptr<INetInfoSource> source_;
    std::unique_ptr<Scanner> scanner_;
    bool open_ = false;
    int side_ = 0;
    std::wstring savedPath_;  // the hidden file panel's folder when the network panel opened
    ViewState view_;
    std::vector<Row> rows_;
    Snapshot snapshot_;
    uint64_t rowsVersion_ = 0;
    bool rowsDirty_ = true;
    int visibleRows_ = 20;
    std::wstring statusNote_;
};

}  // namespace netinfo
