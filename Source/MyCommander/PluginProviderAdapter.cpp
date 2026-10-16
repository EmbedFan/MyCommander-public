#include "PluginProviderAdapter.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <vector>

namespace mc {

namespace {

constexpr int32_t kErrorSize = 1024;

ProviderStatus FromFs(McFsStatus s) {
    switch (s) {
    case kMcFsOk: return ProviderStatus::Ok;
    case kMcFsCancelled: return ProviderStatus::Cancelled;
    case kMcFsConnectionLost: return ProviderStatus::ConnectionLost;
    case kMcFsNotSupported: return ProviderStatus::NotSupported;
    case kMcFsNotFound: return ProviderStatus::NotFound;
    case kMcFsFailed:
    default: return ProviderStatus::Failed;
    }
}

ProviderEntry FromFsEntry(const McFsEntry& e) {
    ProviderEntry p;
    p.name = e.name ? e.name : L"";
    p.isDirectory = e.isDirectory;
    p.isLink = e.isLink;
    p.size = e.size;
    p.modifiedUtc = e.modifiedUtc;
    p.attributes = e.attributes;
    p.permissions = e.permissions ? e.permissions : L"";
    return p;
}

struct ListSink {
    std::vector<ProviderEntry>* entries;
};

void __stdcall CollectEntry(void* context, const McFsEntry* entry) {
    if (!context || !entry) return;
    auto* sink = static_cast<ListSink*>(context);
    ProviderEntry p = FromFsEntry(*entry);
    // Names never carry separators or dot entries; a misbehaving plugin can't
    // smuggle a path into a copy target (zip-slip in reverse).
    if (p.name.empty() || p.name == L"." || p.name == L".." || p.name.find_first_of(L"\\/") != std::wstring::npos)
        return;
    sink->entries->push_back(std::move(p));
}

struct StatSink {
    ProviderEntry* entry;
    bool seen;
};

void __stdcall CollectStat(void* context, const McFsEntry* entry) {
    if (!context || !entry) return;
    auto* sink = static_cast<StatSink*>(context);
    *sink->entry = FromFsEntry(*entry);
    sink->seen = true;
}

// --- SEH thunks: no C++ objects with destructors in these frames ---

bool SehList(const McFsProvider* t, const wchar_t* dir, void* sink, wchar_t* err, McFsStatus* out) {
    __try { *out = t->List(t->context, dir, &CollectEntry, sink, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehStat(const McFsProvider* t, const wchar_t* path, void* sink, wchar_t* err, McFsStatus* out) {
    __try { *out = t->Stat(t->context, path, &CollectStat, sink, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehOpenRead(const McFsProvider* t, const wchar_t* path, void** stream, wchar_t* err, McFsStatus* out) {
    __try { *out = t->OpenRead(t->context, path, stream, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehRead(const McFsProvider* t, void* stream, void* buffer, uint32_t capacity, uint32_t* got, wchar_t* err,
             McFsStatus* out) {
    __try { *out = t->Read(t->context, stream, buffer, capacity, got, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehCloseRead(const McFsProvider* t, void* stream) {
    __try { t->CloseRead(t->context, stream); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehOpenWrite(const McFsProvider* t, const wchar_t* path, uint64_t sizeHint, uint64_t modified, void** stream,
                  wchar_t* err, McFsStatus* out) {
    __try { *out = t->OpenWrite(t->context, path, sizeHint, modified, stream, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehWrite(const McFsProvider* t, void* stream, const void* data, uint32_t size, wchar_t* err, McFsStatus* out) {
    __try { *out = t->Write(t->context, stream, data, size, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehFinishWrite(const McFsProvider* t, void* stream, wchar_t* err, McFsStatus* out) {
    __try { *out = t->FinishWrite(t->context, stream, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehAbortWrite(const McFsProvider* t, void* stream) {
    __try { t->AbortWrite(t->context, stream); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehMakeDirectory(const McFsProvider* t, const wchar_t* path, wchar_t* err, McFsStatus* out) {
    __try { *out = t->MakeDirectory(t->context, path, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehDelete(const McFsProvider* t, const wchar_t* path, bool isDirectory, wchar_t* err, McFsStatus* out) {
    __try { *out = t->Delete(t->context, path, isDirectory, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehRename(const McFsProvider* t, const wchar_t* from, const wchar_t* to, wchar_t* err, McFsStatus* out) {
    __try { *out = t->Rename(t->context, from, to, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehHasPending(const McFsProvider* t, bool* out) {
    __try { *out = t->HasPendingChanges(t->context); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehCommit(const McFsProvider* t, wchar_t* err, McFsStatus* out) {
    __try { *out = t->Commit(t->context, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehReconnect(const McFsProvider* t, wchar_t* err, McFsStatus* out) {
    __try { *out = t->Reconnect(t->context, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool __stdcall ForwardProgress(void* context, uint64_t done, uint64_t total) {
    auto* progress = static_cast<const ProviderByteProgress*>(context);
    return !progress || !*progress || (*progress)(done, total);
}
bool SehDownloadToFile(const McFsProvider* t, const wchar_t* path, const wchar_t* local, uint64_t sizeHint,
                       void* progress, wchar_t* err, McFsStatus* out) {
    __try { *out = t->DownloadToFile(t->context, path, local, sizeHint, &ForwardProgress, progress, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehUploadFromFile(const McFsProvider* t, const wchar_t* local, const wchar_t* path, uint64_t modified,
                       void* progress, wchar_t* err, McFsStatus* out) {
    __try { *out = t->UploadFromFile(t->context, local, path, modified, &ForwardProgress, progress, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehSetAttributes(const McFsProvider* t, const wchar_t* path, uint32_t attributes, wchar_t* err, McFsStatus* out) {
    __try { *out = t->SetAttributes(t->context, path, attributes, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehReload(const McFsProvider* t, wchar_t* err, McFsStatus* out) {
    __try { *out = t->Reload(t->context, err, kErrorSize); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}
bool SehClose(const McFsProvider* t) {
    __try { t->Close(t->context); return true; }
    __except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

std::wstring ErrorText(const std::vector<wchar_t>& buffer) {
    std::wstring s(buffer.data());
    return s;
}

class AdapterReadStream final : public IReadStream {
public:
    AdapterReadStream(const PluginProviderAdapter& owner, void* stream) : owner_(owner), stream_(stream) {}
    ~AdapterReadStream() override {
        const McFsProvider* t = &owner_.Table();
        void* s = stream_;
        if (!owner_.Crashed()) owner_.Guard(L"CloseRead", [&] { return SehCloseRead(t, s); });
    }
    ProviderStatus Read(void* buffer, size_t capacity, size_t& got, std::wstring& error) override {
        got = 0;
        if (owner_.Crashed()) {
            error = L"the plugin stopped working";
            return ProviderStatus::Failed;
        }
        std::vector<wchar_t> err(kErrorSize, L'\0');
        McFsStatus st = kMcFsFailed;
        uint32_t read = 0;
        uint32_t cap = static_cast<uint32_t>(capacity > 0x7FFFFFFF ? 0x7FFFFFFF : capacity);
        const McFsProvider* t = &owner_.Table();
        if (!owner_.Guard(L"Read", [&] { return SehRead(t, stream_, buffer, cap, &read, err.data(), &st); })) {
            error = L"the plugin stopped working";
            return ProviderStatus::Failed;
        }
        if (st != kMcFsOk) {
            error = ErrorText(err);
            return FromFs(st);
        }
        got = read > cap ? cap : read;
        return ProviderStatus::Ok;
    }

private:
    const PluginProviderAdapter& owner_;
    void* stream_;
};

class AdapterWriteStream final : public IWriteStream {
public:
    AdapterWriteStream(const PluginProviderAdapter& owner, void* stream) : owner_(owner), stream_(stream) {}
    ~AdapterWriteStream() override { Abort(); }
    ProviderStatus Write(const void* data, size_t size, std::wstring& error) override {
        if (done_ || owner_.Crashed()) {
            error = done_ ? L"stream closed" : L"the plugin stopped working";
            return ProviderStatus::Failed;
        }
        const McFsProvider* t = &owner_.Table();
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            uint32_t chunk = static_cast<uint32_t>(size > 0x10000000 ? 0x10000000 : size);
            std::vector<wchar_t> err(kErrorSize, L'\0');
            McFsStatus st = kMcFsFailed;
            if (!owner_.Guard(L"Write", [&] { return SehWrite(t, stream_, p, chunk, err.data(), &st); })) {
                error = L"the plugin stopped working";
                return ProviderStatus::Failed;
            }
            if (st != kMcFsOk) {
                error = ErrorText(err);
                return FromFs(st);
            }
            p += chunk;
            size -= chunk;
        }
        return ProviderStatus::Ok;
    }
    ProviderStatus Finish(std::wstring& error) override {
        if (done_) {
            error = L"stream closed";
            return ProviderStatus::Failed;
        }
        done_ = true;
        if (owner_.Crashed()) {
            error = L"the plugin stopped working";
            return ProviderStatus::Failed;
        }
        const McFsProvider* t = &owner_.Table();
        std::vector<wchar_t> err(kErrorSize, L'\0');
        McFsStatus st = kMcFsFailed;
        if (!owner_.Guard(L"FinishWrite", [&] { return SehFinishWrite(t, stream_, err.data(), &st); })) {
            error = L"the plugin stopped working";
            return ProviderStatus::Failed;
        }
        if (st != kMcFsOk) error = ErrorText(err);
        return FromFs(st);
    }
    void Abort() override {
        if (done_) return;
        done_ = true;
        if (owner_.Crashed()) return;
        const McFsProvider* t = &owner_.Table();
        owner_.Guard(L"AbortWrite", [&] { return SehAbortWrite(t, stream_); });
    }

private:
    const PluginProviderAdapter& owner_;
    void* stream_;
    bool done_ = false;
};

}  // namespace

bool PluginProviderAdapter::Validate(const McFsProvider& t, std::wstring& problem) {
    if (t.structSize < sizeof(McFsProvider)) {
        problem = L"McFsProvider::structSize is too small";
        return false;
    }
    if (!t.List || !t.Stat || !t.Close) {
        problem = L"List, Stat and Close are required";
        return false;
    }
    if (!t.rootPath || !*t.rootPath || !t.separator) {
        problem = L"rootPath and separator are required";
        return false;
    }
    if ((t.capabilities & kMcFsCapRead) && (!t.OpenRead || !t.Read || !t.CloseRead)) {
        problem = L"kMcFsCapRead needs OpenRead, Read and CloseRead";
        return false;
    }
    if ((t.capabilities & kMcFsCapWrite) && (!t.OpenWrite || !t.Write || !t.FinishWrite || !t.AbortWrite)) {
        problem = L"kMcFsCapWrite needs OpenWrite, Write, FinishWrite and AbortWrite";
        return false;
    }
    if ((t.capabilities & kMcFsCapMkdir) && !t.MakeDirectory) {
        problem = L"kMcFsCapMkdir needs MakeDirectory";
        return false;
    }
    if ((t.capabilities & kMcFsCapDelete) && !t.Delete) {
        problem = L"kMcFsCapDelete needs Delete";
        return false;
    }
    if ((t.capabilities & kMcFsCapRename) && !t.Rename) {
        problem = L"kMcFsCapRename needs Rename";
        return false;
    }
    if ((t.capabilities & kMcFsCapAttributes) && !t.SetAttributes) {
        problem = L"kMcFsCapAttributes needs SetAttributes";
        return false;
    }
    if ((t.HasPendingChanges == nullptr) != (t.Commit == nullptr)) {
        problem = L"HasPendingChanges and Commit go together";
        return false;
    }
    if ((t.DownloadToFile == nullptr) != (t.UploadFromFile == nullptr)) {
        problem = L"DownloadToFile and UploadFromFile go together";
        return false;
    }
    return true;
}

PluginProviderAdapter::PluginProviderAdapter(const McFsProvider& table, std::wstring ownerId, Scope scope,
                                             CrashReport onCrash)
    : table_(table), ownerId_(std::move(ownerId)), scope_(std::move(scope)), onCrash_(std::move(onCrash)) {
    displayRoot_ = table.displayRoot ? table.displayRoot : L"";
    kind_ = table.kind ? table.kind : L"plugin";
    root_ = table.rootPath ? table.rootPath : L"/";
    separator_ = table.separator ? table.separator : L'/';
    caseInsensitive_ = table.caseInsensitive;
    confirmOnLeave_ = table.confirmOnLeave;
    // The table's own string pointers are only valid until Close; keep copies.
    table_.displayRoot = nullptr;
    table_.kind = nullptr;
    table_.rootPath = nullptr;
}

PluginProviderAdapter::~PluginProviderAdapter() {
    if (crashed_) return;  // never call back into a plugin that already crashed
    const McFsProvider* t = &table_;
    Guard(L"Close", [&] { return SehClose(t); });
}

bool PluginProviderAdapter::Guard(const wchar_t* what, const std::function<bool()>& sehCall) const {
    if (crashed_) return false;
    if (scope_) scope_(true);
    bool ok = sehCall();
    if (scope_) scope_(false);
    if (!ok) {
        crashed_ = true;
        if (onCrash_) onCrash_(std::wstring(L"crashed in its file-system provider (") + what + L")");
    }
    return ok;
}

ProviderStatus PluginProviderAdapter::CrashedStatus(std::wstring& error) {
    error = L"the plugin stopped working";
    return ProviderStatus::Failed;
}

uint32_t PluginProviderAdapter::Capabilities() const {
    if (crashed_) return 0;
    uint32_t caps = 0;
    if (table_.capabilities & kMcFsCapRead) caps |= CapRead;
    if (table_.capabilities & kMcFsCapWrite) caps |= CapWrite;
    if (table_.capabilities & kMcFsCapMkdir) caps |= CapMkdir;
    if (table_.capabilities & kMcFsCapDelete) caps |= CapDelete;
    if (table_.capabilities & kMcFsCapRename) caps |= CapRename;
    if (table_.capabilities & kMcFsCapAttributes) caps |= CapAttributes;
    if (table_.DownloadToFile && table_.UploadFromFile) caps |= CapFileTransfer;
    return caps;
}

std::wstring PluginProviderAdapter::Normalize(const std::wstring& path) const {
    std::wstring p = path.empty() ? root_ : path;
    while (p.size() > root_.size() && p.back() == separator_) p.pop_back();
    return p;
}

std::wstring PluginProviderAdapter::Join(const std::wstring& dir, const std::wstring& name) const {
    std::wstring d = Normalize(dir);
    if (!d.empty() && d.back() == separator_) return d + name;
    return d + separator_ + name;
}

std::wstring PluginProviderAdapter::Parent(const std::wstring& path) const {
    std::wstring p = Normalize(path);
    if (IsRoot(p)) return p;
    size_t at = p.rfind(separator_);
    if (at == std::wstring::npos) return root_;
    std::wstring parent = p.substr(0, at);
    if (parent.size() < root_.size()) return root_;
    return parent;
}

bool PluginProviderAdapter::IsRoot(const std::wstring& path) const { return SamePath(Normalize(path), root_); }

bool PluginProviderAdapter::SamePath(const std::wstring& a, const std::wstring& b) const {
    std::wstring x = Normalize(a), y = Normalize(b);
    if (!caseInsensitive_) return x == y;
    return CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) ==
           CSTR_EQUAL;
}

ProviderStatus PluginProviderAdapter::List(const std::wstring& dir, std::vector<ProviderEntry>& entries,
                                           const ProviderCancelPoll&, std::wstring& error) {
    entries.clear();
    if (crashed_) return CrashedStatus(error);
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    ListSink sink{&entries};
    std::wstring d = Normalize(dir);
    const McFsProvider* t = &table_;
    if (!Guard(L"List", [&] { return SehList(t, d.c_str(), &sink, err.data(), &st); })) {
        entries.clear();
        return CrashedStatus(error);
    }
    if (st != kMcFsOk) {
        entries.clear();
        error = ErrorText(err);
    }
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::Stat(const std::wstring& path, ProviderEntry& entry, const ProviderCancelPoll&,
                                           std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    StatSink sink{&entry, false};
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    if (!Guard(L"Stat", [&] { return SehStat(t, p.c_str(), &sink, err.data(), &st); })) return CrashedStatus(error);
    if (st == kMcFsOk && !sink.seen) {
        error = p + L": the plugin returned no entry";
        return ProviderStatus::Failed;
    }
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::OpenRead(const std::wstring& path, std::unique_ptr<IReadStream>& stream,
                                               const ProviderCancelPoll&, std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!(table_.capabilities & kMcFsCapRead)) {
        error = L"this location can't be read";
        return ProviderStatus::NotSupported;
    }
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    void* handle = nullptr;
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    if (!Guard(L"OpenRead", [&] { return SehOpenRead(t, p.c_str(), &handle, err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) {
        error = ErrorText(err);
        return FromFs(st);
    }
    stream = std::make_unique<AdapterReadStream>(*this, handle);
    return ProviderStatus::Ok;
}

ProviderStatus PluginProviderAdapter::OpenWrite(const std::wstring& path, uint64_t sizeHint, uint64_t modifiedUtc,
                                                std::unique_ptr<IWriteStream>& stream, const ProviderCancelPoll&,
                                                std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!(table_.capabilities & kMcFsCapWrite)) {
        error = L"this location is read-only";
        return ProviderStatus::NotSupported;
    }
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    void* handle = nullptr;
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    if (!Guard(L"OpenWrite", [&] { return SehOpenWrite(t, p.c_str(), sizeHint, modifiedUtc, &handle, err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) {
        error = ErrorText(err);
        return FromFs(st);
    }
    stream = std::make_unique<AdapterWriteStream>(*this, handle);
    return ProviderStatus::Ok;
}

ProviderStatus PluginProviderAdapter::MakeDirectory(const std::wstring& path, const ProviderCancelPoll&,
                                                    std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!(table_.capabilities & kMcFsCapMkdir)) return ProviderStatus::NotSupported;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    if (!Guard(L"MakeDirectory", [&] { return SehMakeDirectory(t, p.c_str(), err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::Delete(const std::wstring& path, bool isDirectory, const ProviderCancelPoll&,
                                             std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!(table_.capabilities & kMcFsCapDelete)) return ProviderStatus::NotSupported;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    if (!Guard(L"Delete", [&] { return SehDelete(t, p.c_str(), isDirectory, err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::Rename(const std::wstring& from, const std::wstring& to,
                                             const ProviderCancelPoll&, std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!(table_.capabilities & kMcFsCapRename)) return ProviderStatus::NotSupported;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    std::wstring a = Normalize(from), b = Normalize(to);
    const McFsProvider* t = &table_;
    if (!Guard(L"Rename", [&] { return SehRename(t, a.c_str(), b.c_str(), err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

bool PluginProviderAdapter::HasPendingChanges() const {
    if (crashed_ || !table_.HasPendingChanges) return false;
    bool pending = false;
    const McFsProvider* t = &table_;
    if (!Guard(L"HasPendingChanges", [&] { return SehHasPending(t, &pending); })) return false;
    return pending;
}

ProviderStatus PluginProviderAdapter::Commit(const ProviderCancelPoll&, std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!table_.Commit) return ProviderStatus::Ok;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    const McFsProvider* t = &table_;
    if (!Guard(L"Commit", [&] { return SehCommit(t, err.data(), &st); })) return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::Reconnect(const ProviderCancelPoll&, std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!table_.Reconnect) return ProviderStatus::NotSupported;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    const McFsProvider* t = &table_;
    if (!Guard(L"Reconnect", [&] { return SehReconnect(t, err.data(), &st); })) return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::DownloadToFile(const std::wstring& path, const std::wstring& localPath,
                                                     uint64_t sizeHint, const ProviderByteProgress& progress,
                                                     std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!table_.DownloadToFile) return ProviderStatus::NotSupported;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    void* progressContext = const_cast<ProviderByteProgress*>(&progress);
    if (!Guard(L"DownloadToFile",
               [&] { return SehDownloadToFile(t, p.c_str(), localPath.c_str(), sizeHint, progressContext, err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::UploadFromFile(const std::wstring& localPath, const std::wstring& path,
                                                     uint64_t modifiedUtc, const ProviderByteProgress& progress,
                                                     std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!table_.UploadFromFile) return ProviderStatus::NotSupported;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    void* progressContext = const_cast<ProviderByteProgress*>(&progress);
    if (!Guard(L"UploadFromFile",
               [&] { return SehUploadFromFile(t, localPath.c_str(), p.c_str(), modifiedUtc, progressContext, err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::SetAttributes(const std::wstring& path, uint32_t attributes,
                                                    std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!(table_.capabilities & kMcFsCapAttributes) || !table_.SetAttributes) return ProviderStatus::NotSupported;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    std::wstring p = Normalize(path);
    const McFsProvider* t = &table_;
    if (!Guard(L"SetAttributes", [&] { return SehSetAttributes(t, p.c_str(), attributes, err.data(), &st); }))
        return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

ProviderStatus PluginProviderAdapter::Reload(std::wstring& error) {
    if (crashed_) return CrashedStatus(error);
    if (!table_.Reload) return ProviderStatus::Ok;
    std::vector<wchar_t> err(kErrorSize, L'\0');
    McFsStatus st = kMcFsFailed;
    const McFsProvider* t = &table_;
    if (!Guard(L"Reload", [&] { return SehReload(t, err.data(), &st); })) return CrashedStatus(error);
    if (st != kMcFsOk) error = ErrorText(err);
    return FromFs(st);
}

}  // namespace mc
