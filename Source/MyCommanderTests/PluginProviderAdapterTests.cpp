#include "TestFramework.h"
#include "TestFixtures.h"

#include "MemoryProvider.h"
#include "PluginProviderAdapter.h"
#include "ProviderCopy.h"

#include <algorithm>
#include <cwchar>

using mc::MemoryProvider;
using mc::PluginProviderAdapter;
using mc::ProviderStatus;

namespace {

// A plugin-side table implemented in "C" over a MemoryProvider, so the
// adapter is exercised exactly as a real plugin DLL would drive it.
struct FakePlugin {
    MemoryProvider tree{L"fake://"};
    int closes = 0;
    int openStreams = 0;
    bool crashList = false;
    bool listPathEntry = false;  // emits a name with a separator (must be dropped)
};

McFsStatus ToFs(ProviderStatus s) {
    switch (s) {
    case ProviderStatus::Ok: return kMcFsOk;
    case ProviderStatus::Cancelled: return kMcFsCancelled;
    case ProviderStatus::ConnectionLost: return kMcFsConnectionLost;
    case ProviderStatus::NotSupported: return kMcFsNotSupported;
    case ProviderStatus::NotFound: return kMcFsNotFound;
    default: return kMcFsFailed;
    }
}

void PutError(const std::wstring& text, wchar_t* error, int32_t size) {
    if (error && size > 0) wcsncpy_s(error, size, text.c_str(), _TRUNCATE);
}

void Emit(const mc::ProviderEntry& p, McFsEntrySink sink, void* sinkContext) {
    McFsEntry e{sizeof(McFsEntry)};
    e.name = p.name.c_str();
    e.isDirectory = p.isDirectory;
    e.isLink = p.isLink;
    e.size = p.size;
    e.modifiedUtc = p.modifiedUtc;
    sink(sinkContext, &e);
}

McFsStatus __stdcall FakeList(void* ctx, const wchar_t* dir, McFsEntrySink sink, void* sinkContext, wchar_t* error,
                              int32_t errorSize) {
    auto* f = static_cast<FakePlugin*>(ctx);
    if (f->crashList) {
        volatile int* nowhere = nullptr;
        *nowhere = 42;  // an access violation inside "plugin code"
    }
    std::vector<mc::ProviderEntry> entries;
    std::wstring err;
    ProviderStatus s = f->tree.List(dir, entries, {}, err);
    PutError(err, error, errorSize);
    for (const auto& e : entries) Emit(e, sink, sinkContext);
    if (f->listPathEntry) {
        mc::ProviderEntry evil;
        evil.name = L"..\\evil.txt";
        Emit(evil, sink, sinkContext);
    }
    return ToFs(s);
}

McFsStatus __stdcall FakeStat(void* ctx, const wchar_t* path, McFsEntrySink sink, void* sinkContext, wchar_t* error,
                              int32_t errorSize) {
    auto* f = static_cast<FakePlugin*>(ctx);
    mc::ProviderEntry e;
    std::wstring err;
    ProviderStatus s = f->tree.Stat(path, e, {}, err);
    PutError(err, error, errorSize);
    if (s == ProviderStatus::Ok) Emit(e, sink, sinkContext);
    return ToFs(s);
}

McFsStatus __stdcall FakeOpenRead(void* ctx, const wchar_t* path, void** stream, wchar_t* error, int32_t errorSize) {
    auto* f = static_cast<FakePlugin*>(ctx);
    std::unique_ptr<mc::IReadStream> in;
    std::wstring err;
    ProviderStatus s = f->tree.OpenRead(path, in, {}, err);
    PutError(err, error, errorSize);
    if (s == ProviderStatus::Ok) {
        *stream = in.release();
        ++f->openStreams;
    }
    return ToFs(s);
}

McFsStatus __stdcall FakeRead(void*, void* stream, void* buffer, uint32_t capacity, uint32_t* got, wchar_t* error,
                              int32_t errorSize) {
    size_t n = 0;
    std::wstring err;
    ProviderStatus s = static_cast<mc::IReadStream*>(stream)->Read(buffer, capacity, n, err);
    *got = static_cast<uint32_t>(n);
    PutError(err, error, errorSize);
    return ToFs(s);
}

void __stdcall FakeCloseRead(void* ctx, void* stream) {
    delete static_cast<mc::IReadStream*>(stream);
    --static_cast<FakePlugin*>(ctx)->openStreams;
}

McFsStatus __stdcall FakeOpenWrite(void* ctx, const wchar_t* path, uint64_t sizeHint, uint64_t modified, void** stream,
                                   wchar_t* error, int32_t errorSize) {
    auto* f = static_cast<FakePlugin*>(ctx);
    std::unique_ptr<mc::IWriteStream> out;
    std::wstring err;
    ProviderStatus s = f->tree.OpenWrite(path, sizeHint, modified, out, {}, err);
    PutError(err, error, errorSize);
    if (s == ProviderStatus::Ok) {
        *stream = out.release();
        ++f->openStreams;
    }
    return ToFs(s);
}

McFsStatus __stdcall FakeWrite(void*, void* stream, const void* data, uint32_t size, wchar_t* error, int32_t errorSize) {
    std::wstring err;
    ProviderStatus s = static_cast<mc::IWriteStream*>(stream)->Write(data, size, err);
    PutError(err, error, errorSize);
    return ToFs(s);
}

McFsStatus __stdcall FakeFinishWrite(void* ctx, void* stream, wchar_t* error, int32_t errorSize) {
    auto* out = static_cast<mc::IWriteStream*>(stream);
    std::wstring err;
    ProviderStatus s = out->Finish(err);
    delete out;
    --static_cast<FakePlugin*>(ctx)->openStreams;
    PutError(err, error, errorSize);
    return ToFs(s);
}

void __stdcall FakeAbortWrite(void* ctx, void* stream) {
    auto* out = static_cast<mc::IWriteStream*>(stream);
    out->Abort();
    delete out;
    --static_cast<FakePlugin*>(ctx)->openStreams;
}

McFsStatus __stdcall FakeMkdir(void* ctx, const wchar_t* path, wchar_t* error, int32_t errorSize) {
    std::wstring err;
    ProviderStatus s = static_cast<FakePlugin*>(ctx)->tree.MakeDirectory(path, {}, err);
    PutError(err, error, errorSize);
    return ToFs(s);
}

McFsStatus __stdcall FakeDelete(void* ctx, const wchar_t* path, bool isDirectory, wchar_t* error, int32_t errorSize) {
    std::wstring err;
    ProviderStatus s = static_cast<FakePlugin*>(ctx)->tree.Delete(path, isDirectory, {}, err);
    PutError(err, error, errorSize);
    return ToFs(s);
}

McFsStatus __stdcall FakeRename(void* ctx, const wchar_t* from, const wchar_t* to, wchar_t* error, int32_t errorSize) {
    std::wstring err;
    ProviderStatus s = static_cast<FakePlugin*>(ctx)->tree.Rename(from, to, {}, err);
    PutError(err, error, errorSize);
    return ToFs(s);
}

bool __stdcall FakeHasPending(void* ctx) { return static_cast<FakePlugin*>(ctx)->tree.HasPendingChanges(); }

McFsStatus __stdcall FakeCommit(void* ctx, wchar_t*, int32_t) {
    std::wstring err;
    return ToFs(static_cast<FakePlugin*>(ctx)->tree.Commit({}, err));
}

McFsStatus __stdcall FakeSetAttributes(void* ctx, const wchar_t* path, uint32_t attributes, wchar_t* error,
                                       int32_t errorSize) {
    std::wstring err;
    ProviderStatus st = static_cast<FakePlugin*>(ctx)->tree.SetAttributes(path, attributes, err);
    PutError(err, error, errorSize);
    return ToFs(st);
}

McFsStatus __stdcall FakeReload(void* ctx, wchar_t*, int32_t) {
    std::wstring err;
    return ToFs(static_cast<FakePlugin*>(ctx)->tree.Reload(err));
}

void __stdcall FakeClose(void* ctx) { ++static_cast<FakePlugin*>(ctx)->closes; }

McFsProvider Table(FakePlugin& f) {
    McFsProvider t{};
    t.structSize = sizeof(McFsProvider);
    t.context = &f;
    t.capabilities = kMcFsCapRead | kMcFsCapWrite | kMcFsCapMkdir | kMcFsCapDelete | kMcFsCapRename;
    t.displayRoot = L"fake://host";
    t.kind = L"fake";
    t.rootPath = L"/";
    t.separator = L'/';
    t.List = &FakeList;
    t.Stat = &FakeStat;
    t.OpenRead = &FakeOpenRead;
    t.Read = &FakeRead;
    t.CloseRead = &FakeCloseRead;
    t.OpenWrite = &FakeOpenWrite;
    t.Write = &FakeWrite;
    t.FinishWrite = &FakeFinishWrite;
    t.AbortWrite = &FakeAbortWrite;
    t.MakeDirectory = &FakeMkdir;
    t.Delete = &FakeDelete;
    t.Rename = &FakeRename;
    t.HasPendingChanges = &FakeHasPending;
    t.Commit = &FakeCommit;
    t.Close = &FakeClose;
    return t;
}

}  // namespace

TEST_CASE(PluginProviderAdapter_Validate_RequiresEntryPointsForDeclaredCapabilities_EXT002) {
    FakePlugin f;
    McFsProvider t = Table(f);
    std::wstring problem;
    CHECK(PluginProviderAdapter::Validate(t, problem));
    McFsProvider noWrite = t;
    noWrite.FinishWrite = nullptr;
    CHECK(!PluginProviderAdapter::Validate(noWrite, problem) && !problem.empty());
    McFsProvider readOnly = noWrite;
    readOnly.capabilities = kMcFsCapRead;
    CHECK(PluginProviderAdapter::Validate(readOnly, problem));
    McFsProvider noClose = t;
    noClose.Close = nullptr;
    CHECK(!PluginProviderAdapter::Validate(noClose, problem));
    McFsProvider halfCommit = t;
    halfCommit.Commit = nullptr;
    CHECK(!PluginProviderAdapter::Validate(halfCommit, problem));
    McFsProvider tooSmall = t;
    tooSmall.structSize = 8;
    CHECK(!PluginProviderAdapter::Validate(tooSmall, problem));
}

TEST_CASE(PluginProviderAdapter_PathsAndListing_FollowTheTable_EXT002) {
    FakePlugin f;
    f.tree.AddFile(L"/docs/a.txt", "aa", 5);
    {
        PluginProviderAdapter p(Table(f), L"test.plugin");
        CHECK(p.DisplayRoot() == L"fake://host" && p.Kind() == L"fake");
        CHECK(p.Join(L"/", L"docs") == L"/docs" && p.Join(L"/docs", L"a.txt") == L"/docs/a.txt");
        CHECK(p.Parent(L"/docs/a.txt") == L"/docs" && p.Parent(L"/docs") == L"/" && p.IsRoot(L"/"));
        std::vector<mc::ProviderEntry> entries;
        std::wstring err;
        CHECK(p.List(L"/docs", entries, {}, err) == ProviderStatus::Ok);
        CHECK(entries.size() == 1 && entries[0].name == L"a.txt" && entries[0].size == 2 && entries[0].modifiedUtc == 5);
        CHECK(p.List(L"/missing", entries, {}, err) == ProviderStatus::NotFound && !err.empty());
        CHECK((p.Capabilities() & mc::CapWrite) != 0);
    }
    CHECK(f.closes == 1);  // Close exactly once, on destruction
}

TEST_CASE(PluginProviderAdapter_EntryWithASeparator_IsDropped_EXT002) {
    FakePlugin f;
    f.tree.AddFile(L"/ok.txt", "x");
    f.listPathEntry = true;
    PluginProviderAdapter p(Table(f), L"test.plugin");
    std::vector<mc::ProviderEntry> entries;
    std::wstring err;
    CHECK(p.List(L"/", entries, {}, err) == ProviderStatus::Ok);
    CHECK(entries.size() == 1 && entries[0].name == L"ok.txt");
}

TEST_CASE(PluginProviderAdapter_CopyThroughTheAbi_BothDirections_EXT002) {
    test::TempDir dir;
    FakePlugin f;
    std::string data(200000, 'q');
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<char>(i * 13);
    f.tree.AddFile(L"/in/data.bin", data, 777);
    {
        PluginProviderAdapter p(Table(f), L"test.plugin");
        mc::MemoryProvider local;
        mc::OperationOutcome down = mc::ProviderCopyItems(p, L"/", {L"in"}, local, L"/", {});
        CHECK(down.succeeded == 1 && local.Content(L"/in/data.bin") == data && local.Modified(L"/in/data.bin") == 777);
        mc::OperationOutcome up = mc::ProviderCopyItemAs(local, L"/in", L"data.bin", p, L"/in", L"copy.bin", {});
        CHECK(up.succeeded == 1 && f.tree.Content(L"/in/copy.bin") == data);
        mc::OperationOutcome gone = mc::ProviderDeleteItems(p, L"/", {L"in"}, {});
        CHECK(gone.succeeded == 1 && !f.tree.Exists(L"/in"));
    }
    CHECK(f.openStreams == 0);  // every stream closed through the table
    CHECK(f.closes == 1);
}

TEST_CASE(PluginProviderAdapter_CancelledWrite_AbortsThroughTheTable_EXT002_FOP012) {
    FakePlugin f;
    mc::MemoryProvider source;
    source.AddFile(L"/f.bin", std::string(300000, 'z'));
    f.tree.AddFile(L"/f.bin", "old");
    {
        PluginProviderAdapter p(Table(f), L"test.plugin");
        int chunks = 0;
        mc::ProviderOpHandlers h;
        h.onConflict = [](const std::filesystem::path&) {
            mc::ConflictAskResult r;
            r.choice = mc::ConflictChoice::Overwrite;
            return r;
        };
        h.onByteProgress = [&](uint64_t, uint64_t) { ++chunks; };
        h.cancelPoll = [&] { return chunks >= 2; };
        mc::OperationOutcome o = mc::ProviderCopyItems(source, L"/", {L"f.bin"}, p, L"/", h);
        CHECK(o.cancelled);
    }
    CHECK(f.tree.Content(L"/f.bin") == "old");
    CHECK(f.openStreams == 0);
}

TEST_CASE(PluginProviderAdapter_PendingChangesAndCommit_EXT002) {
    FakePlugin f;
    PluginProviderAdapter p(Table(f), L"test.plugin");
    CHECK(!p.HasPendingChanges());
    std::wstring err;
    CHECK(p.MakeDirectory(L"/new", {}, err) == ProviderStatus::Ok);
    CHECK(p.HasPendingChanges());
    CHECK(p.Commit({}, err) == ProviderStatus::Ok);
    CHECK(!p.HasPendingChanges() && f.tree.CommitCount() == 1);
}

TEST_CASE(PluginProviderAdapter_ScopeBracketsEveryCall_EXT002) {
    FakePlugin f;
    int depth = 0, maxDepth = 0, entered = 0;
    {
        PluginProviderAdapter p(Table(f), L"test.plugin", [&](bool entering) {
            depth += entering ? 1 : -1;
            if (entering) ++entered;
            maxDepth = (std::max)(maxDepth, depth);
        });
        std::vector<mc::ProviderEntry> entries;
        std::wstring err;
        p.List(L"/", entries, {}, err);
        mc::ProviderEntry e;
        p.Stat(L"/", e, {}, err);
    }
    CHECK(depth == 0 && maxDepth == 1 && entered == 3);  // List, Stat, Close
}

TEST_CASE(PluginProviderAdapter_PluginCrash_IsCaughtReportedAndFailsLaterCalls_EXT002_PLUG009) {
    FakePlugin f;
    f.crashList = true;
    std::vector<std::wstring> reports;
    {
        PluginProviderAdapter p(Table(f), L"test.plugin", {}, [&](const std::wstring& what) { reports.push_back(what); });
        std::vector<mc::ProviderEntry> entries;
        std::wstring err;
        CHECK(p.List(L"/", entries, {}, err) == ProviderStatus::Failed && !err.empty());
        CHECK(p.Crashed());
        mc::ProviderEntry e;
        CHECK(p.Stat(L"/", e, {}, err) == ProviderStatus::Failed);
        CHECK(p.Capabilities() == 0);
    }
    CHECK(reports.size() == 1);
    CHECK(f.closes == 0);  // a crashed plugin is never called again, not even Close
}

TEST_CASE(PluginProviderAdapter_AttributesReloadAndLeaveFlag_EXT002) {
    FakePlugin f;
    f.tree.SetCapabilities(mc::CapRead | mc::CapWrite | mc::CapMkdir | mc::CapDelete | mc::CapRename | mc::CapAttributes);
    f.tree.AddFile(L"/a.txt", "a");
    McFsProvider t = Table(f);
    std::wstring problem;
    McFsProvider needsSetter = t;
    needsSetter.capabilities |= kMcFsCapAttributes;
    CHECK(!PluginProviderAdapter::Validate(needsSetter, problem));  // the bit without SetAttributes
    t.capabilities |= kMcFsCapAttributes;
    t.SetAttributes = &FakeSetAttributes;
    t.Reload = &FakeReload;
    t.confirmOnLeave = true;
    CHECK(PluginProviderAdapter::Validate(t, problem));
    PluginProviderAdapter p(t, L"test.plugin");
    CHECK((p.Capabilities() & mc::CapAttributes) != 0 && p.AsksBeforeLeaving());
    std::wstring err;
    CHECK(p.SetAttributes(L"/a.txt", FILE_ATTRIBUTE_READONLY, err) == ProviderStatus::Ok);
    CHECK(f.tree.Attributes(L"/a.txt") == FILE_ATTRIBUTE_READONLY);
    CHECK(p.Reload(err) == ProviderStatus::Ok && f.tree.ReloadCount() == 1);

    FakePlugin plain;
    PluginProviderAdapter q(Table(plain), L"test.plugin");
    CHECK(!q.AsksBeforeLeaving() && (q.Capabilities() & mc::CapAttributes) == 0);
    CHECK(q.Reload(err) == ProviderStatus::Ok);  // no Reload entry point: nothing to do
    CHECK(q.SetAttributes(L"/x", 0, err) == ProviderStatus::NotSupported);
}
