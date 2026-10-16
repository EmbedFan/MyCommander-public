// Standalone regression harness for the ZIP plugin (ZipPlugin.cpp, included
// directly). Since EXT-002 an open archive is a file-system provider: the fake
// host keeps the McFsProvider the plugin opens and drives it through the
// HOST's adapter and copy engine (PluginProviderAdapter / ProviderCopy /
// LocalProvider) -- the code the app runs for F3/F5/F6/F7/F8/F2/Shift+F5/
// Ctrl+A on an archive panel -- without an interactive console.
// Build with MSVC from a Developer Command Prompt (repository root):
// cl /utf-8 /std:c++20 /EHsc /MTd /FoBuild\ /FeBuild\ZipNavigationTests.exe
//    Source\ZipPlugin\ZipNavigationTests.cpp Source\ZipPlugin\ZipCrypto.cpp
//    Source\ZipPlugin\third_party\miniz\miniz.c Source\MyCommander\PluginProviderAdapter.cpp
//    Source\MyCommander\ProviderCopy.cpp Source\MyCommander\LocalProvider.cpp Source\MyCommander\FileProvider.cpp
//    bcrypt.lib
// Run: Build\ZipNavigationTests.exe Build\zip-navigation-test.zip [Source\ZipPlugin\testdata]
// (the optional second argument enables the ARC-004 encrypted-archive checks)
#include "ZipPlugin.cpp"

#include "../MyCommander/LocalProvider.h"
#include "../MyCommander/PluginProviderAdapter.h"
#include "../MyCommander/ProviderCopy.h"

#include <memory>

namespace {
int progressBegins = 0, progressEnds = 0, progressUpdates = 0, progressDepth = 0;
bool progressValid = true;
bool __stdcall BeginProgress(const wchar_t*, uint64_t) {
    ++progressBegins; if (++progressDepth != 1) progressValid = false; return true;
}
void __stdcall UpdateProgress(uint64_t done, uint64_t total, const wchar_t*) {
    ++progressUpdates; if (progressDepth != 1 || (total && done > total)) progressValid = false;
}
void __stdcall EndProgress() {
    ++progressEnds; if (--progressDepth != 0) progressValid = false;
}
int choiceResult = 0;
int choices = 0;
void __stdcall Log(McLogLevel, const wchar_t*) {}
int __stdcall Choose(const wchar_t*, const wchar_t* const*, int, const McDialogOption*, int) { ++choices; return choiceResult; }
int messages = 0;
void __stdcall Message(const wchar_t*, const wchar_t* const*, int) { ++messages; }
McPanelSide __stdcall ActiveSide() { return kMcPanelLeft; }
// The fake host's two panels: the provider each one shows (if any).
std::shared_ptr<mc::PluginProviderAdapter> panels[2];
std::wstring returnName;
bool __stdcall OpenProvider(McPanelSide side, const McFsProvider* table, const wchar_t*, const wchar_t* selectOnLeave) {
    std::wstring problem;
    if (!mc::PluginProviderAdapter::Validate(*table, problem)) return false;
    panels[side] = std::make_shared<mc::PluginProviderAdapter>(*table, L"com.mycommander.zipplugin");
    returnName = selectOnLeave ? selectOnLeave : L"";
    return true;
}
bool __stdcall CloseProvider(McPanelSide side) {
    panels[side].reset();
    return true;
}
void* __stdcall GetPanelProvider(McPanelSide side) { return panels[side] ? panels[side]->Context() : nullptr; }
// ARC-004: scripted answers for the host's masked password form (an exhausted
// script is Esc). Records how often it was asked and the last label shown.
std::vector<std::wstring> formAnswers;
size_t formCalls = 0;
std::wstring lastFormLabel;
bool __stdcall PasswordForm(const wchar_t*, McFormField* fields, int count) {
    ++formCalls;
    if (count < 1 || fields[0].kind != kMcFormFieldPassword) return false;
    lastFormLabel = fields[0].label ? fields[0].label : L"";
    if (formCalls > formAnswers.size()) return false;
    wcsncpy_s(fields[0].textBuffer, fields[0].textBufferSize, formAnswers[formCalls - 1].c_str(), _TRUNCATE);
    return true;
}
void ScriptPasswords(std::vector<std::wstring> answers) {
    formAnswers = std::move(answers);
    formCalls = 0;
    lastFormLabel.clear();
}
std::vector<uint8_t> HelloBytes() {
    std::string text;
    for (int i = 0; i < 50; ++i) text += "ARC-004 secret line\n";
    return std::vector<uint8_t>(text.begin(), text.end());
}
const ArchiveEntry* FindEntry(const ArchiveView& v, const std::wstring& name) {
    auto it = std::find_if(v.entries.begin(), v.entries.end(), [&](const ArchiveEntry& e) { return e.name == name; });
    return it == v.entries.end() ? nullptr : &*it;
}
int failures = 0;
void Check(bool condition, const char* label) {
    if (!condition) { printf("FAIL: %s\n", label); ++failures; }
}
std::vector<mc::ProviderEntry> List(mc::IFileProvider& p, const std::wstring& dir) {
    std::vector<mc::ProviderEntry> entries;
    std::wstring err;
    p.List(dir, entries, {}, err);
    return entries;
}
const mc::ProviderEntry* Named(const std::vector<mc::ProviderEntry>& entries, const std::wstring& name) {
    for (const auto& e : entries) if (e.name == name) return &e;
    return nullptr;
}
std::vector<uint8_t> ReadThrough(mc::IFileProvider& p, const std::wstring& path) {
    std::unique_ptr<mc::IReadStream> in;
    std::wstring err;
    std::vector<uint8_t> out;
    if (p.OpenRead(path, in, {}, err) != mc::ProviderStatus::Ok) return out;
    uint8_t buffer[7];
    size_t got = 0;
    while (in->Read(buffer, sizeof(buffer), got, err) == mc::ProviderStatus::Ok && got > 0) out.insert(out.end(), buffer, buffer + got);
    return out;
}
void CloseAll() {
    panels[0].reset();
    panels[1].reset();
}
ArchiveView* Left() { return ArchiveOnSide(kMcPanelLeft); }
mc::ProviderOpHandlers Answer(mc::ConflictChoice choice) {
    mc::ProviderOpHandlers h;
    h.onConflict = [choice](const std::filesystem::path&) {
        mc::ConflictAskResult r;
        r.choice = choice;
        return r;
    };
    return h;
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 2) return 2;
    // ARC-001: which file names the plugin claims as browsable archives.
    Check(McPlugin_CanHandleFile(L"a.zip") && McPlugin_CanHandleFile(L"B.ZIP"), "claims .zip, any case");
    Check(McPlugin_CanHandleFile(L"lib.jar") && McPlugin_CanHandleFile(L"app.apk") &&
              McPlugin_CanHandleFile(L"x.WAR") && McPlugin_CanHandleFile(L"e.ear") &&
              McPlugin_CanHandleFile(L"ext.xpi") && McPlugin_CanHandleFile(L"ext.vsix") &&
              McPlugin_CanHandleFile(L"p.1.0.nupkg") && McPlugin_CanHandleFile(L"p-1.0-py3-none-any.whl"),
          "claims ZIP-based package extensions");
    Check(!McPlugin_CanHandleFile(L"report.docx") && !McPlugin_CanHandleFile(L"sheet.xlsx") &&
              !McPlugin_CanHandleFile(L"book.epub") && !McPlugin_CanHandleFile(L"doc.odt"),
          "leaves Office/ODF/EPUB documents alone");
    Check(!McPlugin_CanHandleFile(L"notes.txt") && !McPlugin_CanHandleFile(L"zip") &&
              !McPlugin_CanHandleFile(L"archive.zip.txt") && !McPlugin_CanHandleFile(nullptr),
          "rejects non-archives");
    const McPluginMetadata* meta = McPlugin_GetInfo();
    Check(meta->apiVersionMinor >= 12 && (meta->capabilities & kMcPluginCapFileSystem) &&
              !(meta->capabilities & kMcPluginCapPanel),
          "declares API 1.12, a file-system provider (no panel content any more)");
    if (GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES) {
        printf("Test ZIP path must not already exist.\n");
        return 2;
    }
    McPluginHost host{};
    host.structSize = sizeof(host);
    host.ShowChoice = Choose;
    host.ShowMessage = Message;
    host.Log = Log;
    host.BeginProgress = BeginProgress;
    host.UpdateProgress = UpdateProgress;
    host.EndProgress = EndProgress;
    host.GetActiveSide = ActiveSide;
    host.OpenProvider = OpenProvider;
    host.CloseProvider = CloseProvider;
    host.GetPanelProvider = GetPanelProvider;
    // g_host is only used while this harness runs; host outlives every use.
    // cppcheck-suppress danglingLifetime
    g_host = &host;
    mc::LocalProvider disk;

    const uint64_t stamp = 133000000000000000ull;  // 2022-06-14
    std::vector<ArchiveEntry> entries;
    for (int i = 0; i < 20; ++i) {
        wchar_t name[32]; swprintf_s(name, L"file%02d.txt", i);
        entries.push_back({name, {42}, 0, 0, stamp});
    }
    entries.push_back({L"folder/child.txt", {43}, 0, 0, stamp});
    Check(WriteArchiveEntries(argv[1], entries), "create test ZIP");

    // --- ZIP-005: Enter opens the archive as a provider -----------------------------
    for (McPanelSide side : {kMcPanelLeft, kMcPanelRight}) {
        Check(McPlugin_EnterArchive(side, argv[1]) && panels[side], "enter real ZIP");
        mc::IFileProvider& zip = *panels[side];
        Check(zip.DisplayRoot() == argv[1] && zip.Kind() == L"zip" && returnName == FileNameOf(argv[1]),
              "the header shows the archive; leaving selects it again");
        Check(zip.AsksBeforeLeaving(), "leaving asks first (ZIP-007)");
        auto root = List(zip, L"/");
        Check(root.size() == 21 && Named(root, L"folder") && Named(root, L"folder")->isDirectory, "root lists files and the folder");
        Check(Named(root, L"file07.txt") && Named(root, L"file07.txt")->size == 1, "file sizes");
        uint64_t got = Named(root, L"file07.txt") ? Named(root, L"file07.txt")->modifiedUtc : 0;
        Check(got >= stamp - 20000000ull && got <= stamp + 20000000ull, "entry times survive a save (DOS 2 s precision)");
        auto sub = List(zip, L"/folder");
        Check(sub.size() == 1 && sub[0].name == L"child.txt", "a folder lists its own entries");
        mc::ProviderEntry e;
        std::wstring err;
        Check(zip.Stat(L"/FILE00.TXT", e, {}, err) == mc::ProviderStatus::Ok && !e.isDirectory, "names match in any case");
        Check(zip.Stat(L"/folder", e, {}, err) == mc::ProviderStatus::Ok && e.isDirectory, "an implied folder exists");
        Check(zip.Stat(L"/nope.txt", e, {}, err) == mc::ProviderStatus::NotFound, "a missing name is NotFound");
        Check(ReadThrough(zip, L"/folder/child.txt") == std::vector<uint8_t>{43}, "F3: reading an entry");
    }
    CloseAll();
    Check(g_archives.empty(), "the host's Close frees the archive");

    Check(McPlugin_EnterArchive(kMcPanelLeft, argv[1]), "re-enter");
    mc::IFileProvider& zip = *panels[0];
    ArchiveView& view = *Left();
    ArchiveView reloaded;

    // --- F5: extract to a folder; attributes travel along (ZIP-003) ------------------
    std::wstring destination = std::wstring(argv[1]) + L".extract";
    Check(CreateDirectoryW(destination.c_str(), nullptr) != 0, "create extraction fixture");
    std::wstring err;
    Check(zip.SetAttributes(L"/file00.txt", FILE_ATTRIBUTE_HIDDEN, err) == mc::ProviderStatus::Ok, "set an attribute");
    mc::OperationOutcome o = mc::ProviderCopyItems(zip, L"/", {L"file00.txt", L"file01.txt", L"file02.txt"}, disk, destination, {});
    Check(o.succeeded == 3, "F5 extracts the selection");
    for (int i = 0; i < 3; ++i) {
        wchar_t name[32]; swprintf_s(name, L"file%02d.txt", i);
        auto out = JoinPath(destination, name);
        Check(ReadFileBytes(out) == std::vector<uint8_t>{42}, "F5 writes the entry's bytes");
        if (i == 0) Check((GetFileAttributesW(out.c_str()) & FILE_ATTRIBUTE_HIDDEN) != 0, "the entry's attributes come along");
        SetFileAttributesW(out.c_str(), FILE_ATTRIBUTE_NORMAL);
        DeleteFileW(out.c_str());
    }
    Check(view.dirty, "an attribute change is pending (ZIP-010)");

    // --- Ctrl+A, F8, F6, F7, F2 -- all deferred until Commit ----------------------------
    o = mc::ProviderToggleAttributes(zip, L"/", {L"file00.txt"}, FILE_ATTRIBUTE_READONLY, {});
    Check(o.succeeded == 1, "Ctrl+A toggles an attribute");
    Check(zip.Commit({}, err) == mc::ProviderStatus::Ok && !view.dirty, "Commit saves (the host's Save on leaving)");
    Check(LoadArchive(argv[1], reloaded) && FindEntry(reloaded, L"file00.txt") &&
              (FindEntry(reloaded, L"file00.txt")->attributes & FILE_ATTRIBUTE_READONLY),
          "attributes persist in the ZIP");
    o = mc::ProviderDeleteItems(zip, L"/", {L"file03.txt"}, {});
    Check(o.succeeded == 1 && view.entries.size() == 20 && view.dirty, "F8 removes the entry (pending)");
    Check(zip.Commit({}, err) == mc::ProviderStatus::Ok && LoadArchive(argv[1], reloaded) && reloaded.entries.size() == 20,
          "deletion persists in the ZIP");
    o = mc::ProviderMoveItems(zip, L"/", {L"file04.txt"}, disk, destination, {});
    auto movedPath = JoinPath(destination, L"file04.txt");
    Check(o.succeeded == 1 && ReadFileBytes(movedPath) == std::vector<uint8_t>{42}, "F6 writes the destination");
    Check(zip.Commit({}, err) == mc::ProviderStatus::Ok && LoadArchive(argv[1], reloaded) && reloaded.entries.size() == 19,
          "F6 removes the moved entry from the archive");
    DeleteFileW(movedPath.c_str());
    Check(zip.MakeDirectory(L"/newdir", {}, err) == mc::ProviderStatus::Ok && FindEntry(view, L"newdir/"),
          "F7 adds a persisted folder marker (IS-0012)");
    Check(zip.MakeDirectory(L"/newdir", {}, err) == mc::ProviderStatus::Failed, "F7 refuses an existing folder");
    Check(zip.MakeDirectory(L"/file05.txt", {}, err) == mc::ProviderStatus::Failed, "F7 refuses a file's name");
    Check(zip.Rename(L"/file05.txt", L"/renamed.txt", {}, err) == mc::ProviderStatus::Ok && FindEntry(view, L"renamed.txt"),
          "F2 renames a file");
    Check(zip.Rename(L"/renamed.txt", L"/file06.txt", {}, err) == mc::ProviderStatus::Failed, "a rename never overwrites");
    Check(zip.Rename(L"/folder", L"/moved", {}, err) == mc::ProviderStatus::Ok && FindEntry(view, L"moved/child.txt"),
          "F2 renames a folder with its contents");

    // --- F5 from a folder into the archive (ZIP-006) -------------------------------------
    std::wstring addDir = JoinPath(destination, L"tree");
    CreateDirectoryW(addDir.c_str(), nullptr);
    WriteFileBytes(JoinPath(addDir, L"inner.txt"), {5, 6});
    WriteFileBytes(JoinPath(destination, L"top.txt"), {7});
    o = mc::ProviderCopyItems(disk, destination, {L"tree", L"top.txt"}, zip, L"/moved", {});
    Check(o.succeeded == 2 && FindEntry(view, L"moved/tree/inner.txt") && FindEntry(view, L"moved/top.txt"),
          "F5 into the archive adds files and folders");
    o = mc::ProviderCopyItems(disk, destination, {L"top.txt"}, zip, L"/moved", Answer(mc::ConflictChoice::Skip));
    Check(o.skipped == 1, "adding an existing name asks (Skip keeps it)");
    DeleteFileW(JoinPath(addDir, L"inner.txt").c_str());
    RemoveDirectoryW(addDir.c_str());
    DeleteFileW(JoinPath(destination, L"top.txt").c_str());
    RemoveDirectoryW(destination.c_str());

    // --- Shift+F5 inside the archive (ZIP-011) -------------------------------------------
    {
        view.entries = {
            {L"a.txt", {1, 2, 3}, FILE_ATTRIBUTE_READONLY, 0, stamp},
            {L"dir/", {}, FILE_ATTRIBUTE_DIRECTORY, 0, 0},
            {L"dir/x.txt", {7}, 0, 0, 0},
            {L"dir/sub/y.txt", {8}, 0, 0, 0},
        };
        view.dirty = false;
        auto target = mc::ResolveProviderCopyAs(zip, L"/", L"a.txt", L"b.txt");
        o = mc::ProviderCopyItemAs(zip, L"/", L"a.txt", zip, target.targetDir, target.newName, {});
        const ArchiveEntry* b = FindEntry(view, L"b.txt");
        Check(o.succeeded == 1 && b && b->bytes == std::vector<uint8_t>{1, 2, 3} && (b->attributes & FILE_ATTRIBUTE_READONLY),
              "Shift+F5 copies bytes and attributes");
        Check(FindEntry(view, L"a.txt")->bytes == std::vector<uint8_t>{1, 2, 3} && view.dirty,
              "the original is untouched; the copy is pending");
        target = mc::ResolveProviderCopyAs(zip, L"/", L"dir", L"dir2");
        o = mc::ProviderCopyItemAs(zip, L"/", L"dir", zip, target.targetDir, target.newName, {});
        Check(FindEntry(view, L"dir2/") && FindEntry(view, L"dir2/x.txt") && FindEntry(view, L"dir2/sub/y.txt"),
              "Shift+F5 copies a whole folder");
        target = mc::ResolveProviderCopyAs(zip, L"/", L"a.txt", L"dir");
        Check(target.targetDir == L"/dir" && target.newName == L"a.txt", "an existing folder receives the copy");
        Check(mc::ResolveProviderCopyAs(zip, L"/", L"a.txt", L"A.TXT").problem == mc::CopyAsProblem::SameItem,
              "the unchanged name (any case) is refused");
        Check(mc::ResolveProviderCopyAs(zip, L"/", L"a.txt", L"..\\evil.txt").problem == mc::CopyAsProblem::Unsafe,
              "a path leaving the archive is refused");
        Check(zip.Commit({}, err) == mc::ProviderStatus::Ok && LoadArchive(argv[1], reloaded) && FindEntry(reloaded, L"b.txt") &&
                  FindEntry(reloaded, L"dir2/sub/y.txt"),
              "Shift+F5 copies persist after save");
    }

    // --- Ctrl+R: reload from disk (ZIP-009) -------------------------------------------
    Check(zip.MakeDirectory(L"/temp", {}, err) == mc::ProviderStatus::Ok && view.dirty, "an unsaved change");
    choiceResult = 1;  // Cancel
    Check(zip.Reload(err) == mc::ProviderStatus::Cancelled && FindEntry(view, L"temp/"), "Reload asks first; Cancel keeps it");
    choiceResult = 0;  // Reload anyway
    Check(zip.Reload(err) == mc::ProviderStatus::Ok && !FindEntry(view, L"temp/") && !view.dirty,
          "Reload re-reads the file and drops the unsaved change");
    CloseAll();

    // ARC-004: encrypted archives through the real plugin entry points.
    if (argc >= 3) {
        const std::wstring data = argv[2];
        host.ShowForm = PasswordForm;
        auto fixture = [&](const wchar_t* name) { return data + L"\\" + name; };

        ScriptPasswords({L"S3cretPw!"});
        Check(McPlugin_EnterArchive(kMcPanelLeft, fixture(L"aes256.zip").c_str()), "ARC-004: AES-256 archive opens with its password");
        if (ArchiveView* v = Left()) {
            const ArchiveEntry* hello = FindEntry(*v, L"hello.txt");
            Check(hello && hello->bytes == HelloBytes(), "ARC-004: entry decrypted into the browser");
            Check(FindEntry(*v, L"sub/data.bin") && FindEntry(*v, L"sub/data.bin")->bytes.size() == 3000, "ARC-004: nested entry decrypted");
            Check(v->encrypted && !v->password.Empty() && formCalls == 1, "ARC-004: asked exactly once, password cached for the archive");
        }
        Check(lastFormLabel.find(L"aes256.zip") != std::wstring::npos, "ARC-004: the prompt names the archive");
        CloseAll();

        ScriptPasswords({L"wrong", L"S3cretPw!"});
        Check(McPlugin_EnterArchive(kMcPanelLeft, fixture(L"aes128.zip").c_str()) && formCalls == 2,
              "ARC-004: a wrong password is asked again");
        Check(lastFormLabel.find(L"Wrong password") != std::wstring::npos, "ARC-004: the retry prompt says the password was wrong");
        CloseAll();

        ScriptPasswords({});
        Check(!McPlugin_EnterArchive(kMcPanelLeft, fixture(L"aes256.zip").c_str()) && !panels[0] && g_archives.empty(),
              "ARC-004: Esc at the prompt leaves the archive closed");
        ScriptPasswords({L"a", L"b", L"c"});
        Check(!McPlugin_EnterArchive(kMcPanelLeft, fixture(L"aes256.zip").c_str()) && formCalls == 3,
              "ARC-004: three wrong passwords give up");
        CloseAll();

        ScriptPasswords({L"S3cretPw!"});
        Check(McPlugin_EnterArchive(kMcPanelLeft, fixture(L"zipcrypt.zip").c_str()) && Left() &&
                  FindEntry(*Left(), L"hello.txt") && FindEntry(*Left(), L"hello.txt")->bytes == HelloBytes(),
              "ARC-004: legacy ZipCrypto archive opens");
        CloseAll();

        // Saving an edited encrypted archive keeps it encrypted (never a silent downgrade).
        std::wstring editable = std::wstring(argv[1]) + L".enc.zip";
        std::vector<uint8_t> original = ReadFileBytes(fixture(L"aes256.zip"));
        Check(WriteFileBytes(editable, original), "ARC-004: copy fixture for the edit test");
        ScriptPasswords({L"S3cretPw!"});
        Check(McPlugin_EnterArchive(kMcPanelLeft, editable.c_str()), "ARC-004: editable copy opens");
        if (panels[0]) {
            std::unique_ptr<mc::IWriteStream> out;
            Check(panels[0]->OpenWrite(L"/added.txt", 8, 0, out, {}, err) == mc::ProviderStatus::Ok &&
                      out->Write("new file", 8, err) == mc::ProviderStatus::Ok && out->Finish(err) == mc::ProviderStatus::Ok,
                  "ARC-004: a file written into the encrypted archive");
            Check(panels[0]->Commit({}, err) == mc::ProviderStatus::Ok, "ARC-004: edited encrypted archive saves");
        }
        {
            std::vector<uint8_t> saved = ReadFileBytes(editable);
            std::string image(saved.begin(), saved.end());
            Check(image.find("ARC-004 secret line") == std::string::npos && image.find("new file") == std::string::npos,
                  "ARC-004: saved archive holds no plaintext");
            Check(image.find("S3cretPw!") == std::string::npos, "ARC-004: saved archive holds no password");
        }
        CloseAll();
        ScriptPasswords({L"S3cretPw!"});
        Check(McPlugin_EnterArchive(kMcPanelLeft, editable.c_str()) && formCalls == 1 && Left() &&
                  FindEntry(*Left(), L"added.txt") && FindEntry(*Left(), L"hello.txt") &&
                  FindEntry(*Left(), L"hello.txt")->bytes == HelloBytes(),
              "ARC-004: the saved archive still needs the password and round-trips");
        CloseAll();
        DeleteFileW(editable.c_str());

        // Unpack: a password is asked once and decrypts every entry; Esc stops the unpack.
        std::wstring unpackDir = std::wstring(argv[1]) + L".unpack";
        CreateDirectoryW(unpackDir.c_str(), nullptr);
        {
            ArchiveProgress quiet(L"test", 0, false);
            uint64_t processed = 0;
            bool cancelled = false;
            zipcrypto::Secret batch;
            ScriptPasswords({L"S3cretPw!"});
            int extracted = ExtractZipArchive(fixture(L"aes256.zip"), unpackDir, quiet, processed, cancelled, batch);
            Check(extracted == 3 && !cancelled && formCalls == 1, "ARC-004: Unpack decrypts every entry with one prompt");
            Check(ReadFileBytes(JoinPath(unpackDir, L"hello.txt")) == HelloBytes(), "ARC-004: Unpack wrote the decrypted file");
            Check(ReadFileBytes(JoinPath(unpackDir, L"sub\\data.bin")).size() == 3000, "ARC-004: Unpack wrote the nested file");

            // The batch password is reused for the next archive without asking.
            std::wstring unpackDir2 = unpackDir + L"2";
            CreateDirectoryW(unpackDir2.c_str(), nullptr);
            ScriptPasswords({});
            extracted = ExtractZipArchive(fixture(L"aes256-store.zip"), unpackDir2, quiet, processed, cancelled, batch);
            Check(extracted == 3 && formCalls == 0, "ARC-004: the batch password is tried silently on the next archive");

            std::wstring unpackDir3 = unpackDir + L"3";
            CreateDirectoryW(unpackDir3.c_str(), nullptr);
            zipcrypto::Secret none;
            ScriptPasswords({});
            cancelled = false;
            extracted = ExtractZipArchive(fixture(L"aes256.zip"), unpackDir3, quiet, processed, cancelled, none);
            Check(cancelled && extracted == 0, "ARC-004: Esc at the prompt cancels the unpack");
            for (const std::wstring& d : {unpackDir, unpackDir2, unpackDir3}) {
                DeleteFileW(JoinPath(d, L"hello.txt").c_str());
                DeleteFileW(JoinPath(d, L"sub\\data.bin").c_str());
                DeleteFileW(JoinPath(d, L"empty.txt").c_str());
                RemoveDirectoryW(JoinPath(d, L"sub").c_str());
                RemoveDirectoryW(d.c_str());
            }
        }

        // Pack: an encrypted archive from files on disk reads back with the password only.
        {
            std::wstring src = std::wstring(argv[1]) + L".src.txt", out = std::wstring(argv[1]) + L".packed.zip";
            Check(WriteFileBytes(src, HelloBytes()), "ARC-004: pack source written");
            PackEntry entry;
            entry.sourcePath = src;
            entry.archiveName = L"packed/hello.txt";
            entry.attributes = FILE_ATTRIBUTE_ARCHIVE;
            ArchiveProgress quiet(L"test", 0, false);
            uint64_t processed = 0;
            bool cancelled = false;
            zipcrypto::Secret pw(L"pässwörd-日本");
            Check(WriteZipArchive({entry}, out, 6, quiet, processed, 1, cancelled, &pw), "ARC-004: encrypted Pack writes the archive");
            ScriptPasswords({L"pässwörd-日本"});
            Check(McPlugin_EnterArchive(kMcPanelLeft, out.c_str()) && Left() && FindEntry(*Left(), L"packed/hello.txt") &&
                      FindEntry(*Left(), L"packed/hello.txt")->bytes == HelloBytes(),
                  "ARC-004: packed archive opens with the Unicode password");
            CloseAll();
            DeleteFileW(src.c_str());
            DeleteFileW(out.c_str());
        }
    } else {
        printf("note: no testdata directory argument -- ARC-004 encrypted-archive checks skipped\n");
    }

    Check(!LoadArchive(std::wstring(argv[1]) + L".missing", reloaded), "missing archive reports failure");
    Check(!McPlugin_EnterArchive(kMcPanelLeft, (std::wstring(argv[1]) + L".missing").c_str()) && !panels[0] && g_archives.empty(),
          "a missing archive opens nothing");
    Check(!WriteArchiveEntries(JoinPath(std::wstring(argv[1]) + L".missing-folder", L"out.zip"), entries), "write failure reports failure");
    Check(progressBegins >= 5 && progressUpdates > progressBegins, "archive operations publish progress");
    Check(progressBegins == progressEnds && progressDepth == 0 && progressValid, "progress closes on success and failure without nesting");
    McPlugin_Shutdown();
    DeleteFileW(argv[1]);
    printf("ZIP navigation regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
