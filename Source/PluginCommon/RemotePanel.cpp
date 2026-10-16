// EXT-004 (D2) / EXT-002 (D4): the FTP/SFTP plugins' shared remote server
// as a file-system provider -- see RemotePanel.h.

#include "RemotePanel.h"

#include "RemoteUi.h"

#include <algorithm>
#include <cwchar>
#include <cwctype>

namespace remote {

namespace {

const McPluginHost* g_host = nullptr;
PanelProtocol g_protocol;
std::vector<ServerSession*> g_sessions;
constexpr int kNameBuffer = 512;
constexpr ULONGLONG kCacheMs = 30000;

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

// Handed to the protocol clients as their cancel callback; runs inside bounded
// network waits, so it never blocks. Pausing (FOP-015) is the host's: its own
// transfer progress callback waits while the user has paused.
bool Cancelled() { return g_host && g_host->PollCancel && g_host->PollCancel(); }

namespace {

bool Confirm(const std::wstring& title, const std::vector<std::wstring>& lines) {
    return Choose(title, lines, {{L'Y', L"Yes"}, {L'N', L"No"}}) == 0;
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

McPanelSide Other(McPanelSide side) { return side == kMcPanelLeft ? kMcPanelRight : kMcPanelLeft; }

std::wstring Title(const std::wstring& user, const std::wstring& host, uint16_t port, const std::wstring& path) {
    return FormatPanelTitle(P().scheme, P().store->defaultPort, user, host, port, path);
}

// ---------------------------------------------------------------------------
// The provider (McFileSystem.h). `context` is the ServerSession.

ServerSession& S(void* context) { return *static_cast<ServerSession*>(context); }

McFsStatus ToFs(Status s) {
    switch (s) {
        case Status::Ok: return kMcFsOk;
        case Status::Cancelled: return kMcFsCancelled;
        case Status::ConnectionLost: return kMcFsConnectionLost;
        case Status::Failed:
        default: return kMcFsFailed;
    }
}

void PutError(const std::wstring& text, wchar_t* error, int32_t size) {
    if (error && size > 0) wcsncpy_s(error, static_cast<size_t>(size), text.c_str(), _TRUNCATE);
}

void Emit(const RemoteEntry& e, McFsEntrySink sink, void* sinkContext) {
    McFsEntry out{};
    out.structSize = sizeof(McFsEntry);
    out.name = e.name.c_str();
    out.isDirectory = e.isDirectory;
    out.isLink = e.isLink;
    out.size = e.size;
    out.modifiedUtc = e.modifiedUtc;
    out.permissions = e.permissions.c_str();
    sink(sinkContext, &out);
}

std::wstring LeafName(const std::wstring& path) {
    size_t slash = path.find_last_of(L'/');
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

// The cache follows this session's own changes so a copy of many files into
// one folder doesn't re-list it once per file.
void CacheUpsert(ServerSession& s, const std::wstring& path, const RemoteEntry& entry) {
    if (!s.cacheValid || s.cachedDir != ParentRemotePath(path)) return;
    auto it = std::find_if(s.cached.begin(), s.cached.end(), [&](const RemoteEntry& e) { return e.name == entry.name; });
    if (it != s.cached.end()) *it = entry;
    else s.cached.push_back(entry);
}

void CacheRemove(ServerSession& s, const std::wstring& path) {
    if (!s.cacheValid || s.cachedDir != ParentRemotePath(path)) return;
    std::wstring name = LeafName(path);
    s.cached.erase(std::remove_if(s.cached.begin(), s.cached.end(), [&](const RemoteEntry& e) { return e.name == name; }),
                   s.cached.end());
}

// FTP-009 / SFTP-008: a dropped connection while listing offers to reconnect.
bool OfferReconnect(ServerSession& s, const std::wstring& error);

Status ListFolder(ServerSession& s, const std::wstring& dir, std::vector<RemoteEntry>& entries, std::wstring& error) {
    Status st = s.client->List(dir, entries, Cancelled, error);
    if (st == Status::ConnectionLost && OfferReconnect(s, error)) {
        error.clear();
        st = s.client->List(dir, entries, Cancelled, error);
    }
    if (st == Status::Ok) {
        s.cachedDir = dir;
        s.cached = entries;
        s.cachedAt = GetTickCount64();
        s.cacheValid = true;
    }
    return st;
}

void RemovePendingParts(ServerSession& s) {
    for (const auto& part : s.pendingParts) {
        std::wstring ignored;
        s.client->DeleteRemoteFile(part, {}, ignored);
    }
    s.pendingParts.clear();
}

bool OfferReconnect(ServerSession& s, const std::wstring& error) {
    Log(kMcLogWarning, L"connection lost: " + error);
    int choice = Choose(P().name + L" connection lost", {s.connection.host + L":", error},
                        {{L'R', L"Reconnect"}, {L'C', L"Cancel"}});
    if (choice != 0) return false;
    std::wstring connectError;
    if (s.client->Connect(s.connection, s.password, Cancelled, connectError) == Status::Ok) {
        RemovePendingParts(s);
        return true;
    }
    Message(P().name + L" connection", {L"Could not reconnect:", connectError});
    return false;
}

McFsStatus __stdcall FsList(void* context, const wchar_t* dir, McFsEntrySink sink, void* sinkContext, wchar_t* error,
                            int32_t errorSize) {
    ServerSession& s = S(context);
    std::vector<RemoteEntry> entries;
    std::wstring err;
    Status st = ListFolder(s, dir, entries, err);
    if (st != Status::Ok) {
        PutError(err, error, errorSize);
        return ToFs(st);
    }
    for (const auto& e : entries) Emit(e, sink, sinkContext);
    return kMcFsOk;
}

McFsStatus __stdcall FsStat(void* context, const wchar_t* path, McFsEntrySink sink, void* sinkContext, wchar_t* error,
                            int32_t errorSize) {
    ServerSession& s = S(context);
    std::wstring p = path;
    if (p.empty() || p == L"/") {
        RemoteEntry root;
        root.name = L"/";
        root.isDirectory = true;
        Emit(root, sink, sinkContext);
        return kMcFsOk;
    }
    std::wstring parent = ParentRemotePath(p), name = LeafName(p);
    if (!s.cacheValid || s.cachedDir != parent || GetTickCount64() - s.cachedAt > kCacheMs) {
        std::vector<RemoteEntry> entries;
        std::wstring err;
        Status st = ListFolder(s, parent, entries, err);
        if (st != Status::Ok) {
            PutError(err, error, errorSize);
            // A missing parent folder means the path doesn't exist either.
            return st == Status::Failed ? kMcFsNotFound : ToFs(st);
        }
    }
    for (const auto& e : s.cached) {
        if (e.name == name) {
            Emit(e, sink, sinkContext);
            return kMcFsOk;
        }
    }
    return kMcFsNotFound;
}

std::wstring TempFile() {
    wchar_t dir[MAX_PATH], file[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"mcr", 0, file)) return L"";
    return file;
}

// Reading: the file is fetched into a temporary local file first (the client
// moves whole files); Read then streams from it. DELETE_ON_CLOSE cleans up.
McFsStatus __stdcall FsOpenRead(void* context, const wchar_t* path, void** stream, wchar_t* error, int32_t errorSize) {
    ServerSession& s = S(context);
    std::wstring temp = TempFile(), err;
    if (temp.empty()) {
        PutError(L"cannot create a temporary file", error, errorSize);
        return kMcFsFailed;
    }
    Status st = s.client->Download(path, temp, 0, {}, Cancelled, err);
    if (st != Status::Ok) {
        DeleteFileW(temp.c_str());
        PutError(err, error, errorSize);
        return ToFs(st);
    }
    HANDLE h = CreateFileW(temp.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_DELETE_ON_CLOSE | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        DeleteFileW(temp.c_str());
        PutError(L"cannot open the downloaded copy", error, errorSize);
        return kMcFsFailed;
    }
    *stream = h;
    return kMcFsOk;
}

McFsStatus __stdcall FsRead(void*, void* stream, void* buffer, uint32_t capacity, uint32_t* got, wchar_t* error,
                            int32_t errorSize) {
    DWORD read = 0;
    if (!ReadFile(static_cast<HANDLE>(stream), buffer, capacity, &read, nullptr)) {
        PutError(L"cannot read the downloaded copy", error, errorSize);
        return kMcFsFailed;
    }
    *got = read;
    return kMcFsOk;
}

void __stdcall FsCloseRead(void*, void* stream) { CloseHandle(static_cast<HANDLE>(stream)); }

// Uploads `local` to "<path><partSuffix>", then renames it over `path` -- the
// existing file is replaced only once the new one is complete.
Status UploadReplacing(ServerSession& s, const std::wstring& local, const std::wstring& path, const ProgressFn& progress,
                       const CancelFn& cancel, std::wstring& error) {
    std::wstring part = path + P().partSuffix;
    s.pendingParts.insert(part);
    Status st = s.client->Upload(local, part, progress, cancel, error);
    if (st != Status::Ok) {
        if (st != Status::ConnectionLost) {
            std::wstring ignored;
            s.client->DeleteRemoteFile(part, {}, ignored);
            s.pendingParts.erase(part);
        }
        return st;
    }
    std::wstring renameError;
    st = s.client->RenameRemote(part, path, Cancelled, renameError);
    if (st == Status::Failed) {  // the target exists (a rename never overwrites): replace it now
        std::wstring deleteError;
        if (s.client->DeleteRemoteFile(path, Cancelled, deleteError) == Status::Ok)
            st = s.client->RenameRemote(part, path, Cancelled, renameError);
    }
    if (st != Status::Ok) {
        error = renameError.empty() ? L"could not move the upload into place" : renameError;
        if (st != Status::ConnectionLost) {
            std::wstring ignored;
            s.client->DeleteRemoteFile(part, {}, ignored);
            s.pendingParts.erase(part);
        }
        return st;
    }
    s.pendingParts.erase(part);
    WIN32_FILE_ATTRIBUTE_DATA data{};
    RemoteEntry entry;
    entry.name = LeafName(path);
    if (GetFileAttributesExW(local.c_str(), GetFileExInfoStandard, &data))
        entry.size = (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    CacheUpsert(s, path, entry);
    return Status::Ok;
}

struct WriteStream {
    HANDLE h = INVALID_HANDLE_VALUE;
    std::wstring temp;
    std::wstring path;
};

McFsStatus __stdcall FsOpenWrite(void*, const wchar_t* path, uint64_t, uint64_t, void** stream, wchar_t* error,
                                 int32_t errorSize) {
    auto w = std::make_unique<WriteStream>();
    w->temp = TempFile();
    w->path = path;
    w->h = w->temp.empty() ? INVALID_HANDLE_VALUE
                           : CreateFileW(w->temp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (w->h == INVALID_HANDLE_VALUE) {
        if (!w->temp.empty()) DeleteFileW(w->temp.c_str());
        PutError(L"cannot create a temporary file", error, errorSize);
        return kMcFsFailed;
    }
    *stream = w.release();
    return kMcFsOk;
}

McFsStatus __stdcall FsWrite(void*, void* stream, const void* data, uint32_t size, wchar_t* error, int32_t errorSize) {
    auto* w = static_cast<WriteStream*>(stream);
    DWORD written = 0;
    if (!WriteFile(w->h, data, size, &written, nullptr) || written != size) {
        PutError(L"cannot write the temporary file", error, errorSize);
        return kMcFsFailed;
    }
    return kMcFsOk;
}

McFsStatus __stdcall FsFinishWrite(void* context, void* stream, wchar_t* error, int32_t errorSize) {
    std::unique_ptr<WriteStream> w(static_cast<WriteStream*>(stream));
    CloseHandle(w->h);
    std::wstring err;
    Status st = UploadReplacing(S(context), w->temp, w->path, {}, Cancelled, err);
    DeleteFileW(w->temp.c_str());
    if (st != Status::Ok) PutError(err, error, errorSize);
    return ToFs(st);
}

void __stdcall FsAbortWrite(void*, void* stream) {
    std::unique_ptr<WriteStream> w(static_cast<WriteStream*>(stream));
    CloseHandle(w->h);
    DeleteFileW(w->temp.c_str());
}

McFsStatus __stdcall FsMakeDirectory(void* context, const wchar_t* path, wchar_t* error, int32_t errorSize) {
    ServerSession& s = S(context);
    std::wstring err;
    Status st = s.client->MakeRemoteDirectory(path, Cancelled, err);
    if (st != Status::Ok) {
        PutError(err, error, errorSize);
        return ToFs(st);
    }
    RemoteEntry entry;
    entry.name = LeafName(path);
    entry.isDirectory = true;
    CacheUpsert(s, path, entry);
    return kMcFsOk;
}

// `isDirectory` is true only for a real folder; a link -- even to a folder --
// is removed as itself, never followed (SEC-006).
McFsStatus __stdcall FsDelete(void* context, const wchar_t* path, bool isDirectory, wchar_t* error, int32_t errorSize) {
    ServerSession& s = S(context);
    std::wstring err;
    Status st = isDirectory ? s.client->RemoveRemoteDirectory(path, Cancelled, err)
                            : s.client->DeleteRemoteFile(path, Cancelled, err);
    if (st != Status::Ok) {
        PutError(err, error, errorSize);
        return ToFs(st);
    }
    CacheRemove(s, path);
    return kMcFsOk;
}

McFsStatus __stdcall FsRename(void* context, const wchar_t* from, const wchar_t* to, wchar_t* error, int32_t errorSize) {
    ServerSession& s = S(context);
    std::wstring err;
    Status st = s.client->RenameRemote(from, to, Cancelled, err);
    if (st != Status::Ok) {
        PutError(err, error, errorSize);
        return ToFs(st);
    }
    s.cacheValid = false;
    return kMcFsOk;
}

// Ctrl+R: the next listing goes to the server again.
McFsStatus __stdcall FsReload(void* context, wchar_t*, int32_t) {
    S(context).cacheValid = false;
    return kMcFsOk;
}

McFsStatus __stdcall FsReconnect(void* context, wchar_t* error, int32_t errorSize) {
    ServerSession& s = S(context);
    std::wstring err;
    Status st = s.client->Connect(s.connection, s.password, Cancelled, err);
    if (st != Status::Ok) {
        PutError(err, error, errorSize);
        return ToFs(st);
    }
    Log(kMcLogInfo, L"reconnected to " + s.connection.host);
    RemovePendingParts(s);
    s.cacheValid = false;
    return kMcFsOk;
}

struct TransferProgress {
    McFsProgressFn fn;
    void* context;
    bool stopped = false;
};

McFsStatus __stdcall FsDownloadToFile(void* context, const wchar_t* path, const wchar_t* localPath, uint64_t sizeHint,
                                      McFsProgressFn progress, void* progressContext, wchar_t* error,
                                      int32_t errorSize) {
    ServerSession& s = S(context);
    TransferProgress tp{progress, progressContext};
    std::wstring err;
    Status st = s.client->Download(
        path, localPath, sizeHint,
        [&](uint64_t done, uint64_t total) {
            if (tp.fn && !tp.fn(tp.context, done, total)) tp.stopped = true;
        },
        [&] { return tp.stopped || Cancelled(); }, err);
    if (tp.stopped) st = Status::Cancelled;
    if (st != Status::Ok) {
        DeleteFileW(localPath);
        PutError(err, error, errorSize);
    }
    return ToFs(st);
}

McFsStatus __stdcall FsUploadFromFile(void* context, const wchar_t* localPath, const wchar_t* path, uint64_t,
                                      McFsProgressFn progress, void* progressContext, wchar_t* error,
                                      int32_t errorSize) {
    ServerSession& s = S(context);
    TransferProgress tp{progress, progressContext};
    std::wstring err;
    Status st = UploadReplacing(
        s, localPath, path,
        [&](uint64_t done, uint64_t total) {
            if (tp.fn && !tp.fn(tp.context, done, total)) tp.stopped = true;
        },
        [&] { return tp.stopped || Cancelled(); }, err);
    if (tp.stopped && st != Status::Ok) st = Status::Cancelled;
    if (st != Status::Ok) PutError(err, error, errorSize);
    return ToFs(st);
}

void __stdcall FsClose(void* context) {
    ServerSession* s = static_cast<ServerSession*>(context);
    if (s->client) {
        RemovePendingParts(*s);
        s->client->Disconnect();
    }
    Log(kMcLogInfo, L"disconnected from " + s->connection.host);
    g_sessions.erase(std::remove(g_sessions.begin(), g_sessions.end(), s), g_sessions.end());
    delete s;
    UpdateDisconnectItem();
}

}  // namespace

McFsProvider MakeProviderTable(ServerSession& session) {
    McFsProvider t{};
    t.structSize = sizeof(McFsProvider);
    t.context = &session;
    t.capabilities = kMcFsCapRead | kMcFsCapWrite | kMcFsCapMkdir | kMcFsCapDelete | kMcFsCapRename;
    t.displayRoot = session.displayRoot.c_str();
    t.kind = P().scheme.c_str();
    t.rootPath = L"/";
    t.separator = L'/';
    t.caseInsensitive = false;
    t.confirmOnLeave = true;  // FTP-002 / SFTP-002: leaving the server's top folder asks first
    t.List = &FsList;
    t.Stat = &FsStat;
    t.OpenRead = &FsOpenRead;
    t.Read = &FsRead;
    t.CloseRead = &FsCloseRead;
    t.OpenWrite = &FsOpenWrite;
    t.Write = &FsWrite;
    t.FinishWrite = &FsFinishWrite;
    t.AbortWrite = &FsAbortWrite;
    t.MakeDirectory = &FsMakeDirectory;
    t.Delete = &FsDelete;
    t.Rename = &FsRename;
    t.Reconnect = &FsReconnect;
    t.Reload = &FsReload;
    t.DownloadToFile = &FsDownloadToFile;
    t.UploadFromFile = &FsUploadFromFile;
    t.Close = &FsClose;
    return t;
}

size_t SessionCount() { return g_sessions.size(); }

ServerSession* SessionFor(McPanelSide side) {
    if (!g_host || !g_host->GetPanelProvider) return nullptr;
    void* context = g_host->GetPanelProvider(side);
    for (ServerSession* s : g_sessions)
        if (s == context) return s;
    return nullptr;
}

// Network > "Disconnect <name>" is offered only while a panel is connected
// (the host hides a disabled plugin menu item).
void UpdateDisconnectItem() {
    if (g_host && g_host->SetMenuItemEnabled) g_host->SetMenuItemEnabled(P().disconnectCommandId, !g_sessions.empty());
}

namespace {

// Network > Disconnect <name>: the active panel's server, else the other panel's.
void DisconnectFromMenu() {
    McPanelSide active = g_host->GetActiveSide();
    if (SessionFor(active)) g_host->CloseProvider(active);
    else if (SessionFor(Other(active))) g_host->CloseProvider(Other(active));
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


// Connects, then hands the session to the host as the active panel's
// provider. On failure the session is freed here; on success the host's Close
// frees it later.
void OpenServer(const Connection& connection, std::wstring password) {
    McPanelSide side = g_host->GetActiveSide();
    auto session = std::make_unique<ServerSession>();
    session->side = side;
    session->connection = connection;
    session->password = std::move(password);
    session->client = P().makeClient();
    session->client->SetTrace([](const std::wstring& line) { Log(kMcLogDebug, line); });  // never a secret
    session->displayRoot = Title(connection.user, connection.host, connection.port, L"");
    if (!session->displayRoot.empty() && session->displayRoot.back() == L'/') session->displayRoot.pop_back();
    std::wstring error;
    Status s;
    {
        Progress progress(L"Connecting", 0);
        progress.Update(0, 0, DisplayHost(connection));
        s = session->client->Connect(session->connection, session->password, Cancelled, error);
    }
    if (s != Status::Ok) {
        if (s != Status::Cancelled)
            Message(P().name + L" connection", {L"Could not connect to " + DisplayHost(connection) + L":", error});
        return;
    }
    std::wstring start = connection.folder;
    if (start.empty() && session->client->HomeDirectory(start, Cancelled, error) != Status::Ok) start = L"/";
    if (start.empty() || start[0] != L'/') start = L"/" + start;
    Log(kMcLogInfo, L"connected to " + DisplayHost(connection));

    ServerSession* raw = session.release();
    g_sessions.push_back(raw);
    UpdateDisconnectItem();
    McFsProvider table = MakeProviderTable(*raw);
    if (!g_host->OpenProvider(side, &table, start.c_str(), nullptr)) {
        // Not taken over (e.g. the user kept a modified archive open there).
        g_sessions.erase(std::remove(g_sessions.begin(), g_sessions.end(), raw), g_sessions.end());
        raw->client->Disconnect();
        Log(kMcLogInfo, L"disconnected from " + connection.host);
        delete raw;
        UpdateDisconnectItem();
    }
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

}  // namespace

void PanelInit(const McPluginHost* host, PanelProtocol protocol) {
    g_host = host;
    g_protocol = std::move(protocol);
}

void PanelShutdown() {
    // The host closes every provider before a plugin shuts down; this only
    // covers a host that didn't (a harness).
    for (ServerSession* s : g_sessions) {
        if (s->client) s->client->Disconnect();
        delete s;
    }
    g_sessions.clear();
    g_host = nullptr;
}

void OnMenuAction(uint32_t commandId) {
    if (commandId == P().connectionsCommandId) ConnectionManager();
    else if (commandId == P().newConnectionCommandId) NewConnection();
    else if (commandId == P().disconnectCommandId) DisconnectFromMenu();
}

}  // namespace remote
