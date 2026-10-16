#include "TestFramework.h"
#include "TestFixtures.h"

#include "LocalProvider.h"
#include "MemoryProvider.h"
#include "PauseToken.h"
#include "ProviderCopy.h"

#include <atomic>

using mc::ConflictAskResult;
using mc::ConflictChoice;
using mc::ErrorAskResult;
using mc::ErrorChoice;
using mc::MemoryProvider;
using mc::OperationOutcome;
using mc::ProviderOpHandlers;

namespace {

std::string Big(size_t n) {
    std::string s(n, '\0');
    for (size_t i = 0; i < n; ++i) s[i] = static_cast<char>((i * 7919) >> 3);
    return s;
}

ProviderOpHandlers Answer(ConflictChoice choice, bool all = false, std::wstring renameTo = L"") {
    ProviderOpHandlers h;
    h.onConflict = [=](const std::filesystem::path&) {
        ConflictAskResult r;
        r.choice = choice;
        r.applyToAll = all;
        r.renameTo = renameTo;
        return r;
    };
    return h;
}

}  // namespace

TEST_CASE(ProviderCopy_Tree_BetweenTwoProviders_CopiesContentAndTimes_EXT002) {
    MemoryProvider a(L"a://"), b(L"b://");
    a.AddFile(L"/src/x.txt", "x-content", 111);
    a.AddFile(L"/src/sub/y.txt", "y", 222);
    a.AddDirectory(L"/src/empty");
    b.AddDirectory(L"/dst");
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"src"}, b, L"/dst", {});
    CHECK(o.succeeded == 1 && o.failed.empty() && !o.cancelled);
    CHECK(b.Content(L"/dst/src/x.txt") == "x-content");
    CHECK(b.Content(L"/dst/src/sub/y.txt") == "y");
    CHECK(b.IsDirectory(L"/dst/src/empty"));
    CHECK(b.Modified(L"/dst/src/x.txt") == 111);
    CHECK(a.Content(L"/src/x.txt") == "x-content");  // copy keeps the source
}

TEST_CASE(ProviderCopy_ByteProgress_ReportsChunksUpToTheFileSize_EXT002) {
    MemoryProvider a, b;
    a.AddFile(L"/f.bin", Big(200000));
    uint64_t last = 0, total = 0;
    int calls = 0;
    ProviderOpHandlers h;
    h.onByteProgress = [&](uint64_t done, uint64_t of) {
        ++calls;
        last = done;
        total = of;
    };
    mc::ProviderCopyItems(a, L"/", {L"f.bin"}, b, L"/", h);
    CHECK(calls >= 4);  // 0 + 4 chunks of 64 KB
    CHECK(last == 200000 && total == 200000);
}

TEST_CASE(ProviderCopy_Conflict_OverwriteSkipAndRename_EXT002_FOP010) {
    MemoryProvider a, b;
    a.AddFile(L"/f.txt", "new");
    b.AddFile(L"/f.txt", "old");
    OperationOutcome skip = mc::ProviderCopyItems(a, L"/", {L"f.txt"}, b, L"/", Answer(ConflictChoice::Skip));
    CHECK(skip.skipped == 1 && skip.succeeded == 0 && b.Content(L"/f.txt") == "old");
    OperationOutcome renamed =
        mc::ProviderCopyItems(a, L"/", {L"f.txt"}, b, L"/", Answer(ConflictChoice::Rename, false, L"f (2).txt"));
    CHECK(renamed.succeeded == 1 && b.Content(L"/f (2).txt") == "new" && b.Content(L"/f.txt") == "old");
    OperationOutcome over = mc::ProviderCopyItems(a, L"/", {L"f.txt"}, b, L"/", Answer(ConflictChoice::Overwrite));
    CHECK(over.succeeded == 1 && b.Content(L"/f.txt") == "new");
    OperationOutcome cancel = mc::ProviderCopyItems(a, L"/", {L"f.txt"}, b, L"/", Answer(ConflictChoice::Cancel));
    CHECK(cancel.cancelled);
}

TEST_CASE(ProviderCopy_Conflict_ApplyToAllAsksOnceAtAnyDepth_EXT002_FOP010) {
    MemoryProvider a, b;
    a.AddFile(L"/d/1.txt", "n1");
    a.AddFile(L"/d/sub/2.txt", "n2");
    a.AddFile(L"/d/3.txt", "n3");
    b.AddFile(L"/d/1.txt", "o1");
    b.AddFile(L"/d/sub/2.txt", "o2");
    b.AddFile(L"/d/3.txt", "o3");
    int asked = 0;
    ProviderOpHandlers h;
    h.onConflict = [&](const std::filesystem::path&) {
        ++asked;
        ConflictAskResult r;
        r.choice = ConflictChoice::Overwrite;
        r.applyToAll = true;
        return r;
    };
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"d"}, b, L"/", h);
    CHECK(asked == 1);
    CHECK(o.succeeded == 1);  // the folder merged into the existing one
    CHECK(b.Content(L"/d/1.txt") == "n1" && b.Content(L"/d/sub/2.txt") == "n2" && b.Content(L"/d/3.txt") == "n3");
}

TEST_CASE(ProviderCopy_FileOntoFolder_FailsWithoutTouchingIt_EXT002) {
    MemoryProvider a, b;
    a.AddFile(L"/x", "file");
    b.AddFile(L"/x/inner.txt", "keep");
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"x"}, b, L"/", Answer(ConflictChoice::Overwrite));
    CHECK(o.succeeded == 0 && o.failed.size() == 1);
    CHECK(b.Content(L"/x/inner.txt") == "keep");
}

TEST_CASE(ProviderCopy_CancelMidFile_LeavesTheOldTargetIntact_EXT002_FOP012) {
    MemoryProvider a, b;
    a.AddFile(L"/f.bin", Big(300000));
    b.AddFile(L"/f.bin", "old");
    int progress = 0;
    ProviderOpHandlers h = Answer(ConflictChoice::Overwrite);
    h.onByteProgress = [&](uint64_t, uint64_t) { ++progress; };
    h.cancelPoll = [&] { return progress >= 2; };
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"f.bin"}, b, L"/", h);
    CHECK(o.cancelled && o.succeeded == 0);
    CHECK(b.Content(L"/f.bin") == "old");
}

TEST_CASE(ProviderCopy_CancelMidFile_ToLocalLeavesNoPartFile_EXT002_FOP012) {
    test::TempDir dir;
    MemoryProvider a;
    mc::LocalProvider local;
    a.AddFile(L"/f.bin", Big(300000));
    int progress = 0;
    ProviderOpHandlers h;
    h.onByteProgress = [&](uint64_t, uint64_t) { ++progress; };
    h.cancelPoll = [&] { return progress >= 2; };
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"f.bin"}, local, dir.Path().wstring(), h);
    CHECK(o.cancelled);
    CHECK(!std::filesystem::exists(dir.Path() / L"f.bin"));
    CHECK(!std::filesystem::exists(dir.Path() / (std::wstring(L"f.bin") + mc::kLocalPartSuffix)));
}

TEST_CASE(ProviderCopy_WriteFailure_NoHandlerRecordsIt_SkipCountsIt_EXT002_FOP010) {
    MemoryProvider a, b;
    a.AddFile(L"/f.bin", Big(200000));
    a.AddFile(L"/g.txt", "g");
    b.AddFile(L"/f.bin", "old");
    b.FailNextWriteAfter(1000);
    OperationOutcome o =
        mc::ProviderCopyItems(a, L"/", {L"f.bin", L"g.txt"}, b, L"/", Answer(ConflictChoice::Overwrite));
    CHECK(o.failed.size() == 1 && o.failed[0].first == L"f.bin");
    CHECK(o.succeeded == 1);
    CHECK(b.Content(L"/f.bin") == "old");  // the failed write never replaced it

    b.FailNextWriteAfter(1000);
    ProviderOpHandlers h = Answer(ConflictChoice::Overwrite);
    int asked = 0;
    h.onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        ++asked;
        ErrorAskResult r;
        r.choice = ErrorChoice::Skip;
        return r;
    };
    o = mc::ProviderCopyItems(a, L"/", {L"f.bin"}, b, L"/", h);
    CHECK(asked == 1 && o.skipped == 1 && o.failed.empty());

    b.FailNextWriteAfter(1000);
    h.onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        ErrorAskResult r;
        r.choice = ErrorChoice::Retry;  // the injected fault fires once, so the retry succeeds
        return r;
    };
    o = mc::ProviderCopyItems(a, L"/", {L"f.bin"}, b, L"/", h);
    CHECK(o.succeeded == 1 && b.Content(L"/f.bin") == a.Content(L"/f.bin"));
}

TEST_CASE(ProviderCopy_ConnectionLost_RetryReconnectsAndCompletes_EXT002_SFTP008) {
    MemoryProvider a, server(L"sftp://test");
    a.AddFile(L"/f.bin", Big(200000));
    server.DropConnectionAfter(70000);
    std::wstring seenError;
    ProviderOpHandlers h;
    h.onError = [&](const std::wstring&, const std::wstring& error, DWORD) {
        seenError = error;
        ErrorAskResult r;
        r.choice = ErrorChoice::Retry;
        return r;
    };
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"f.bin"}, server, L"/", h);
    CHECK(seenError.find(L"connection lost") != std::wstring::npos);
    CHECK(server.ReconnectCount() == 1 && server.Connected());
    CHECK(o.succeeded == 1 && server.Content(L"/f.bin") == a.Content(L"/f.bin"));
}

TEST_CASE(ProviderCopy_LinkToFolder_IsNeverFollowed_EXT002_SEC006) {
    MemoryProvider a, b;
    a.AddFile(L"/outside/secret.txt", "s");
    a.AddFile(L"/d/real.txt", "r");
    a.AddLink(L"/d/lnk", L"/outside", true);
    std::vector<std::wstring> notes;
    ProviderOpHandlers h;
    h.onNote = [&](const std::wstring& n) { notes.push_back(n); };
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"d"}, b, L"/", h);
    CHECK(b.Content(L"/d/real.txt") == "r");
    CHECK(!b.Exists(L"/d/lnk") && !b.Exists(L"/d/lnk/secret.txt"));
    CHECK(notes.size() == 1 && o.skipped == 1);
}

TEST_CASE(ProviderCopy_FolderIntoItself_IsRefused_EXT002) {
    MemoryProvider a;
    a.AddFile(L"/d/f.txt", "f");
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"d"}, a, L"/d", {});
    CHECK(o.succeeded == 0 && o.failed.size() == 1);
    CHECK(!a.Exists(L"/d/d"));
    OperationOutcome onto = mc::ProviderCopyItems(a, L"/d", {L"f.txt"}, a, L"/d", Answer(ConflictChoice::Overwrite));
    CHECK(onto.failed.size() == 1 && a.Content(L"/d/f.txt") == "f");
}

TEST_CASE(ProviderCopy_CopyItemAs_UsesTheNewName_EXT002_FOP019) {
    MemoryProvider a;
    a.AddFile(L"/f.txt", "f");
    OperationOutcome o = mc::ProviderCopyItemAs(a, L"/", L"f.txt", a, L"/", L"g.txt", {});
    CHECK(o.succeeded == 1 && a.Content(L"/g.txt") == "f" && a.Content(L"/f.txt") == "f");
}

TEST_CASE(ProviderMove_SameProvider_RenamesInsteadOfCopying_EXT002) {
    MemoryProvider a;
    a.AddFile(L"/d/f.txt", "f");
    a.AddDirectory(L"/e");
    a.FailNextWriteAfter(1);  // any streamed copy would fail
    OperationOutcome o = mc::ProviderMoveItems(a, L"/", {L"d"}, a, L"/e", {});
    CHECK(o.succeeded == 1 && o.failed.empty());
    CHECK(a.Content(L"/e/d/f.txt") == "f" && !a.Exists(L"/d"));
}

TEST_CASE(ProviderMove_SameProvider_FileConflictOverwriteReplacesTarget_EXT002) {
    MemoryProvider a;
    a.AddFile(L"/f.txt", "new");
    a.AddFile(L"/e/f.txt", "old");
    OperationOutcome o = mc::ProviderMoveItems(a, L"/", {L"f.txt"}, a, L"/e", Answer(ConflictChoice::Overwrite));
    CHECK(o.succeeded == 1 && a.Content(L"/e/f.txt") == "new" && !a.Exists(L"/f.txt"));
}

TEST_CASE(ProviderMove_AcrossProviders_DeletesTheSourceOnlyWhenEverythingCopied_EXT002) {
    MemoryProvider a, b;
    a.AddFile(L"/d/1.txt", "1");
    a.AddFile(L"/d/2.txt", "2");
    b.AddFile(L"/d/2.txt", "keep");
    OperationOutcome partial = mc::ProviderMoveItems(a, L"/", {L"d"}, b, L"/", Answer(ConflictChoice::Skip));
    CHECK(partial.succeeded == 0);
    CHECK(a.Exists(L"/d/1.txt") && a.Exists(L"/d/2.txt"));  // a skipped child keeps the whole source
    CHECK(b.Content(L"/d/2.txt") == "keep");

    OperationOutcome full = mc::ProviderMoveItems(a, L"/", {L"d"}, b, L"/", Answer(ConflictChoice::Overwrite));
    CHECK(full.succeeded == 1 && !a.Exists(L"/d"));
    CHECK(b.Content(L"/d/1.txt") == "1" && b.Content(L"/d/2.txt") == "2");
}

TEST_CASE(ProviderDelete_Tree_RemovesChildrenFirstAndLinksOnly_EXT002_SEC006) {
    MemoryProvider a;
    a.AddFile(L"/outside/keep.txt", "k");
    a.AddFile(L"/d/sub/f.txt", "f");
    a.AddLink(L"/d/lnk", L"/outside", true);
    OperationOutcome o = mc::ProviderDeleteItems(a, L"/", {L"d"}, {});
    CHECK(o.succeeded == 1 && !a.Exists(L"/d"));
    CHECK(a.Content(L"/outside/keep.txt") == "k");
}

TEST_CASE(ProviderDelete_FailedChild_KeepsItsParent_EXT002) {
    MemoryProvider a;
    a.AddFile(L"/d/a.txt", "a");
    a.AddFile(L"/d/b.txt", "b");
    a.FailPath(L"/d/b.txt");
    OperationOutcome o = mc::ProviderDeleteItems(a, L"/", {L"d"}, {});
    CHECK(o.succeeded == 0 && o.failed.size() == 1);
    CHECK(a.Exists(L"/d/b.txt") && !a.Exists(L"/d/a.txt"));
}

TEST_CASE(ProviderCopy_ItemStartCanCancelBeforeTheNextItem_EXT002) {
    MemoryProvider a, b;
    a.AddFile(L"/1.txt", "1");
    a.AddFile(L"/2.txt", "2");
    ProviderOpHandlers h;
    h.onItemStart = [](const std::wstring&, int index, int) { return index == 0; };
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"1.txt", L"2.txt"}, b, L"/", h);
    CHECK(o.cancelled && o.succeeded == 1 && !b.Exists(L"/2.txt"));
}

TEST_CASE(ProviderCopy_LocalToMemoryAndBack_RoundTripsUnicodeNames_EXT002) {
    test::TempDir dir;
    mc::LocalProvider local;
    MemoryProvider server(L"sftp://x");
    std::string data = Big(150000);
    std::filesystem::create_directories(dir.Path() / L"src" / L"ünï");
    test::WriteFileContent(dir.Path() / L"src" / L"ünï" / L"日本.bin", data);
    OperationOutcome up = mc::ProviderCopyItems(local, (dir.Path() / L"src").wstring(), {L"ünï"}, server, L"/", {});
    CHECK(up.succeeded == 1 && server.Content(L"/ünï/日本.bin") == data);
    std::filesystem::create_directory(dir.Path() / L"back");
    OperationOutcome down = mc::ProviderCopyItems(server, L"/", {L"ünï"}, local, (dir.Path() / L"back").wstring(), {});
    CHECK(down.succeeded == 1);
    CHECK(test::ReadFileContent(dir.Path() / L"back" / L"ünï" / L"日本.bin") == data);
}

TEST_CASE(ProviderCopy_PauseInsideTheCancelPoll_DelaysButCompletes_EXT002_FOP015) {
    MemoryProvider a, b;
    a.AddFile(L"/f.bin", Big(400000));
    mc::PauseToken token;
    token.Pause();
    struct Resumer {
        mc::PauseToken* token;
        std::atomic<bool> resumed{false};
        static DWORD WINAPI Run(void* self) {
            auto* r = static_cast<Resumer*>(self);
            Sleep(80);
            r->resumed = true;
            r->token->Resume();
            return 0;
        }
    } resumer{&token};
    HANDLE thread = CreateThread(nullptr, 0, &Resumer::Run, &resumer, 0, nullptr);
    ProviderOpHandlers h;
    h.cancelPoll = [&] { return !token.WaitWhilePaused([] { return false; }, {}, 10); };
    OperationOutcome o = mc::ProviderCopyItems(a, L"/", {L"f.bin"}, b, L"/", h);
    WaitForSingleObject(thread, INFINITE);
    CloseHandle(thread);
    CHECK(resumer.resumed);
    CHECK(o.succeeded == 1 && b.Content(L"/f.bin") == a.Content(L"/f.bin"));
}

TEST_CASE(MemoryProvider_PathsAndListing_EXT002) {
    MemoryProvider p;
    CHECK(p.Join(L"/", L"a") == L"/a");
    CHECK(p.Join(L"/a", L"b") == L"/a/b");
    CHECK(p.Parent(L"/a/b") == L"/a");
    CHECK(p.Parent(L"/a") == L"/");
    CHECK(p.IsRoot(L"/"));
    p.AddFile(L"/a/b.txt", "bb");
    p.AddLink(L"/a/l", L"/a/b.txt", false);
    std::vector<mc::ProviderEntry> entries;
    std::wstring err;
    CHECK(p.List(L"/a", entries, {}, err) == mc::ProviderStatus::Ok);
    CHECK(entries.size() == 2);
    CHECK(p.Content(L"/a/l") == "bb");
    CHECK(p.List(L"/missing", entries, {}, err) == mc::ProviderStatus::NotFound);
}

// --- CapFileTransfer (whole-file transfers, the FTP/SFTP shape) ---------------

TEST_CASE(ProviderCopy_TransferProvider_ToLocal_UsesDownloadToFile_EXT002) {
    test::TempDir dir;
    MemoryProvider server(L"ftp://x");
    server.EnableFileTransfer(true);
    std::string data = Big(150000);
    server.AddFile(L"/d/f.bin", data, 4242);
    mc::LocalProvider local;
    OperationOutcome o = mc::ProviderCopyItems(server, L"/", {L"d"}, local, dir.Path().wstring(), {});
    CHECK(o.succeeded == 1 && server.DownloadCount() == 1);
    CHECK(test::ReadFileContent(dir.Path() / L"d" / L"f.bin") == data);
    CHECK(!std::filesystem::exists(dir.Path() / L"d" / (std::wstring(L"f.bin") + mc::kLocalPartSuffix)));
    mc::ProviderEntry e;
    std::wstring err;
    local.Stat((dir.Path() / L"d" / L"f.bin").wstring(), e, {}, err);
    CHECK(e.modifiedUtc == 4242);
}

TEST_CASE(ProviderCopy_TransferProvider_FromLocal_UsesUploadFromFile_EXT002) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"up.txt", "upload me");
    MemoryProvider server(L"sftp://x");
    server.EnableFileTransfer(true);
    mc::LocalProvider local;
    OperationOutcome o = mc::ProviderCopyItems(local, dir.Path().wstring(), {L"up.txt"}, server, L"/", {});
    CHECK(o.succeeded == 1 && server.UploadCount() == 1 && server.Content(L"/up.txt") == "upload me");
}

TEST_CASE(ProviderCopy_TransferBetweenTwoProviders_StagesAndCleansUp_EXT002) {
    MemoryProvider ftp(L"ftp://a"), zip(L"zip://b");
    ftp.EnableFileTransfer(true);
    std::string data = Big(100000);
    ftp.AddFile(L"/f.bin", data);
    OperationOutcome o = mc::ProviderCopyItems(ftp, L"/", {L"f.bin"}, zip, L"/", {});
    CHECK(o.succeeded == 1 && ftp.DownloadCount() == 1 && zip.Content(L"/f.bin") == data);
    OperationOutcome back = mc::ProviderCopyItemAs(zip, L"/", L"f.bin", ftp, L"/", L"g.bin", {});
    CHECK(back.succeeded == 1 && ftp.UploadCount() == 1 && ftp.Content(L"/g.bin") == data);
    wchar_t temp[MAX_PATH];
    GetTempPathW(MAX_PATH, temp);
    bool leftover = false;
    std::wstring prefix = L"mc-stage-" + std::to_wstring(GetCurrentProcessId()) + L"-";
    for (const auto& item : std::filesystem::directory_iterator(temp))
        leftover |= item.path().filename().wstring().rfind(prefix, 0) == 0;
    CHECK(!leftover);
}

TEST_CASE(ProviderCopy_TransferCancelled_LeavesNoLocalFileOrPart_EXT002_FOP012) {
    test::TempDir dir;
    MemoryProvider server;
    server.EnableFileTransfer(true);
    server.AddFile(L"/f.bin", Big(400000));
    test::WriteFileContent(dir.Path() / L"f.bin", "old");
    mc::LocalProvider local;
    int progress = 0;
    ProviderOpHandlers h = Answer(ConflictChoice::Overwrite);
    h.onByteProgress = [&](uint64_t, uint64_t) { ++progress; };
    h.cancelPoll = [&] { return progress >= 2; };
    OperationOutcome o = mc::ProviderCopyItems(server, L"/", {L"f.bin"}, local, dir.Path().wstring(), h);
    CHECK(o.cancelled);
    CHECK(test::ReadFileContent(dir.Path() / L"f.bin") == "old");
    CHECK(!std::filesystem::exists(dir.Path() / (std::wstring(L"f.bin") + mc::kLocalPartSuffix)));
}

TEST_CASE(ProviderCopy_TransferUploadLost_RetryReconnectsAndKeepsTheOldFileMeanwhile_EXT002_SFTP008) {
    test::TempDir dir;
    test::WriteFileContent(dir.Path() / L"f.bin", Big(200000));
    MemoryProvider server;
    server.EnableFileTransfer(true);
    server.AddFile(L"/f.bin", "old");
    server.DropConnectionAfter(70000);
    mc::LocalProvider local;
    std::string seenContentAtError;
    ProviderOpHandlers h = Answer(ConflictChoice::Overwrite);
    h.onError = [&](const std::wstring&, const std::wstring&, DWORD) {
        seenContentAtError = server.Content(L"/f.bin");  // still connected for reads in the fake
        ErrorAskResult r;
        r.choice = ErrorChoice::Retry;
        return r;
    };
    OperationOutcome o = mc::ProviderCopyItems(local, dir.Path().wstring(), {L"f.bin"}, server, L"/", h);
    CHECK(seenContentAtError == "old");
    CHECK(o.succeeded == 1 && server.ReconnectCount() == 1);
    CHECK(server.Content(L"/f.bin") == test::ReadFileContent(dir.Path() / L"f.bin"));
}

// --- Shift+F5 inside a provider (FOP-019 / ZIP-011) --------------------------

TEST_CASE(ResolveProviderCopyAs_NamesPathsAndFolders_EXT002_ZIP011) {
    MemoryProvider p;
    p.AddFile(L"/a.txt", "a");
    p.AddFile(L"/dir/x.txt", "x");
    p.AddFile(L"/dir/sub/y.txt", "y");
    auto r = mc::ResolveProviderCopyAs(p, L"/", L"a.txt", L"b.txt");
    CHECK(r.problem == mc::CopyAsProblem::None && r.targetDir == L"/" && r.newName == L"b.txt");
    r = mc::ResolveProviderCopyAs(p, L"/dir", L"x.txt", L"z.txt");
    CHECK(r.targetDir == L"/dir" && r.newName == L"z.txt");
    r = mc::ResolveProviderCopyAs(p, L"/", L"a.txt", L"new\\deep\\c.txt");
    CHECK(r.targetDir == L"/new/deep" && r.newName == L"c.txt");
    r = mc::ResolveProviderCopyAs(p, L"/", L"a.txt", L"dir");
    CHECK(r.targetDir == L"/dir" && r.newName == L"a.txt");  // an existing folder receives the copy
    r = mc::ResolveProviderCopyAs(p, L"/", L"a.txt", L"newdir/");
    CHECK(r.targetDir == L"/newdir" && r.newName == L"a.txt");  // a trailing slash means "into"
    r = mc::ResolveProviderCopyAs(p, L"/", L"a.txt", L"  \"q.txt\" ");
    CHECK(r.problem == mc::CopyAsProblem::None && r.newName == L"q.txt");
    CHECK(mc::ResolveProviderCopyAs(p, L"/", L"a.txt", L"   ").problem == mc::CopyAsProblem::Empty);
    for (const wchar_t* bad : {L"../out.txt", L"/abs.txt", L"C:\\x.txt", L"a/./b.txt", L"x//b.txt"})
        CHECK(mc::ResolveProviderCopyAs(p, L"/", L"a.txt", bad).problem == mc::CopyAsProblem::Unsafe);
    CHECK(mc::ResolveProviderCopyAs(p, L"/", L"a.txt", L"a.txt").problem == mc::CopyAsProblem::SameItem);
    CHECK(mc::ResolveProviderCopyAs(p, L"/", L"dir", L"dir").problem == mc::CopyAsProblem::SameItem);
    CHECK(mc::ResolveProviderCopyAs(p, L"/", L"dir", L"dir/sub/dir2").problem == mc::CopyAsProblem::IntoItself);
    r = mc::ResolveProviderCopyAs(p, L"/", L"dir", L"dir2");
    CHECK(r.problem == mc::CopyAsProblem::None && r.targetDir == L"/" && r.newName == L"dir2");
}

// --- attributes (CapAttributes) ---------------------------------------------------

TEST_CASE(ProviderToggleAttributes_FlipsOneBitOnTheItemsOnly_EXT002_CON005) {
    MemoryProvider p(L"mem://", mc::CapRead | mc::CapWrite | mc::CapMkdir | mc::CapDelete | mc::CapRename | mc::CapAttributes);
    p.AddFile(L"/a.txt", "a");
    p.AddFile(L"/d/inner.txt", "i");
    std::wstring err;
    p.SetAttributes(L"/a.txt", FILE_ATTRIBUTE_ARCHIVE, err);
    OperationOutcome o = mc::ProviderToggleAttributes(p, L"/", {L"a.txt", L"d"}, FILE_ATTRIBUTE_READONLY, {});
    CHECK(o.succeeded == 2);
    CHECK(p.Attributes(L"/a.txt") == (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_READONLY));
    CHECK(p.Attributes(L"/d") == FILE_ATTRIBUTE_READONLY && p.Attributes(L"/d/inner.txt") == 0);
    o = mc::ProviderToggleAttributes(p, L"/", {L"a.txt"}, FILE_ATTRIBUTE_READONLY, {});
    CHECK(p.Attributes(L"/a.txt") == FILE_ATTRIBUTE_ARCHIVE);
}

TEST_CASE(ProviderCopy_AttributesTravelToALocalCopy_EXT002_ZIP003) {
    test::TempDir dir;
    MemoryProvider zip(L"zip://x", mc::CapRead | mc::CapWrite | mc::CapAttributes);
    zip.AddFile(L"/ro.txt", "r");
    std::wstring err;
    zip.SetAttributes(L"/ro.txt", FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN, err);
    mc::LocalProvider local;
    OperationOutcome o = mc::ProviderCopyItems(zip, L"/", {L"ro.txt"}, local, dir.Path().wstring(), {});
    CHECK(o.succeeded == 1);
    std::filesystem::path copied = dir.Path() / L"ro.txt";
    DWORD attrs = GetFileAttributesW(copied.c_str());
    CHECK(attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) && (attrs & FILE_ATTRIBUTE_HIDDEN));
    SetFileAttributesW(copied.c_str(), FILE_ATTRIBUTE_NORMAL);  // let TempDir clean up
    test::WriteFileContent(dir.Path() / L"back.txt", "b");
    SetFileAttributesW((dir.Path() / L"back.txt").c_str(), FILE_ATTRIBUTE_SYSTEM);
    o = mc::ProviderCopyItems(local, dir.Path().wstring(), {L"back.txt"}, zip, L"/", {});
    CHECK(o.succeeded == 1 && (zip.Attributes(L"/back.txt") & FILE_ATTRIBUTE_SYSTEM));
    SetFileAttributesW((dir.Path() / L"back.txt").c_str(), FILE_ATTRIBUTE_NORMAL);
}

