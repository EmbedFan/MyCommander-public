// EXT-004 (D2): the shared remote-server panel -- see RemotePanel.h.
// Extracted from the FTP plugin (EXT-005, FTP-001..009); the SFTP plugin
// (SFTP-001..009) uses the same code with its own PanelProtocol.

#include "RemotePanel.h"

#include "../MyCommander/TextWidth.h"
#include "RemoteUi.h"

#include <algorithm>
#include <cwctype>
#include <numeric>

namespace remote {

namespace {

const McPluginHost* g_host = nullptr;
PanelProtocol g_protocol;
constexpr int kNameBuffer = 512;

const PanelProtocol& P() { return g_protocol; }

}  // namespace

// ---------------------------------------------------------------------------
// Host helpers

const McPluginHost* Host() { return g_host; }

void Log(McLogLevel level, const std::wstring& message) {
    if (g_host) g_host->Log(level, (P().name + L": " + message).c_str());
}

void Message(const std::wstring& title, const std::vector<std::wstring>& lines) {
    std::vector<const wchar_t*> ptrs(lines.size());
    std::transform(lines.begin(), lines.end(), ptrs.begin(), [](const std::wstring& l) { return l.c_str(); });
    g_host->ShowMessage(title.c_str(), ptrs.data(), static_cast<int>(ptrs.size()));
}

int Choose(const std::wstring& title, const std::vector<std::wstring>& lines, const std::vector<McDialogOption>& options) {
    std::vector<const wchar_t*> ptrs(lines.size());
    std::transform(lines.begin(), lines.end(), ptrs.begin(), [](const std::wstring& l) { return l.c_str(); });
    return g_host->ShowChoice(title.c_str(), ptrs.data(), static_cast<int>(ptrs.size()), options.data(),
                              static_cast<int>(options.size()));
}

bool Cancelled() { return g_host && g_host->PollCancel && g_host->PollCancel(); }

namespace {

bool Confirm(const std::wstring& title, const std::vector<std::wstring>& lines) {
    return Choose(title, lines, {{L'Y', L"Yes"}, {L'N', L"No"}}) == 0;
}

std::optional<std::wstring> Prompt(const std::wstring& title, const std::wstring& initial) {
    std::wstring buffer(kNameBuffer, L'\0');
    if (!g_host->ShowTextPrompt(title.c_str(), initial.c_str(), buffer.data(), kNameBuffer)) return std::nullopt;
    buffer.resize(wcslen(buffer.c_str()));
    return buffer;
}

class Progress {
    bool open_ = false;

public:
    Progress(const std::wstring& title, uint64_t total) {
        if (g_host->BeginProgress && g_host->UpdateProgress && g_host->EndProgress)
            open_ = g_host->BeginProgress(title.c_str(), total);
    }
    ~Progress() {
        if (open_) g_host->EndProgress();
    }
    Progress(const Progress&) = delete;
    Progress& operator=(const Progress&) = delete;
    void Update(uint64_t done, uint64_t total, const std::wstring& message) const {
        if (open_) g_host->UpdateProgress(done, total, message.c_str());
    }
};

std::wstring JoinLocal(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    return (dir.back() == L'\\' || dir.back() == L'/') ? dir + name : dir + L"\\" + name;
}

std::wstring PanelPath(McPanelSide side) {
    std::wstring buffer(32768, L'\0');
    g_host->GetPanelPath(side, buffer.data(), static_cast<int>(buffer.size()));
    buffer.resize(wcslen(buffer.c_str()));
    return buffer;
}

std::vector<std::wstring> PanelSelection(McPanelSide side) {
    int count = g_host->GetPanelSelectedNames(side, nullptr, 0, 0);
    if (count <= 0) return {};
    std::vector<std::wstring> buffers(static_cast<size_t>(count), std::wstring(1024, L'\0'));
    std::vector<wchar_t*> ptrs(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) ptrs[static_cast<size_t>(i)] = buffers[static_cast<size_t>(i)].data();
    int actual = g_host->GetPanelSelectedNames(side, ptrs.data(), count, 1024);
    std::vector<std::wstring> names;
    for (int i = 0; i < std::min(actual, count); ++i) names.emplace_back(ptrs[static_cast<size_t>(i)]);
    return names;
}

McPanelSide Other(McPanelSide side) { return side == kMcPanelLeft ? kMcPanelRight : kMcPanelLeft; }

std::wstring Title(const std::wstring& user, const std::wstring& host, uint16_t port, const std::wstring& path) {
    return FormatPanelTitle(P().scheme, P().store->defaultPort, user, host, port, path);
}

// ---------------------------------------------------------------------------
// One connected server shown in one panel.

std::unique_ptr<ServerView> g_views[2];

}  // namespace

ServerView* ViewFor(McPanelSide side) {
    auto& v = g_views[side == kMcPanelLeft ? 0 : 1];
    return v && v->open ? v.get() : nullptr;
}

namespace {

// Rows as shown: ".." first (always, even at the root -- it disconnects), then entries.
int RowCount(const ServerView& view) { return static_cast<int>(view.entries.size()) + 1; }
const RemoteEntry* EntryAtRow(const ServerView& view, int row) {
    return row >= 1 && row <= static_cast<int>(view.entries.size()) ? &view.entries[static_cast<size_t>(row - 1)] : nullptr;
}

void ClampCursor(ServerView& view) {
    int count = RowCount(view);
    view.cursor = std::clamp(view.cursor, 0, count - 1);
    if (view.cursor < view.topIndex) view.topIndex = view.cursor;
    if (view.cursor >= view.topIndex + view.visibleRows) view.topIndex = view.cursor - view.visibleRows + 1;
    view.topIndex = std::clamp(view.topIndex, 0, std::max(0, count - view.visibleRows));
}

}  // namespace

// Network > "Disconnect <name>" is offered only while a panel is connected
// (the host hides a disabled plugin menu item).
void UpdateDisconnectItem() {
    bool connected = (g_views[0] && g_views[0]->open) || (g_views[1] && g_views[1]->open);
    if (g_host && g_host->SetMenuItemEnabled) g_host->SetMenuItemEnabled(P().disconnectCommandId, connected);
}

namespace {

void CloseView(ServerView& view) {
    McPanelSide side = view.side;
    view.client->Disconnect();
    g_host->ReleasePanelContent(side);
    Log(kMcLogInfo, L"disconnected from " + view.connection.host);
    g_views[side == kMcPanelLeft ? 0 : 1].reset();
    UpdateDisconnectItem();
}

// Network > Disconnect <name>: the active panel's server, else the other panel's.
void DisconnectFromMenu() {
    McPanelSide active = g_host->GetActiveSide();
    ServerView* view = ViewFor(active);
    if (!view) view = ViewFor(Other(active));
    if (view) CloseView(*view);
}

// FTP-009 / SFTP-008: a lost connection offers to reconnect (and stay where we were).
bool HandleLost(ServerView& view, const std::wstring& error) {
    Log(kMcLogWarning, L"connection lost: " + error);
    int choice = Choose(P().name + L" connection lost", {view.connection.host + L":", error},
                        {{L'R', L"Reconnect"}, {L'C', L"Close"}});
    if (choice == 0) {
        Progress progress(L"Reconnecting", 0);
        progress.Update(0, 0, view.connection.host);
        std::wstring connectError;
        if (view.client->Connect(view.connection, view.password, Cancelled, connectError) == Status::Ok) return true;
        Message(P().name + L" connection", {L"Could not reconnect:", connectError});
    }
    CloseView(view);
    return false;
}

// Reports a non-Ok status. Returns false when the view was closed.
bool Report(ServerView& view, Status status, const std::wstring& title, const std::wstring& error) {
    if (status == Status::ConnectionLost) return HandleLost(view, error);
    if (status == Status::Failed) Message(title, {error});
    return true;
}

void SortEntries(std::vector<RemoteEntry>& entries) {
    std::stable_sort(entries.begin(), entries.end(), [](const RemoteEntry& a, const RemoteEntry& b) {
        if (a.isDirectory != b.isDirectory) return a.isDirectory;
        return _wcsicmp(a.name.c_str(), b.name.c_str()) < 0;
    });
}

// Lists `newPath`; on success it becomes the view's folder and the cursor
// goes on `selectName` (if present). Returns false if the view was closed.
bool LoadFolder(ServerView& view, const std::wstring& newPath, const std::wstring& selectName, bool reportErrors = true) {
    std::vector<RemoteEntry> entries;
    std::wstring error;
    Status s;
    {
        Progress progress(L"Reading folder", 0);
        progress.Update(0, 0, Title(view.connection.user, view.connection.host, view.connection.port, newPath));
        s = view.client->List(newPath, entries, Cancelled, error);
        if (s == Status::ConnectionLost) {
            // fall through to HandleLost below, outside the progress box
        }
    }
    if (s == Status::ConnectionLost) {
        if (!HandleLost(view, error)) return false;
        return LoadFolder(view, newPath, selectName, reportErrors);
    }
    if (s != Status::Ok) {
        if (reportErrors && s == Status::Failed) Message(P().name, {L"Cannot open " + newPath + L":", error});
        return true;
    }
    SortEntries(entries);
    bool sameFolder = newPath == view.path;
    view.path = newPath;
    view.entries = std::move(entries);
    if (!sameFolder) view.selected.clear();
    view.cursor = 0;
    for (size_t i = 0; i < view.entries.size(); ++i)
        if (view.entries[i].name == selectName) view.cursor = static_cast<int>(i) + 1;
    ClampCursor(view);
    return true;
}

std::wstring CursorName(const ServerView& view) {
    const RemoteEntry* e = EntryAtRow(view, view.cursor);
    return e ? e->name : L"";
}

// The selection, or the cursor entry.
std::vector<RemoteEntry> Targets(const ServerView& view) {
    std::vector<RemoteEntry> out;
    for (const auto& e : view.entries)
        if (view.selected.count(e.name)) out.push_back(e);
    if (out.empty()) {
        if (const RemoteEntry* e = EntryAtRow(view, view.cursor)) out.push_back(*e);
    }
    return out;
}

// ---------------------------------------------------------------------------
// Connecting (FTP-001/006/007, SFTP-001/005/006/009)

std::wstring DisplayHost(const Connection& c) {
    return Title(c.user, c.host, c.port, L"");
}

void SaveConnection(std::vector<Connection>& list, const Connection& c, const std::wstring& oldName,
                    const std::wstring& password);

bool ConnectionIsSaved(const Connection& c) {
    std::vector<std::wstring> ignored;
    for (const auto& x : LoadConnections(*P().store, ignored))
        if (_wcsicmp(x.name.c_str(), c.name.c_str()) == 0) return true;
    return false;
}

// The login password/passphrase prompt. Like the connection form, it always
// offers to keep it in the Windows Credential Manager (FTP-006, SFTP-006,
// DEC-013):
// ticking it stores the password and records the connection with
// save-password = yes (saving the connection too if it wasn't saved yet --
// a stored password needs a saved connection to belong to); unticking it
// for a connection that had one forgets the stored password.
bool AskPassword(Connection& c, std::wstring& password) {
    std::vector<wchar_t> buffer(256, L'\0');
    std::wstring label = P().secretLabel(c);
    McFormField fields[2]{};
    fields[0].structSize = sizeof(McFormField);
    fields[0].kind = kMcFormFieldPassword;
    fields[0].label = label.c_str();
    fields[0].textBuffer = buffer.data();
    fields[0].textBufferSize = static_cast<int32_t>(buffer.size());
    fields[1].structSize = sizeof(McFormField);
    fields[1].kind = kMcFormFieldCheckbox;
    fields[1].label = L"Save the password (Windows Credential Manager)";
    fields[1].boolValue = c.savePassword;
    std::wstring title = DisplayHost(c) + P().loginTitleNote;
    bool ok = g_host->ShowForm(title.c_str(), fields, 2);
    password = buffer.data();
    SecureZeroMemory(buffer.data(), buffer.size() * sizeof(wchar_t));
    if (!ok) return false;

    bool save = fields[1].boolValue;
    if (save && !password.empty()) {
        c.savePassword = true;
        std::vector<std::wstring> ignored;
        auto list = LoadConnections(*P().store, ignored);
        SaveConnection(list, c, L"", password);
    } else if (!save && c.savePassword) {
        c.savePassword = false;
        if (ConnectionIsSaved(c)) {
            std::vector<std::wstring> ignored;
            auto list = LoadConnections(*P().store, ignored);
            SaveConnection(list, c, L"", L"");  // forgets the stored password
        } else {
            ForgetPassword(*P().store, c.name);
        }
    }
    return true;
}

void OpenServer(const Connection& connection, std::wstring password) {
    McPanelSide side = g_host->GetActiveSide();
    if (ViewFor(side)) {
        Message(P().name + L" connection", {L"This panel is already connected to " + ViewFor(side)->connection.host + L".",
                                    L"Press Backspace at its root to disconnect first."});
        return;
    }
    auto view = std::make_unique<ServerView>();
    view->side = side;
    view->connection = connection;
    view->password = std::move(password);
    view->client = P().makeClient();
    view->client->SetTrace([](const std::wstring& line) { Log(kMcLogDebug, line); });  // never a secret
    std::wstring error;
    Status s;
    {
        Progress progress(L"Connecting", 0);
        progress.Update(0, 0, DisplayHost(connection));
        s = view->client->Connect(view->connection, view->password, Cancelled, error);
    }
    if (s != Status::Ok) {
        if (s != Status::Cancelled) Message(P().name + L" connection", {L"Could not connect to " + DisplayHost(connection) + L":", error});
        return;
    }
    std::wstring start = connection.folder;
    if (start.empty() && view->client->HomeDirectory(start, Cancelled, error) != Status::Ok) start = L"/";
    std::wstring title = P().name + L" " + connection.host;
    if (!g_host->RequestPanelContent(side, title.c_str())) {
        Message(P().name + L" connection", {L"The active panel is showing other content (an archive or the tree)."});
        view->client->Disconnect();
        return;
    }
    view->open = true;
    ServerView& ref = *view;
    g_views[side == kMcPanelLeft ? 0 : 1] = std::move(view);
    UpdateDisconnectItem();
    Log(kMcLogInfo, L"connected to " + DisplayHost(connection));
    LoadFolder(ref, start, L"");
}

void ConnectSaved(Connection c) {
    std::wstring password;
    if (c.savePassword) {
        if (auto stored = ReadPassword(*P().store, c.name)) password = *stored;
    }
    if (password.empty() && P().needsSecret(c) && !AskPassword(c, password)) return;
    OpenServer(c, std::move(password));
}

void SaveConnection(std::vector<Connection>& list, const Connection& c, const std::wstring& oldName,
                    const std::wstring& password) {
    auto it = std::find_if(list.begin(), list.end(), [&](const Connection& x) {
        return _wcsicmp(x.name.c_str(), (oldName.empty() ? c.name : oldName).c_str()) == 0;
    });
    if (it != list.end()) *it = c;
    else list.push_back(c);
    if (!oldName.empty() && _wcsicmp(oldName.c_str(), c.name.c_str()) != 0) ForgetPassword(*P().store, oldName);
    if (c.savePassword && !password.empty()) {
        if (!StorePassword(*P().store, c.name, c.user, password))
            Message(P().name + L" connection", {L"The password could not be saved in the Windows Credential Manager."});
    } else if (!c.savePassword) {
        ForgetPassword(*P().store, c.name);
    }
    if (!SaveConnections(*P().store, list)) Message(P().name + L" connection", {L"Could not write " + ConnectionsFilePath(*P().store)});
}

void NewConnection() {
    Connection c = remote::NewConnection(*P().store);
    std::wstring password;
    bool save = true;
    if (!P().editConnection(L"New " + P().name + L" connection", c, password, save)) return;
    if (save) {
        std::vector<std::wstring> warnings;
        auto list = LoadConnections(*P().store, warnings);
        SaveConnection(list, c, L"", password);
    }
    if (password.empty() && P().needsSecret(c) && !AskPassword(c, password)) return;
    OpenServer(c, std::move(password));
}

// Network > "<name> connection": pick a saved connection and what to do with it.
void ConnectionManager() {
    std::vector<std::wstring> warnings;
    auto list = LoadConnections(*P().store, warnings);
    for (const auto& w : warnings) Log(kMcLogWarning, w);
    if (!warnings.empty()) {
        std::vector<std::wstring> lines{L"Some lines in " + std::wstring(P().store->fileName) + L" were ignored:"};
        lines.insert(lines.end(), warnings.begin(), warnings.begin() + std::min<size_t>(warnings.size(), 6));
        Message(P().name + L" connections", lines);
    }
    if (list.empty()) {
        NewConnection();
        return;
    }
    std::vector<std::wstring> names(list.size());
    std::transform(list.begin(), list.end(), names.begin(),
                   [](const Connection& c) { return c.name + L"  (" + DisplayHost(c) + L")"; });
    std::vector<const wchar_t*> namePtrs(names.size());
    std::transform(names.begin(), names.end(), namePtrs.begin(), [](const std::wstring& n) { return n.c_str(); });
    static const wchar_t* kActions[] = {L"Connect", L"Edit", L"Delete", L"New connection"};
    McFormField f[2]{};
    f[0].structSize = f[1].structSize = sizeof(McFormField);
    f[0].kind = f[1].kind = kMcFormFieldChoice;
    f[0].label = L"Connection";
    f[0].choices = namePtrs.data();
    f[0].choiceCount = static_cast<int32_t>(namePtrs.size());
    f[1].label = L"Action";
    f[1].choices = kActions;
    f[1].choiceCount = 4;
    std::wstring formTitle = P().name + L" connections";
    if (!g_host->ShowForm(formTitle.c_str(), f, 2)) return;
    size_t index = static_cast<size_t>(std::clamp<int32_t>(f[0].choiceIndex, 0, static_cast<int32_t>(list.size()) - 1));
    Connection chosen = list[index];
    switch (f[1].choiceIndex) {
        case 0:
            ConnectSaved(chosen);
            break;
        case 1: {
            std::wstring oldName = chosen.name, password;
            bool save = true;
            if (!P().editConnection(L"Edit " + P().name + L" connection", chosen, password, save)) return;
            if (password.empty() && chosen.savePassword) password = ReadPassword(*P().store, oldName).value_or(L"");
            SaveConnection(list, chosen, oldName, password);
            break;
        }
        case 2:
            if (Confirm(P().name + L" connections", {L"Delete the saved connection \"" + chosen.name + L"\"?"})) {
                list.erase(list.begin() + static_cast<std::ptrdiff_t>(index));
                ForgetPassword(*P().store, chosen.name);
                if (!SaveConnections(*P().store, list)) Message(P().name + L" connections", {L"Could not write " + ConnectionsFilePath(*P().store)});
            }
            break;
        default:
            NewConnection();
            break;
    }
}

// ---------------------------------------------------------------------------
// Transfers (FTP-003/004, SFTP-003)

enum class Blanket { None, OverwriteAll, SkipAll };
enum class ConflictAnswer { Overwrite, Skip, Rename, Cancel };

ConflictAnswer AskConflict(const std::wstring& title, const std::wstring& what, Blanket& blanket, std::wstring& renameTo) {
    if (blanket == Blanket::OverwriteAll) return ConflictAnswer::Overwrite;
    if (blanket == Blanket::SkipAll) return ConflictAnswer::Skip;
    int choice = Choose(title, {L"Already exists:", what},
                        {{L'O', L"Overwrite"}, {L'A', L"Overwrite all"}, {L'S', L"Skip"}, {L'K', L"Skip all"},
                         {L'R', L"Rename"}, {L'C', L"Cancel"}});
    switch (choice) {
        case 0: return ConflictAnswer::Overwrite;
        case 1: blanket = Blanket::OverwriteAll; return ConflictAnswer::Overwrite;
        case 2: return ConflictAnswer::Skip;
        case 3: blanket = Blanket::SkipAll; return ConflictAnswer::Skip;
        case 4: {
            auto name = Prompt(L"New name", renameTo);
            if (!name || name->empty()) return ConflictAnswer::Cancel;
            renameTo = *name;
            return ConflictAnswer::Rename;
        }
        default: return ConflictAnswer::Cancel;
    }
}

struct RemoteItem {
    std::wstring remotePath;    // full server path
    std::wstring relativePath;  // below the selection's folder, with '\'
    bool isDirectory = false;
    uint64_t size = 0;
};

// Expands the selection into every folder and file below it (folders first,
// parents before children). False = cancelled, connection lost (view may be
// closed) or a listing failed.
bool CollectRemote(ServerView& view, const std::vector<RemoteEntry>& top, std::vector<RemoteItem>& out,
                   const Progress& progress) {
    // A symbolic link to a folder is never descended into (it could loop or
    // leave the tree, the SEC-006 rule for local junctions); it's skipped.
    auto linkedFolder = [&](const RemoteEntry& e, const std::wstring& remotePath) {
        if (!(e.isDirectory && e.isLink)) return false;
        Log(kMcLogWarning, L"skipped the linked folder " + remotePath);
        return true;
    };
    for (const auto& e : top) {
        std::wstring remotePath = JoinRemotePath(view.path, e.name);
        if (!linkedFolder(e, remotePath)) out.push_back({remotePath, e.name, e.isDirectory, e.size});
    }
    for (size_t i = 0; i < out.size(); ++i) {
        if (!out[i].isDirectory) continue;
        if (Cancelled()) return false;
        progress.Update(0, 0, L"Scanning " + out[i].remotePath);
        std::vector<RemoteEntry> children;
        std::wstring error;
        Status s = view.client->List(out[i].remotePath, children, Cancelled, error);
        if (s != Status::Ok) {
            if (s == Status::Failed) Message(P().name, {L"Cannot read " + out[i].remotePath + L":", error});
            else if (s == Status::ConnectionLost) HandleLost(view, error);
            return false;
        }
        std::wstring parentRemote = out[i].remotePath, parentRelative = out[i].relativePath;
        for (const auto& c : children) {
            std::wstring remotePath = JoinRemotePath(parentRemote, c.name);
            if (!linkedFolder(c, remotePath)) out.push_back({remotePath, parentRelative + L"\\" + c.name, c.isDirectory, c.size});
        }
    }
    return true;
}

// F5 / F6 from the server to the other panel's folder.
void Download(ServerView& view, bool move) {
    std::vector<RemoteEntry> top = Targets(view);
    if (top.empty()) return;
    McPanelSide other = Other(view.side);
    if (ViewFor(other)) {
        Message(P().name, {L"Copying directly between two servers isn't supported."});
        return;
    }
    std::wstring dest = PanelPath(other);
    if (move && !Confirm(L"Move from server", {L"Download " + std::to_wstring(top.size()) + L" item(s) to " + dest + L",",
                                               L"then delete them on the server?"}))
        return;
    std::vector<RemoteItem> items;
    {
        Progress scan(move ? L"Moving from server" : L"Copying from server", 0);
        if (!CollectRemote(view, top, items, scan)) {
            g_host->RefreshPanel(other);
            return;
        }
    }
    const uint64_t total = std::accumulate(items.begin(), items.end(), uint64_t{0},
                                           [](uint64_t sum, const RemoteItem& item) { return sum + item.size; });
    Progress progress(move ? L"Moving from server" : L"Copying from server", total);
    Blanket blanket = Blanket::None;
    uint64_t doneBefore = 0;
    int copied = 0;
    std::set<std::wstring> downloaded;  // remote paths fully copied (for F6)
    bool stop = false;
    for (const auto& item : items) {
        if (stop || Cancelled()) break;
        std::wstring target = JoinLocal(dest, item.relativePath);
        if (item.isDirectory) {
            CreateDirectoryW(target.c_str(), nullptr);
            downloaded.insert(item.remotePath);
            continue;
        }
        if (GetFileAttributesW(target.c_str()) != INVALID_FILE_ATTRIBUTES) {
            std::wstring renameTo = item.relativePath.substr(item.relativePath.find_last_of(L'\\') + 1);
            ConflictAnswer answer = AskConflict(L"Copy from server", target, blanket, renameTo);
            if (answer == ConflictAnswer::Cancel) break;
            if (answer == ConflictAnswer::Skip) {
                doneBefore += item.size;
                continue;
            }
            if (answer == ConflictAnswer::Rename) target = JoinLocal(target.substr(0, target.find_last_of(L'\\')), renameTo);
        }
        std::wstring error;
        Status status = Status::Ok;
        bool ok = DownloadReplacing(target, P().partSuffix, [&](const std::wstring& temp) {
            status = view.client->Download(item.remotePath, temp, item.size,
                                          [&](uint64_t done, uint64_t) {
                                              progress.Update(doneBefore + done, total, item.remotePath);
                                          },
                                          Cancelled, error);
            return status == Status::Ok;
        });
        doneBefore += item.size;
        if (ok) {
            ++copied;
            downloaded.insert(item.remotePath);
            continue;
        }
        if (status == Status::Cancelled) break;
        if (status == Status::ConnectionLost) {
            if (!HandleLost(view, error)) {
                g_host->RefreshPanel(other);
                return;
            }
            continue;
        }
        int choice = Choose(L"Copy from server", {item.remotePath + L":", error},
                            {{L'S', L"Skip"}, {L'C', L"Cancel"}});
        if (choice != 0) stop = true;
    }
    if (move) {
        // Delete on the server only what arrived intact: files first, then
        // folders deepest-first, and a folder only if all of its contents went.
        for (auto it = items.rbegin(); it != items.rend(); ++it) {
            if (!downloaded.count(it->remotePath)) continue;
            bool complete = true;
            if (it->isDirectory) {
                for (const auto& child : items)
                    if (child.remotePath.rfind(it->remotePath + L"/", 0) == 0 && !downloaded.count(child.remotePath))
                        complete = false;
            }
            if (!complete) continue;
            std::wstring error;
            Status s = it->isDirectory ? view.client->RemoveRemoteDirectory(it->remotePath, Cancelled, error)
                                            : view.client->DeleteRemoteFile(it->remotePath, Cancelled, error);
            if (s == Status::ConnectionLost && !HandleLost(view, error)) break;
            if (s == Status::Failed) Log(kMcLogWarning, L"could not delete " + it->remotePath + L": " + error);
        }
    }
    g_host->RefreshPanel(other);
    if (ServerView* still = ViewFor(other == kMcPanelLeft ? kMcPanelRight : kMcPanelLeft)) {
        if (copied > 0 || move) still->selected.clear();
        if (move) LoadFolder(*still, still->path, CursorName(*still));
    }
}

struct LocalItem {
    std::wstring localPath;
    std::wstring relativePath;  // with '/' for the server
    bool isDirectory = false;
    uint64_t size = 0;
};

void CollectLocal(const std::wstring& path, const std::wstring& relative, std::vector<LocalItem>& out) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &data)) return;
    bool dir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    bool reparse = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out.push_back({path, relative, dir,
                   dir ? 0 : ((static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow)});
    if (!dir || reparse) return;  // SEC-006: never follow a junction/symlink folder
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileW(JoinLocal(path, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        CollectLocal(JoinLocal(path, name), relative + L"/" + name, out);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// F5 in the other panel while this server is shown: upload there (FTP-004).
void Upload(ServerView& view, McPanelSide sourceSide) {
    std::vector<std::wstring> names = PanelSelection(sourceSide);
    if (names.empty()) {
        Message(L"Copy to server", {L"Nothing selected in the other panel."});
        return;
    }
    std::wstring sourceDir = PanelPath(sourceSide);
    std::vector<LocalItem> items;
    for (const auto& n : names) CollectLocal(JoinLocal(sourceDir, n), n, items);
    const uint64_t total = std::accumulate(items.begin(), items.end(), uint64_t{0},
                                           [](uint64_t sum, const LocalItem& item) { return sum + item.size; });
    Progress progress(L"Copying to server", total);
    std::set<std::wstring> existing;  // names already in the current server folder
    for (const auto& e : view.entries) existing.insert(e.name);
    Blanket blanket = Blanket::None;
    uint64_t doneBefore = 0;
    std::wstring lastUploaded;
    for (const auto& item : items) {
        if (Cancelled()) break;
        std::wstring remote = JoinRemotePath(view.path, item.relativePath);
        std::wstring error;
        if (item.isDirectory) {
            Status s = view.client->MakeRemoteDirectory(remote, Cancelled, error);  // "exists" is fine
            if (s == Status::ConnectionLost && !HandleLost(view, error)) return;
            continue;
        }
        bool topLevel = item.relativePath.find(L'/') == std::wstring::npos;
        if (topLevel && existing.count(item.relativePath)) {
            std::wstring renameTo = item.relativePath;
            ConflictAnswer answer = AskConflict(L"Copy to server", remote, blanket, renameTo);
            if (answer == ConflictAnswer::Cancel) break;
            if (answer == ConflictAnswer::Skip) {
                doneBefore += item.size;
                continue;
            }
            if (answer == ConflictAnswer::Rename) remote = JoinRemotePath(view.path, renameTo);
        }
        Status s = view.client->Upload(item.localPath, remote,
                                           [&](uint64_t done, uint64_t) { progress.Update(doneBefore + done, total, remote); },
                                           Cancelled, error);
        doneBefore += item.size;
        if (s == Status::Ok) {
            lastUploaded = remote.substr(remote.find_last_of(L'/') + 1);
            continue;
        }
        if (s == Status::Cancelled) {
            std::wstring ignored;
            view.client->DeleteRemoteFile(remote, {}, ignored);  // no partial file left behind (best effort)
            break;
        }
        if (s == Status::ConnectionLost) {
            if (!HandleLost(view, error)) return;
            continue;
        }
        if (Choose(L"Copy to server", {remote + L":", error}, {{L'S', L"Skip"}, {L'C', L"Cancel"}}) != 0) break;
    }
    LoadFolder(view, view.path, lastUploaded.empty() ? CursorName(view) : lastUploaded);
}

// ---------------------------------------------------------------------------
// Server-side operations (FTP-005, SFTP-004)

void ViewRemoteFile(ServerView& view) {
    const RemoteEntry* e = EntryAtRow(view, view.cursor);
    if (!e || e->isDirectory || !g_host->ShowFileViewer) return;
    wchar_t dir[MAX_PATH], temp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"mcf", 0, temp)) return;
    std::wstring remote = JoinRemotePath(view.path, e->name), error;
    uint64_t size = e->size;
    Status s;
    {
        Progress progress(L"Downloading for viewing", size);
        s = view.client->Download(remote, temp, size, [&](uint64_t d, uint64_t t) { progress.Update(d, t, remote); },
                                 Cancelled, error);
    }
    if (s == Status::Ok) g_host->ShowFileViewer(temp);
    DeleteFileW(temp);
    if (s != Status::Ok && s != Status::Cancelled) Report(view, s, L"View", error);
}

void MakeFolder(ServerView& view) {
    auto name = Prompt(L"Create folder on the server", L"");
    if (!name || name->empty()) return;
    std::wstring error;
    Status s = view.client->MakeRemoteDirectory(JoinRemotePath(view.path, *name), Cancelled, error);
    if (!Report(view, s, L"Create folder", error)) return;
    LoadFolder(view, view.path, *name);
}

void RenameEntry(ServerView& view) {
    const RemoteEntry* e = EntryAtRow(view, view.cursor);
    if (!e) return;
    std::wstring oldName = e->name;
    auto name = Prompt(L"Rename on the server", oldName);
    if (!name || name->empty() || *name == oldName) return;
    std::wstring error;
    Status s = view.client->RenameRemote(JoinRemotePath(view.path, oldName),
                                             name->find(L'/') == 0 ? *name : JoinRemotePath(view.path, *name),
                                             Cancelled, error);
    if (!Report(view, s, L"Rename", error)) return;
    LoadFolder(view, view.path, *name);
}

// Recursive delete: children first, then the folder itself. `isDirectory`
// is false for a symbolic link, even to a folder: the link itself is removed,
// never what it points to.
Status DeleteTree(ServerView& view, const std::wstring& remote, bool isDirectory, Progress& progress, int& count,
                       std::wstring& error) {
    if (Cancelled()) return Status::Cancelled;
    progress.Update(static_cast<uint64_t>(count), 0, remote);
    if (isDirectory) {
        std::vector<RemoteEntry> children;
        Status s = view.client->List(remote, children, Cancelled, error);
        if (s != Status::Ok) return s;
        for (const auto& c : children) {
            s = DeleteTree(view, JoinRemotePath(remote, c.name), c.isDirectory && !c.isLink, progress, count, error);
            if (s != Status::Ok) return s;
        }
        s = view.client->RemoveRemoteDirectory(remote, Cancelled, error);
        if (s == Status::Ok) ++count;
        return s;
    }
    Status s = view.client->DeleteRemoteFile(remote, Cancelled, error);
    if (s == Status::Ok) ++count;
    return s;
}

void DeleteEntries(ServerView& view) {
    std::vector<RemoteEntry> targets = Targets(view);
    if (targets.empty()) return;
    // Always confirmed: destructive, and there's no Recycle Bin on a server.
    std::wstring what = targets.size() == 1 ? L"\"" + targets[0].name + L"\"" + (targets[0].isDirectory && !targets[0].isLink ? L" and everything in it" : L"")
                                            : std::to_wstring(targets.size()) + L" items (folders with their contents)";
    if (!Confirm(L"Delete on server", {L"Permanently delete " + what + L" on " + view.connection.host + L"?",
                                       L"This cannot be undone."}))
        return;
    std::wstring error;
    Status s = Status::Ok;
    {
        int count = 0;
        Progress progress(L"Deleting on server", 0);
        for (const auto& t : targets) {
            s = DeleteTree(view, JoinRemotePath(view.path, t.name), t.isDirectory && !t.isLink, progress, count, error);
            if (s != Status::Ok) break;
        }
    }
    view.selected.clear();
    if (s != Status::Ok && s != Status::Cancelled && !Report(view, s, L"Delete on server", error)) return;
    LoadFolder(view, view.path, CursorName(view));
}

void SelectByMask(ServerView& view, bool select) {
    auto mask = Prompt(select ? L"Select files matching" : L"Unselect files matching", L"*");
    if (!mask) return;
    for (const auto& e : view.entries)
        if (WildcardMatch(e.name.c_str(), mask->c_str())) {
            if (select) view.selected.insert(e.name);
            else view.selected.erase(e.name);
        }
}

void GoUpOrDisconnect(ServerView& view) {
    if (view.path != L"/" && !view.path.empty()) {
        std::wstring child = view.path.substr(view.path.find_last_of(L'/') + 1);
        LoadFolder(view, ParentRemotePath(view.path), child);
        return;
    }
    if (Confirm(L"Disconnect", {L"Disconnect from " + view.connection.host + L"?"})) CloseView(view);
}

void Enter(ServerView& view) {
    if (view.cursor == 0) {
        GoUpOrDisconnect(view);
        return;
    }
    const RemoteEntry* e = EntryAtRow(view, view.cursor);
    if (!e) return;
    if (e->isDirectory || e->isLink) {
        std::wstring target = JoinRemotePath(view.path, e->name);
        if (e->isLink) {
            // A link may point at a file: try it as a folder quietly first.
            std::vector<RemoteEntry> probe;
            std::wstring error;
            if (view.client->List(target, probe, Cancelled, error) != Status::Ok) {
                ViewRemoteFile(view);
                return;
            }
        }
        LoadFolder(view, target, L"");
        return;
    }
    ViewRemoteFile(view);  // decided: Enter on a file views it
}

void ToggleAndMove(ServerView& view, int delta) {
    auto toggle = [&](int row) {
        if (const RemoteEntry* e = EntryAtRow(view, row)) {
            if (!view.selected.erase(e->name)) view.selected.insert(e->name);
        }
    };
    if (delta > 0) {
        toggle(view.cursor);
        ++view.cursor;
    } else {
        --view.cursor;
        ClampCursor(view);
        toggle(view.cursor);
    }
}

// ---------------------------------------------------------------------------
// Panel drawing and input (FTP-002, SFTP-002)

constexpr size_t kSizeWidth = 9, kTimeWidth = 16, kPermWidth = 9;

std::wstring RightAlign(const std::wstring& text, size_t width) {
    size_t used = std::min<size_t>(width, mc::StringDisplayWidth(text));
    return std::wstring(width - used, L' ') + mc::PadToDisplayWidth(text, used);
}

}  // namespace

void Draw(McPanelSide side, uint16_t width, uint16_t height, bool active) {
    ServerView* view = ViewFor(side);
    if (!view) return;
    view->visibleRows = std::max(1, static_cast<int>(height) - 1);
    ClampCursor(*view);
    std::wstring header = Title(view->connection.user, view->connection.host, view->connection.port, view->path);
    g_host->PanelDrawText(side, 0, 0, mc::PadToDisplayWidth(header, width).c_str(), kMcPanelColorSection);

    int details = 3;  // drop permissions, then time, then size on a narrow panel
    auto reserved = [&] {
        size_t r = 3;
        if (details >= 1) r += 1 + kSizeWidth;
        if (details >= 2) r += 1 + kTimeWidth;
        if (details >= 3) r += 1 + kPermWidth;
        return r;
    };
    while (details > 0 && width < reserved() + 10) --details;
    size_t nameWidth = width > reserved() ? width - reserved() : 1;

    for (int row = 0; row < view->visibleRows; ++row) {
        int index = view->topIndex + row;
        if (index >= RowCount(*view)) break;
        const RemoteEntry* e = EntryAtRow(*view, index);
        bool atCursor = active && index == view->cursor;
        bool selected = e && view->selected.count(e->name);
        std::wstring line = std::wstring(atCursor ? L">" : L" ") + (selected ? L"* " : L"  ");
        if (!e) {
            line += mc::PadToDisplayWidth(L"..", nameWidth);
        } else {
            line += mc::PadToDisplayWidth(e->name + (e->isDirectory ? L"/" : e->isLink ? L"@" : L""), nameWidth);
            if (details >= 1) line += L" " + RightAlign(e->isDirectory ? L"<DIR>" : FormatSize(e->size), kSizeWidth);
            if (details >= 2) line += L" " + mc::PadToDisplayWidth(FormatTime(e->modifiedUtc), kTimeWidth);
            if (details >= 3) line += L" " + mc::PadToDisplayWidth(e->permissions, kPermWidth);
        }
        McPanelColor color = atCursor ? kMcPanelColorHighlight
                                      : selected ? kMcPanelColorSelected
                                                 : (!e || e->isDirectory) ? kMcPanelColorSection : kMcPanelColorNormal;
        g_host->PanelDrawText(side, 0, static_cast<int16_t>(row + 1), mc::PadToDisplayWidth(line, width).c_str(), color);
    }
}

void HandleKey(McPanelSide side, uint16_t key, uint32_t control) {
    ServerView* view = ViewFor(side);
    if (!view) return;
    bool ctrl = (control & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    bool shift = (control & SHIFT_PRESSED) != 0;
    int page = view->visibleRows;
    switch (key) {
        case VK_UP:
            if (shift) ToggleAndMove(*view, -1);
            else --view->cursor;
            break;
        case VK_DOWN:
            if (shift) ToggleAndMove(*view, 1);
            else ++view->cursor;
            break;
        case VK_PRIOR: view->cursor -= page; break;
        case VK_NEXT: view->cursor += page; break;
        case VK_HOME: view->cursor = 0; break;
        case VK_END: view->cursor = RowCount(*view) - 1; break;
        case VK_RETURN: Enter(*view); return;
        case VK_BACK: GoUpOrDisconnect(*view); return;
        case VK_INSERT: ToggleAndMove(*view, 1); break;
        case VK_ADD:
            if (ctrl) for (const auto& e : view->entries) view->selected.insert(e.name);
            else SelectByMask(*view, true);
            break;
        case VK_SUBTRACT:
            if (ctrl) view->selected.clear();
            else SelectByMask(*view, false);
            break;
        case VK_MULTIPLY:
            for (const auto& e : view->entries)
                if (!view->selected.erase(e.name)) view->selected.insert(e.name);
            break;
        case VK_F3: ViewRemoteFile(*view); return;
        case VK_F5: Download(*view, false); return;
        case VK_F6: Download(*view, true); return;
        case VK_F7: MakeFolder(*view); return;
        case VK_F8:
        case VK_DELETE: DeleteEntries(*view); return;
        case VK_F2: RenameEntry(*view); return;
        case 'R':
            if (ctrl) {
                LoadFolder(*view, view->path, CursorName(*view));
                return;
            }
            break;
        default: break;
    }
    if (ViewFor(side)) ClampCursor(*view);
}

void HandleMouse(McPanelSide side, int16_t, int16_t y, uint32_t buttons, uint32_t flags, uint32_t control) {
    ServerView* view = ViewFor(side);
    if (!view) return;
    if (flags == MOUSE_WHEELED) {
        view->cursor += static_cast<short>(HIWORD(buttons)) > 0 ? -3 : 3;
        ClampCursor(*view);
        return;
    }
    if (!(buttons & RIGHTMOST_BUTTON_PRESSED)) view->mouseSelecting = false;
    int index = view->topIndex + y - 1;
    if (y < 1 || y > view->visibleRows || index < 0 || index >= RowCount(*view)) return;
    bool range = (control & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    if ((buttons & (RIGHTMOST_BUTTON_PRESSED | FROM_LEFT_1ST_BUTTON_PRESSED)) && range && flags == 0) {
        for (int i = std::min(view->cursor, index); i <= std::max(view->cursor, index); ++i)
            if (const RemoteEntry* e = EntryAtRow(*view, i)) view->selected.insert(e->name);
        view->cursor = index;
        return;
    }
    if (buttons & RIGHTMOST_BUTTON_PRESSED) {
        if (!view->mouseSelecting && flags != 0) return;
        const RemoteEntry* e = EntryAtRow(*view, index);
        if (!view->mouseSelecting) {
            view->mouseSelectValue = !(e && view->selected.count(e->name));
            view->mouseSelecting = true;
        }
        if (e) {
            if (view->mouseSelectValue) view->selected.insert(e->name);
            else view->selected.erase(e->name);
        }
        view->cursor = index;
        return;
    }
    if ((buttons & FROM_LEFT_1ST_BUTTON_PRESSED) && (flags == 0 || flags == DOUBLE_CLICK)) {
        view->cursor = index;
        if (flags == DOUBLE_CLICK) Enter(*view);
    }
}

void PanelInit(const McPluginHost* host, PanelProtocol protocol) {
    g_host = host;
    g_protocol = std::move(protocol);
}

void PanelShutdown() {
    for (auto& view : g_views) {
        if (view && view->client) view->client->Disconnect();
        view.reset();
    }
    g_host = nullptr;
}

void OnMenuAction(uint32_t commandId) {
    if (commandId == P().connectionsCommandId) ConnectionManager();
    else if (commandId == P().newConnectionCommandId) NewConnection();
    else if (commandId == P().disconnectCommandId) DisconnectFromMenu();
}

bool OnReceiveFiles(McPanelSide side, McPanelSide sourceSide) {
    ServerView* view = ViewFor(side);
    if (!view) return false;
    Upload(*view, sourceSide);
    return true;
}

}  // namespace remote
