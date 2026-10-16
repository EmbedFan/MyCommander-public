// FEA-0010: the Network Info panel's behaviour -- which panel it takes, Esc and the toggle, the hidden file
// panel's state, scanning on worker threads and closing without waiting. Everything runs against a fake host
// and a fake source: no real network call, no real panel.

#include "TestFramework.h"

#include "../NetInfoPlugin/FakeNetInfoSources.h"
#include "../NetInfoPlugin/NetInfoController.h"
#include "../NetInfoPlugin/DetachedThread.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <chrono>
#include <map>

using namespace netinfo;

namespace {

class FakeHost : public IPanelHost {
public:
    int active = 0;
    bool provider[2] = {false, false};
    bool requestOk = true;
    bool canNavigate = true;
    bool navigateOk = true;
    int choiceAnswer = 0;
    std::wstring path[2] = {L"C:\\Left", L"D:\\Right"};
    std::vector<std::wstring> events;
    std::vector<std::vector<std::wstring>> messages;
    std::map<int, std::wstring> screen;  // y -> text of the last draw
    DWORD owner = GetCurrentThreadId();
    bool wrongThread = false;

    int ActiveSide() override { return active; }
    bool IsInProvider(int side) override { return provider[side]; }
    bool RequestContent(int side, const std::wstring& title) override {
        events.push_back(L"request " + std::to_wstring(side) + L" " + title);
        return requestOk;
    }
    void ReleaseContent(int side) override { events.push_back(L"release " + std::to_wstring(side)); }
    std::wstring PanelPath(int side) override { return path[side]; }
    void RefreshPanel(int side) override { events.push_back(L"refresh " + std::to_wstring(side)); }
    bool CanNavigate() override { return canNavigate; }
    bool NavigatePanel(int side, const std::wstring& p) override {
        events.push_back(L"navigate " + std::to_wstring(side) + L" " + p);
        return navigateOk;
    }
    void ShowMessage(const std::wstring&, const std::vector<std::wstring>& lines) override { messages.push_back(lines); }
    int ShowChoice(const std::wstring&, const std::vector<std::wstring>&,
                   const std::vector<std::pair<wchar_t, std::wstring>>&) override {
        events.push_back(L"choice");
        return choiceAnswer;
    }
    void PutText(int, int, int y, const std::wstring& text, DrawColor) override {
        if (GetCurrentThreadId() != owner) wrongThread = true;
        screen[y] = text;
    }
    void Log(int, const std::wstring&) override {}

    bool Saw(const std::wstring& e) const {
        for (const auto& x : events)
            if (x.find(e) == 0) return true;
        return false;
    }
    std::wstring Screen() const {
        std::wstring all;
        for (const auto& kv : screen) all += kv.second + L"\n";
        return all;
    }
};

std::shared_ptr<FakeSource> SampleSource() {
    auto src = std::make_shared<FakeSource>();
    src->local.computerName = L"PC1";
    ShareEntry pub;
    pub.name = L"Pub";
    pub.path = L"C:\\Pub";
    src->local.shares = {pub};
    src->wifi.status = WifiStatus::Ok;
    WifiNetwork home;
    home.ssid = L"Home";
    home.signal = 80;
    home.security = L"WPA2-Personal";
    home.connected = true;
    src->wifi.networks = {home};
    Machine nas;
    nas.name = L"nas";
    nas.host = L"nas";
    nas.address = L"192.168.1.5";
    Machine laptop;
    laptop.name = L"laptop";
    laptop.host = L"laptop";
    laptop.address = L"192.168.1.9";
    src->machines.machines = {nas, laptop};
    SharesResult ok;
    ok.ok = true;
    ShareEntry media;
    media.name = L"media";
    ok.shares = {media};
    src->shares[L"nas"] = ok;
    return src;
}

template <typename Pred>
bool WaitFor(Pred pred, int ms = 3000) {
    for (int waited = 0; waited < ms; waited += 5) {
        if (pred()) return true;
        Sleep(5);
    }
    return pred();
}

// Draws until the rows match what the scan produced (the worker threads update the snapshot asynchronously).
bool DrawUntil(Controller& c, FakeHost& host, const std::wstring& text, int ms = 3000) {
    return WaitFor(
        [&] {
            c.OnDraw(c.Side(), 70, 30, true, GetTickCount64());
            return host.Screen().find(text) != std::wstring::npos;
        },
        ms);
}

int RowOf(const Controller& c, RowKind kind, const std::wstring& part) {
    const auto& rows = c.Rows();
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == kind && rows[i].text.find(part) != std::wstring::npos) return static_cast<int>(i);
    return -1;
}

void PutCursorOn(Controller& c, RowKind kind, const std::wstring& part) {
    // Walks the real key path (Up/Down) so the test also covers the cursor handling.
    for (int i = 0; i < 80; ++i) {
        int row = RowOf(c, kind, part);
        if (row < 0 || c.View().cursor == row) return;
        c.OnKey(c.Side(), c.View().cursor < row ? VK_DOWN : VK_UP, 0);
    }
}

}  // namespace

TEST_CASE(Controller_OpensOnThePanelThatIsNotActive_NET002) {
    FakeHost host;
    host.active = 0;
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(c.IsOpen());
    CHECK(c.Side() == 1);
    CHECK(host.Saw(L"request 1 Network info"));
    CHECK(!host.Saw(L"request 0"));
    c.Close();

    FakeHost host2;
    host2.active = 1;
    Controller c2(host2, SampleSource());
    c2.Toggle();
    CHECK(c2.Side() == 0);
    c2.Close();
}

TEST_CASE(Controller_RemembersTheHiddenPanelFolder_NET003) {
    FakeHost host;
    host.path[1] = L"D:\\Deep\\Folder";
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(c.SavedPath() == L"D:\\Deep\\Folder");
    c.Close();
}

TEST_CASE(Controller_EscClosesReleasesAndRefreshesTheFilePanel_NET003_NET004) {
    FakeHost host;
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(c.IsOpen());
    c.OnKey(1, VK_ESCAPE, 0);
    CHECK(!c.IsOpen());
    CHECK(host.Saw(L"release 1"));
    CHECK(host.Saw(L"refresh 1"));
    // release comes before refresh, so the listing that is refreshed is the file panel, not the network view
    size_t rel = 0, ref = 0;
    for (size_t i = 0; i < host.events.size(); ++i) {
        if (host.events[i] == L"release 1") rel = i;
        if (host.events[i] == L"refresh 1") ref = i;
    }
    CHECK(rel < ref);
    // keys after closing are ignored
    c.OnKey(1, VK_DOWN, 0);
    c.OnDraw(1, 60, 20, true, 1);
    CHECK(host.screen.empty());
}

TEST_CASE(Controller_ToggleClosesAnOpenPanel_AndReopensIt_NET004) {
    FakeHost host;
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(c.IsOpen());
    c.Toggle();
    CHECK(!c.IsOpen());
    c.Toggle();
    CHECK(c.IsOpen());
    c.Close();
}

TEST_CASE(Controller_OnlyHandlesItsOwnSide_NET002) {
    FakeHost host;
    Controller c(host, SampleSource());
    c.Toggle();
    c.OnKey(0, VK_ESCAPE, 0);  // the other panel's key: not ours
    CHECK(c.IsOpen());
    c.OnDraw(0, 60, 20, true, 1);
    CHECK(host.screen.empty());
    c.Close();
}

TEST_CASE(Controller_OtherPanelInProvider_AsksOrRefuses_NET002) {
    FakeHost host;
    host.provider[1] = true;
    host.choiceAnswer = 1;  // Cancel
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(!c.IsOpen());
    CHECK(host.Saw(L"choice"));
    CHECK(!host.Saw(L"request"));

    host.choiceAnswer = 0;  // Use this panel
    c.Toggle();
    CHECK(c.IsOpen());
    CHECK(c.Side() == 0);   // the active panel was taken instead
    c.Close();

    FakeHost both;
    both.provider[0] = both.provider[1] = true;
    Controller c2(both, SampleSource());
    c2.Toggle();
    CHECK(!c2.IsOpen());
    CHECK(both.messages.size() == 1);
}

TEST_CASE(Controller_PanelCannotBeTaken_ShowsAMessage_NET002) {
    FakeHost host;
    host.requestOk = false;
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(!c.IsOpen());
    CHECK(host.messages.size() == 1);
    c.Close();  // harmless
    CHECK(!host.Saw(L"release"));
}

TEST_CASE(Controller_ScanResultsAppearAndNeverFromAWorkerThread_NET001_NET009) {
    FakeHost host;
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(DrawUntil(c, host, L"laptop"));
    std::wstring screen = host.Screen();
    CHECK(screen.find(L"PC1") != std::wstring::npos);
    CHECK(screen.find(L"Pub") != std::wstring::npos);
    CHECK(screen.find(L"* Home") != std::wstring::npos);  // the connected network is emitted
    CHECK(screen.find(L"nas") != std::wstring::npos);
    CHECK(!host.wrongThread);
    c.Close();
}

TEST_CASE(Controller_EnterOnAMachineReadsItsShares_ErrorsStayOnThatMachine_NET007) {
    auto src = SampleSource();
    FakeHost host;
    Controller c(host, src);
    c.Toggle();
    CHECK(DrawUntil(c, host, L"laptop"));
    PutCursorOn(c, RowKind::Machine, L"nas");
    c.OnKey(1, VK_RETURN, 0);
    CHECK(DrawUntil(c, host, L"media"));
    CHECK(src->shareCalls == 1);
    // the second machine has no answer configured: "Access denied" appears under it, not under nas
    PutCursorOn(c, RowKind::Machine, L"laptop");
    c.OnKey(1, VK_RETURN, 0);
    CHECK(DrawUntil(c, host, L"Access denied"));
    const auto& rows = c.Rows();
    int nas = RowOf(c, RowKind::Machine, L"nas");
    int laptop = RowOf(c, RowKind::Machine, L"laptop");
    CHECK(rows[static_cast<size_t>(nas) + 1].kind == RowKind::MachineShare);
    CHECK(rows[static_cast<size_t>(laptop) + 1].kind == RowKind::MachineNote);
    CHECK(rows[static_cast<size_t>(laptop) + 1].warn);
    // collapsing and expanding again does not re-ask for a machine whose shares are known
    PutCursorOn(c, RowKind::Machine, L"nas");
    c.OnKey(1, VK_RETURN, 0);
    c.OnKey(1, VK_RETURN, 0);
    CHECK(src->shareCalls == 2);
    c.Close();
}

TEST_CASE(Controller_EnterOnAShareOpensItInTheOtherPanel_NET008) {
    auto src = SampleSource();
    FakeHost host;
    Controller c(host, src);
    c.Toggle();  // takes panel 1; panel 0 is the file panel
    CHECK(DrawUntil(c, host, L"laptop"));
    PutCursorOn(c, RowKind::Machine, L"nas");
    c.OnKey(1, VK_RETURN, 0);
    CHECK(DrawUntil(c, host, L"media"));
    PutCursorOn(c, RowKind::MachineShare, L"media");
    c.OnKey(1, VK_RETURN, 0);
    CHECK(host.Saw(L"navigate 0 \\\\nas\\media"));
    CHECK(c.StatusNote().find(L"Opened") != std::wstring::npos);

    PutCursorOn(c, RowKind::LocalShare, L"Pub");
    c.OnKey(1, VK_RETURN, 0);
    CHECK(host.Saw(L"navigate 0 C:\\Pub"));
    c.Close();
}

TEST_CASE(Controller_NavigationRefusedOrMissing_IsExplained_NET008) {
    FakeHost host;
    host.navigateOk = false;
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(DrawUntil(c, host, L"Pub"));
    PutCursorOn(c, RowKind::LocalShare, L"Pub");
    c.OnKey(1, VK_RETURN, 0);
    CHECK(host.messages.size() == 1);  // the other panel is busy: told, not silently ignored
    host.canNavigate = false;
    c.OnKey(1, VK_RETURN, 0);
    CHECK(host.messages.size() == 2);  // an older host without NavigatePanel
    c.Close();
}

TEST_CASE(Controller_F5AndCtrlRRescan_NET009) {
    auto src = SampleSource();
    FakeHost host;
    Controller c(host, src);
    c.Toggle();
    CHECK(DrawUntil(c, host, L"laptop"));
    CHECK(WaitFor([&] { return src->wifiCalls == 1 && src->machineCalls == 1 && !c.Busy(); }));
    c.OnKey(1, VK_F5, 0);
    CHECK(WaitFor([&] { return src->wifiCalls == 2 && src->machineCalls == 2 && !c.Busy(); }));
    c.OnKey(1, 'R', LEFT_CTRL_PRESSED);
    CHECK(WaitFor([&] { return src->wifiCalls == 3 && src->machineCalls == 3 && !c.Busy(); }));
    c.Close();
}

TEST_CASE(Controller_WifiIsRescannedOnATimerWhileOpen_NET009) {
    auto src = SampleSource();
    FakeHost host;
    Controller c(host, src);
    c.Toggle();
    CHECK(WaitFor([&] { return src->wifiCalls == 1; }));
    c.OnDraw(1, 60, 20, true, 1000);            // first tick only records the time
    c.OnDraw(1, 60, 20, true, 1000 + 29000);    // not yet
    Sleep(50);
    CHECK(src->wifiCalls == 1);
    c.OnDraw(1, 60, 20, true, 1000 + 31000);    // 30 s later
    CHECK(WaitFor([&] { return src->wifiCalls == 2; }));
    c.Close();
}

TEST_CASE(Controller_ClosingWhileAScanIsBlocked_ReturnsAtOnce_NET009) {
    auto src = SampleSource();
    std::atomic<bool> release{false};
    src->onFindMachines = [&](const CancelFn& cancelled) {
        while (!release.load() && !cancelled()) Sleep(5);
        if (!cancelled()) return;
        while (!release.load()) Sleep(5);  // pretend the Windows call itself does not return until later
    };
    FakeHost host;
    Controller c(host, src);
    c.Toggle();
    CHECK(WaitFor([&] { return src->machineCalls == 1; }));
    const auto start = std::chrono::steady_clock::now();
    c.OnKey(1, VK_ESCAPE, 0);
    const auto took = std::chrono::steady_clock::now() - start;
    CHECK(!c.IsOpen());
    CHECK(took < std::chrono::milliseconds(1500));
    CHECK(host.Saw(L"release 1"));
    release = true;  // let the abandoned worker finish
    Sleep(30);
    // reopening starts fresh and works
    c.Toggle();
    CHECK(c.IsOpen());
    CHECK(DrawUntil(c, host, L"laptop"));
    c.Close();
}

TEST_CASE(Controller_NoScanRunsWhileClosed_NET009) {
    auto src = SampleSource();
    FakeHost host;
    Controller c(host, src);
    CHECK(src->wifiCalls == 0);
    CHECK(src->machineCalls == 0);
    CHECK(src->localCalls == 0);
    c.Toggle();
    CHECK(WaitFor([&] { return src->wifiCalls == 1 && src->machineCalls == 1; }));
    c.Close();
    int w = src->wifiCalls, m = src->machineCalls;
    c.OnDraw(1, 60, 20, true, 999999);
    c.OnKey(1, VK_F5, 0);
    Sleep(50);
    CHECK(src->wifiCalls == w);
    CHECK(src->machineCalls == m);
}

TEST_CASE(Controller_DetailsBoxForAWifiNetwork_NET005) {
    FakeHost host;
    Controller c(host, SampleSource());
    c.Toggle();
    CHECK(DrawUntil(c, host, L"Home"));
    PutCursorOn(c, RowKind::Wifi, L"Home");
    c.OnKey(1, VK_RETURN, 0);
    CHECK(host.messages.size() == 1);
    bool connected = false;
    for (const auto& l : host.messages[0])
        if (l.find(L"connected") != std::wstring::npos) connected = true;
    CHECK(connected);
    c.Close();
}

TEST_CASE(Scanner_RescanKeepsTheSharesAlreadyRead_NET007) {
    auto src = SampleSource();
    Scanner s(src);
    s.StartAll();
    CHECK(WaitFor([&] { return s.Get().machinesDone; }));
    s.StartShares(L"nas");
    CHECK(WaitFor([&] {
        Snapshot snap = s.Get();
        for (const Machine& m : snap.machines)
            if (m.name == L"nas") return m.shares == LoadState::Loaded;  // sorted by name: "laptop" comes first
        return false;
    }));
    s.StartMachines();
    CHECK(WaitFor([&] { return src->machineCalls == 2 && !s.Busy(); }));
    Snapshot snap = s.Get();
    int nas = snap.machines[0].name == L"nas" ? 0 : 1;  // (sorted by name, so normally 1)
    CHECK(snap.machines[static_cast<size_t>(nas)].shares == LoadState::Loaded);
    CHECK(snap.machines[static_cast<size_t>(nas)].shareList.size() == 1);
}

TEST_CASE(Scanner_StopWaitsForQuickWorkersAndResetsTheSnapshot_NET009) {
    auto src = SampleSource();
    Scanner s(src);
    s.StartAll();
    CHECK(WaitFor([&] { return s.Get().machinesDone; }));
    CHECK(s.Stop(1000));
    CHECK(s.Get().machines.empty());
    CHECK(!s.Get().localLoaded);
}

TEST_CASE(Scanner_AScanOfOneKindIsNotStartedTwice_NET009) {
    auto src = SampleSource();
    std::atomic<bool> go{false};
    src->onReadWifi = [&](const CancelFn&) {
        while (!go.load()) Sleep(2);
    };
    Scanner s(src);
    s.StartAll();
    s.StartWifi();
    s.StartWifi();
    Sleep(30);
    CHECK(src->wifiCalls == 1);
    go = true;
    CHECK(WaitFor([&] { return s.Get().wifiStatus == WifiStatus::Ok; }));
}


TEST_CASE(RunDetached_RunsTheFunctionAndKeepsTheModuleMapped_FEA0010) {
    HANDLE done = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    CHECK(done != nullptr);
    const bool started = RunDetached([done]() { SetEvent(done); });
    CHECK(started);
    CHECK(WaitForSingleObject(done, 5000) == WAIT_OBJECT_0);
    CloseHandle(done);
    // The extra module reference must have been released again: this module is still loaded exactly as before.
    HMODULE self = nullptr;
    CHECK(GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                             reinterpret_cast<LPCWSTR>(&detail::DetachedThreadMain), &self));
    CHECK(self != nullptr);
}
