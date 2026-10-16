// FEA-0010: the Network Info plugin's pure model -- text helpers, rows, cursor and key rules, drawing.

#include "TestFramework.h"

#include "../NetInfoPlugin/NetInfoModel.h"
#include "../NetInfoPlugin/NetInfoSources.h"
#include "TextWidth.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

using namespace netinfo;

namespace {

ShareEntry Share(const std::wstring& name, ShareType type = ShareType::Disk, const std::wstring& remark = L"",
                 const std::wstring& path = L"", bool special = false) {
    ShareEntry s;
    s.name = name;
    s.type = type;
    s.remark = remark;
    s.path = path;
    s.special = special;
    return s;
}

Machine MakeMachine(const std::wstring& name, const std::wstring& address) {
    Machine m;
    m.name = name;
    m.host = name;
    m.address = address;
    m.viaNeighbour = true;
    return m;
}

WifiNetwork Wifi(const std::wstring& ssid, int signal, bool connected = false) {
    WifiNetwork n;
    n.ssid = ssid;
    n.signal = signal;
    n.security = L"WPA2-Personal";
    n.secured = true;
    n.connected = connected;
    return n;
}

Snapshot Sample() {
    Snapshot s;
    s.computerName = L"PC1";
    s.localLoaded = true;
    s.localShares = {Share(L"Pub", ShareType::Disk, L"", L"C:\\Pub"), Share(L"C$", ShareType::Disk, L"Default share", L"C:\\", true)};
    s.mapped = {{L"Z:", L"\\\\nas\\media"}};
    s.wifiStatus = WifiStatus::Ok;
    s.wifi = {Wifi(L"Home", 80, true), Wifi(L"Cafe", 40)};
    s.machinesDone = true;
    s.machines = {MakeMachine(L"nas", L"192.168.1.5"), MakeMachine(L"laptop", L"192.168.1.9")};
    return s;
}

int FindRow(const std::vector<Row>& rows, RowKind kind, const std::wstring& textPart) {
    for (size_t i = 0; i < rows.size(); ++i)
        if (rows[i].kind == kind && rows[i].text.find(textPart) != std::wstring::npos) return static_cast<int>(i);
    return -1;
}

}  // namespace

TEST_CASE(SignalBar_MapsQualityToFourSteps_NET005) {
    CHECK(SignalBar(0) == L"[....]");
    CHECK(SignalBar(100) == L"[####]");
    CHECK(SignalBar(50) == L"[##..]");
    CHECK(SignalBar(-5) == L"[....]");
    CHECK(SignalBar(250) == L"[####]");
}

TEST_CASE(SecurityName_KnownAlgorithmsAndFallback_NET005) {
    CHECK(SecurityName(7, true) == L"WPA2-Personal");
    CHECK(SecurityName(9, true) == L"WPA3-Personal");
    CHECK(SecurityName(1, false) == L"Open");
    CHECK(SecurityName(1, true) == L"Secured");
    CHECK(SecurityName(99, false) == L"Open");
    CHECK(SecurityName(99, true) == L"Secured");
}

TEST_CASE(SortWifi_ConnectedFirstThenStrongerThenName_NET005) {
    std::vector<WifiNetwork> nets = {Wifi(L"b", 50), Wifi(L"A", 50), Wifi(L"strong", 90), Wifi(L"mine", 10, true)};
    SortWifi(nets);
    CHECK(nets[0].ssid == L"mine");
    CHECK(nets[1].ssid == L"strong");
    CHECK(nets[2].ssid == L"A");
    CHECK(nets[3].ssid == L"b");
}

TEST_CASE(SortMachines_NamedFirstThenByAddress_NET001) {
    std::vector<Machine> ms = {MakeMachine(L"192.168.1.9", L"192.168.1.9"), MakeMachine(L"Zed", L"10.0.0.2"),
                               MakeMachine(L"alpha", L"10.0.0.3"), MakeMachine(L"192.168.1.2", L"192.168.1.2")};
    SortMachines(ms);
    CHECK(ms[0].name == L"alpha");
    CHECK(ms[1].name == L"Zed");
    CHECK(ms[2].name == L"192.168.1.2");
    CHECK(ms[3].name == L"192.168.1.9");
}

TEST_CASE(ShareOpenPath_FoldersOnly_NET008) {
    Machine m = MakeMachine(L"nas", L"192.168.1.5");
    CHECK(ShareOpenPath(Share(L"media"), &m) == L"\\\\nas\\media");
    CHECK(ShareOpenPath(Share(L"Pub", ShareType::Disk, L"", L"C:\\Pub"), nullptr) == L"C:\\Pub");
    CHECK(ShareOpenPath(Share(L"printer", ShareType::Printer), &m).empty());
    CHECK(ShareOpenPath(Share(L"IPC$", ShareType::Ipc), &m).empty());
    Machine unnamed;
    CHECK(ShareOpenPath(Share(L"media"), &unnamed).empty());
}

TEST_CASE(ShortHostName_StripsTheDomainButNotAddresses_NET001) {
    CHECK(ShortHostName(L"nas.fritz.box") == L"nas");
    CHECK(ShortHostName(L"nas") == L"nas");
    CHECK(ShortHostName(L"192.168.1.5") == L"192.168.1.5");
    CHECK(ShortHostName(L"fe80::1") == L"fe80::1");
    CHECK(ShortHostName(L"").empty());
}

TEST_CASE(ShareErrorText_FriendlyForTheCommonErrors_NET007) {
    CHECK(ShareErrorText(ERROR_ACCESS_DENIED) == L"Access denied");
    CHECK(ShareErrorText(ERROR_BAD_NETPATH) == L"The machine could not be reached");
    CHECK(!ShareErrorText(ERROR_FILE_NOT_FOUND).empty());
}

TEST_CASE(BuildRows_ThreeSectionsInOrder_NET001) {
    Snapshot s = Sample();
    std::vector<Row> rows = BuildRows(s, {});
    int computer = FindRow(rows, RowKind::Heading, L"This computer");
    int wifi = FindRow(rows, RowKind::Heading, L"Wi-Fi");
    int net = FindRow(rows, RowKind::Heading, L"Network");
    CHECK(computer == 0);
    CHECK(wifi > computer);
    CHECK(net > wifi);
    CHECK(rows[0].detail == L"PC1");
    CHECK(FindRow(rows, RowKind::LocalShare, L"Pub") > computer);
    CHECK(FindRow(rows, RowKind::MappedDrive, L"Z:") > computer);
    CHECK(FindRow(rows, RowKind::Machine, L"nas") > net);
    CHECK(FindRow(rows, RowKind::Machine, L"laptop") > net);
}

TEST_CASE(BuildRows_AdministrativeSharesAreHiddenByDefault_NET001) {
    Snapshot s = Sample();
    CHECK(FindRow(BuildRows(s, {}), RowKind::LocalShare, L"C$") < 0);
    BuildOptions all;
    all.showSpecialShares = true;
    CHECK(FindRow(BuildRows(s, all), RowKind::LocalShare, L"C$") >= 0);
}

TEST_CASE(BuildRows_NoSharesShowsAPlainLine_NET001) {
    Snapshot s = Sample();
    s.localShares.clear();
    s.mapped.clear();
    std::vector<Row> rows = BuildRows(s, {});
    CHECK(FindRow(rows, RowKind::Text, L"no shared folders") >= 0);
}

TEST_CASE(BuildRows_ConnectedNetworkIsEmitted_NET005) {
    Snapshot s = Sample();
    std::vector<Row> rows = BuildRows(s, {});
    int home = FindRow(rows, RowKind::Wifi, L"Home");
    int cafe = FindRow(rows, RowKind::Wifi, L"Cafe");
    CHECK(home >= 0);
    CHECK(rows[static_cast<size_t>(home)].connected);
    CHECK(rows[static_cast<size_t>(home)].text.find(L'*') != std::wstring::npos);  // marked in text, not colour alone
    CHECK(rows[static_cast<size_t>(home)].detail.find(L"connected") != std::wstring::npos);
    CHECK(!rows[static_cast<size_t>(cafe)].connected);
    CHECK(rows[static_cast<size_t>(cafe)].text.find(L'*') == std::wstring::npos);
}

TEST_CASE(BuildRows_WifiReasonsAreStated_NET006) {
    for (WifiStatus st : {WifiStatus::NoAdapter, WifiStatus::ServiceStopped, WifiStatus::RadioOff,
                          WifiStatus::LocationBlocked, WifiStatus::NoNetworksFound, WifiStatus::Failed}) {
        Snapshot s = Sample();
        s.wifi.clear();
        s.wifiStatus = st;
        std::vector<Row> rows = BuildRows(s, {});
        int wifi = FindRow(rows, RowKind::Heading, L"Wi-Fi");
        CHECK(rows[static_cast<size_t>(wifi) + 1].kind == RowKind::Text);
        CHECK(rows[static_cast<size_t>(wifi) + 1].text.size() > 6);
    }
    Snapshot s = Sample();
    s.wifi.clear();
    s.wifiStatus = WifiStatus::LocationBlocked;
    std::vector<Row> rows = BuildRows(s, {});
    int line = FindRow(rows, RowKind::Text, L"Location");
    CHECK(line >= 0);
    CHECK(rows[static_cast<size_t>(line)].warn);
}

TEST_CASE(BuildRows_ExpandedMachineShowsItsSharesOrWhyNot_NET007) {
    Snapshot s = Sample();
    s.machines[0].shares = LoadState::Loaded;
    s.machines[0].shareList = {Share(L"media", ShareType::Disk, L"Movies"), Share(L"IPC$", ShareType::Ipc, L"", L"", true),
                               Share(L"print", ShareType::Printer)};
    s.machines[1].shares = LoadState::Failed;
    s.machines[1].error = L"Access denied";
    BuildOptions o;
    o.expanded = {L"nas", L"laptop"};
    std::vector<Row> rows = BuildRows(s, o);
    int nas = FindRow(rows, RowKind::Machine, L"nas");
    CHECK(rows[static_cast<size_t>(nas)].text.find(L"[-]") != std::wstring::npos);
    int media = FindRow(rows, RowKind::MachineShare, L"media");
    CHECK(media == nas + 1);
    CHECK(rows[static_cast<size_t>(media)].index == 0);
    CHECK(rows[static_cast<size_t>(media)].share == 0);
    CHECK(FindRow(rows, RowKind::MachineShare, L"IPC$") < 0);  // administrative: hidden
    CHECK(FindRow(rows, RowKind::MachineShare, L"print") >= 0);
    int denied = FindRow(rows, RowKind::MachineNote, L"Access denied");
    CHECK(denied >= 0);
    CHECK(rows[static_cast<size_t>(denied)].warn);   // the error belongs to that machine's rows only
    CHECK(denied > FindRow(rows, RowKind::Machine, L"laptop"));
    // collapsed: no share rows at all
    CHECK(FindRow(BuildRows(s, {}), RowKind::MachineShare, L"media") < 0);
}

TEST_CASE(BuildRows_MachinesStatesWhileScanningAndWhenNoneAnswered_NET009) {
    Snapshot s = Sample();
    s.machines.clear();
    s.machinesDone = false;
    s.machinesScanning = true;
    CHECK(FindRow(BuildRows(s, {}), RowKind::Text, L"Searching") >= 0);
    s.machinesScanning = false;
    s.machinesDone = true;
    s.machinesNote = L"No machines answered.";
    std::vector<Row> rows = BuildRows(s, {});
    CHECK(FindRow(rows, RowKind::Text, L"No machines found") >= 0);
    CHECK(FindRow(rows, RowKind::Text, L"No machines answered") >= 0);
}

TEST_CASE(HandleKey_UpDownSkipHeadingsAndTextRows_NET001) {
    Snapshot s = Sample();
    std::vector<Row> rows = BuildRows(s, {});
    ViewState v;
    v.cursor = NearestSelectable(rows, 0);
    CHECK(rows[static_cast<size_t>(v.cursor)].kind == RowKind::LocalShare);
    std::vector<int> visited;
    for (int i = 0; i < 20; ++i) {
        HandleKey(rows, v, VK_DOWN, 30);
        CHECK(rows[static_cast<size_t>(v.cursor)].selectable);
        visited.push_back(v.cursor);
    }
    CHECK(rows[static_cast<size_t>(v.cursor)].kind == RowKind::Machine);  // stopped on the last selectable row
    int last = v.cursor;
    HandleKey(rows, v, VK_DOWN, 30);
    CHECK(v.cursor == last);
    HandleKey(rows, v, VK_HOME, 30);
    CHECK(v.cursor == NearestSelectable(rows, 0));
    HandleKey(rows, v, VK_UP, 30);
    CHECK(v.cursor == NearestSelectable(rows, 0));
    HandleKey(rows, v, VK_END, 30);
    CHECK(v.cursor == last);
}

TEST_CASE(HandleKey_EscCloses_F5Rescans_NET004) {
    std::vector<Row> rows = BuildRows(Sample(), {});
    ViewState v;
    CHECK(HandleKey(rows, v, VK_ESCAPE, 20).kind == ActionKind::Close);
    CHECK(HandleKey(rows, v, VK_F5, 20).kind == ActionKind::Rescan);
    CHECK(HandleKey(rows, v, 'Q', 20).kind == ActionKind::None);
}

TEST_CASE(HandleKey_EnterDependsOnTheRow_NET007_NET008) {
    Snapshot s = Sample();
    s.machines[0].shares = LoadState::Loaded;
    s.machines[0].shareList = {Share(L"media")};
    BuildOptions o;
    o.expanded = {L"nas"};
    std::vector<Row> rows = BuildRows(s, o);
    ViewState v;
    v.expanded = o.expanded;
    v.cursor = FindRow(rows, RowKind::Machine, L"nas");
    CHECK(HandleKey(rows, v, VK_RETURN, 20).kind == ActionKind::ToggleMachine);
    v.cursor = FindRow(rows, RowKind::MachineShare, L"media");
    CHECK(HandleKey(rows, v, VK_RETURN, 20).kind == ActionKind::OpenShare);
    v.cursor = FindRow(rows, RowKind::LocalShare, L"Pub");
    CHECK(HandleKey(rows, v, VK_RETURN, 20).kind == ActionKind::OpenShare);
    v.cursor = FindRow(rows, RowKind::MappedDrive, L"Z:");
    CHECK(HandleKey(rows, v, VK_RETURN, 20).kind == ActionKind::OpenShare);
    v.cursor = FindRow(rows, RowKind::Wifi, L"Cafe");
    CHECK(HandleKey(rows, v, VK_RETURN, 20).kind == ActionKind::Details);
    CHECK(HandleKey(rows, v, VK_SPACE, 20).kind == ActionKind::Details);
}

TEST_CASE(HandleKey_RightExpandsLeftCollapsesAndGoesBack_NET007) {
    Snapshot s = Sample();
    s.machines[0].shares = LoadState::Loaded;
    s.machines[0].shareList = {Share(L"media")};
    std::vector<Row> rows = BuildRows(s, {});
    ViewState v;
    v.cursor = FindRow(rows, RowKind::Machine, L"nas");
    CHECK(HandleKey(rows, v, VK_RIGHT, 20).kind == ActionKind::ToggleMachine);  // collapsed: expand
    CHECK(HandleKey(rows, v, VK_LEFT, 20).kind == ActionKind::None);            // collapsed: nothing to collapse

    BuildOptions o;
    o.expanded = {L"nas"};
    rows = BuildRows(s, o);
    v.expanded = o.expanded;
    v.cursor = FindRow(rows, RowKind::Machine, L"nas");
    CHECK(HandleKey(rows, v, VK_LEFT, 20).kind == ActionKind::ToggleMachine);   // expanded: collapse
    HandleKey(rows, v, VK_RIGHT, 20);                                           // expanded: step into the shares
    CHECK(rows[static_cast<size_t>(v.cursor)].kind == RowKind::MachineShare);
    HandleKey(rows, v, VK_LEFT, 20);                                            // from a share: back to the machine
    CHECK(rows[static_cast<size_t>(v.cursor)].kind == RowKind::Machine);
}

TEST_CASE(HandleKey_PageKeysMoveByAScreenAndLandOnARow_NET001) {
    Snapshot s = Sample();
    s.machines.clear();
    for (int i = 0; i < 60; ++i) s.machines.push_back(MakeMachine(L"host" + std::to_wstring(100 + i), L"10.0.0." + std::to_wstring(i + 1)));
    std::vector<Row> rows = BuildRows(s, {});
    ViewState v;
    v.cursor = NearestSelectable(rows, 0);
    HandleKey(rows, v, VK_NEXT, 10);
    CHECK(v.cursor >= 10);
    CHECK(rows[static_cast<size_t>(v.cursor)].selectable);
    CHECK(v.cursor >= v.top);
    CHECK(v.cursor < v.top + 10);
    HandleKey(rows, v, VK_END, 10);
    CHECK(v.cursor == static_cast<int>(rows.size()) - 1);
    HandleKey(rows, v, VK_PRIOR, 10);
    CHECK(v.cursor < static_cast<int>(rows.size()) - 1);
    HandleKey(rows, v, VK_HOME, 10);
    CHECK(v.top == 0);
}

TEST_CASE(HandleClick_SelectsARowAndDoubleClickActs_NET001) {
    std::vector<Row> rows = BuildRows(Sample(), {});
    ViewState v;
    int pub = FindRow(rows, RowKind::LocalShare, L"Pub");
    Action single = HandleClick(rows, v, pub, false, 40);
    CHECK(single.kind == ActionKind::None);
    CHECK(v.cursor == pub);
    Action dbl = HandleClick(rows, v, pub, true, 40);
    CHECK(dbl.kind == ActionKind::OpenShare);
    CHECK(dbl.rowIndex == pub);
    // a heading, and a click below the last row, do nothing
    ViewState w;
    w.cursor = pub;
    CHECK(HandleClick(rows, w, 0, true, 40).kind == ActionKind::None);
    CHECK(w.cursor == pub);
    CHECK(HandleClick(rows, w, 39, true, 40).kind == ActionKind::None);
    CHECK(HandleClick(rows, w, -1, true, 40).kind == ActionKind::None);
}

TEST_CASE(RestoreCursor_StaysOnTheSameRowWhenRowsChange_NET009) {
    Snapshot s = Sample();
    std::vector<Row> oldRows = BuildRows(s, {});
    int cafe = FindRow(oldRows, RowKind::Wifi, L"Cafe");
    s.wifi.insert(s.wifi.begin(), Wifi(L"Aaa", 99));  // a new network above shifts every index
    s.wifi[1].connected = true;
    std::vector<Row> newRows = BuildRows(s, {});
    int now = RestoreCursor(oldRows, cafe, newRows);
    CHECK(newRows[static_cast<size_t>(now)].text.find(L"Cafe") != std::wstring::npos);
    // the row vanished: the nearest selectable one
    s.wifi.clear();
    std::vector<Row> gone = BuildRows(s, {});
    int closest = RestoreCursor(oldRows, cafe, gone);
    CHECK(gone[static_cast<size_t>(closest)].selectable);
}

TEST_CASE(RenderPanel_EveryLineIsExactlyTheWidth_AndTheStatusRowIsLast_NET001) {
    std::vector<Row> rows = BuildRows(Sample(), {});
    ViewState v;
    v.cursor = NearestSelectable(rows, 0);
    for (int width : {20, 40, 80}) {
        std::vector<DrawLine> lines = RenderPanel(rows, v, width, 24, true, L"Ready");
        CHECK(lines.size() == 24);
        for (const DrawLine& l : lines) CHECK(mc::StringDisplayWidth(l.text) == width);
        CHECK(lines.back().y == 23);
        CHECK(lines.back().text.find(L"Ready") != std::wstring::npos);
    }
}

TEST_CASE(RenderPanel_DetailIsRightAlignedAndCursorHighlighted_NET001) {
    std::vector<Row> rows = BuildRows(Sample(), {});
    ViewState v;
    v.cursor = FindRow(rows, RowKind::Machine, L"nas");
    std::vector<DrawLine> lines = RenderPanel(rows, v, 60, 30, true, L"");
    const DrawLine& nas = lines[static_cast<size_t>(v.cursor)];
    CHECK(nas.color == DrawColor::Highlight);
    CHECK(nas.text.find(L"192.168.1.5") == static_cast<size_t>(60 - 11));  // flush with the right edge
    CHECK(lines[0].color == DrawColor::Section);        // a heading
    std::vector<DrawLine> inactive = RenderPanel(rows, v, 60, 30, false, L"");
    CHECK(inactive[static_cast<size_t>(v.cursor)].color == DrawColor::Selected);
    int home = FindRow(rows, RowKind::Wifi, L"Home");
    CHECK(lines[static_cast<size_t>(home)].color == DrawColor::Selected);  // the connected network, off the cursor
}

TEST_CASE(RenderPanel_WideCharactersNeverBreakTheColumns_UI011) {
    Snapshot s = Sample();
    s.wifi = {Wifi(L"\x30CD\x30C3\x30C8\x30EF\x30FC\x30AF", 70)};  // katakana: 2 columns each
    std::vector<Row> rows = BuildRows(s, {});
    ViewState v;
    for (int width : {24, 31, 50}) {
        for (const DrawLine& l : RenderPanel(rows, v, width, 30, true, L"")) CHECK(mc::StringDisplayWidth(l.text) == width);
    }
}

TEST_CASE(RenderPanel_TinyAreasDoNotCrash_NET001) {
    std::vector<Row> rows = BuildRows(Sample(), {});
    ViewState v;
    CHECK(RenderPanel(rows, v, 0, 10, true, L"").empty());
    CHECK(RenderPanel(rows, v, 10, 0, true, L"").empty());
    CHECK(RenderPanel(rows, v, 1, 1, true, L"x").size() == 1);
    CHECK(RenderPanel({}, v, 30, 5, true, L"x").size() == 5);
}

TEST_CASE(EnsureVisible_KeepsTheCursorOnScreen_NET001) {
    Snapshot s = Sample();
    s.machines.clear();
    for (int i = 0; i < 50; ++i) s.machines.push_back(MakeMachine(L"h" + std::to_wstring(100 + i), L"10.0.0." + std::to_wstring(i + 1)));
    std::vector<Row> rows = BuildRows(s, {});
    ViewState v;
    v.cursor = static_cast<int>(rows.size()) - 1;
    EnsureVisible(rows, v, 10);
    CHECK(v.cursor >= v.top);
    CHECK(v.cursor < v.top + 10);
    v.cursor = NearestSelectable(rows, 0);
    EnsureVisible(rows, v, 10);
    CHECK(v.top == 0);  // the first row brings its headings back into view
}

TEST_CASE(DetailLines_DescribeTheRow_NET001) {
    Snapshot s = Sample();
    s.machines[0].shares = LoadState::Loaded;
    s.machines[0].shareList = {Share(L"media", ShareType::Disk, L"Movies")};
    BuildOptions o;
    o.expanded = {L"nas"};
    std::vector<Row> rows = BuildRows(s, o);
    auto join = [](const std::vector<std::wstring>& lines) {
        std::wstring all;
        for (const auto& l : lines) all += l + L"\n";
        return all;
    };
    std::wstring wifi = join(DetailLines(s, rows[static_cast<size_t>(FindRow(rows, RowKind::Wifi, L"Home"))]));
    CHECK(wifi.find(L"Home") != std::wstring::npos);
    CHECK(wifi.find(L"connected") != std::wstring::npos);
    std::wstring machine = join(DetailLines(s, rows[static_cast<size_t>(FindRow(rows, RowKind::Machine, L"nas"))]));
    CHECK(machine.find(L"192.168.1.5") != std::wstring::npos);
    std::wstring share = join(DetailLines(s, rows[static_cast<size_t>(FindRow(rows, RowKind::MachineShare, L"media"))]));
    CHECK(share.find(L"\\\\nas\\media") != std::wstring::npos);
    CHECK(DetailLines(s, rows[0]).empty());  // a heading has no details
}

TEST_CASE(DefaultStatusText_ReflectsScanning_NET009) {
    Snapshot s = Sample();
    CHECK(DefaultStatusText(s).find(L"Esc") != std::wstring::npos);
    s.machinesScanning = true;
    CHECK(DefaultStatusText(s).find(L"Scanning") != std::wstring::npos);
    CHECK(DefaultStatusText(s).find(L"machines") != std::wstring::npos);
    s.wifiStatus = WifiStatus::Scanning;
    CHECK(DefaultStatusText(s).find(L"Wi-Fi") != std::wstring::npos);
}

TEST_CASE(RealSources_SmokeTest_ReadsWithoutCrashingOrChanging_NET010) {
    // Opt-in: reads this machine's real shares, Wi-Fi state and neighbour table. Read-only.
    wchar_t flag[8] = {};
    if (GetEnvironmentVariableW(L"MC_NETINFO_TEST", flag, 8) == 0) return;
    std::shared_ptr<INetInfoSource> src = MakeWindowsSource();
    auto never = [] { return false; };
    LocalInfo local = src->ReadLocal();
    CHECK(!local.computerName.empty());
    WifiResult wifi = src->ReadWifi(never);
    CHECK(wifi.status != WifiStatus::NotScanned);
    MachineScanResult machines = src->FindMachines(never);
    for (const Machine& m : machines.machines) CHECK(!m.name.empty());
}
