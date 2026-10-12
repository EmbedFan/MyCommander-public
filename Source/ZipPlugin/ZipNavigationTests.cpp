// Standalone regression harness: exercises the production archive callbacks
// with a host that captures the drawn rows, without an interactive console.
// Build with MSVC from a Developer Command Prompt:
// cl /std:c++20 /EHsc /MTd /FoBuild\ /FeBuild\ZipNavigationTests.exe
//    Source\ZipPlugin\ZipNavigationTests.cpp Source\ZipPlugin\third_party\miniz\miniz.c
// Run: Build\ZipNavigationTests.exe Build\zip-navigation-test.zip
#include "ZipPlugin.cpp"

namespace {
std::vector<std::wstring> drawn;
std::vector<McPanelColor> colors;
void __stdcall Capture(McPanelSide, int16_t, int16_t y, const wchar_t* text, McPanelColor color) {
    if (y >= (int)drawn.size()) drawn.resize(y + 1);
    if (y >= (int)colors.size()) colors.resize(y + 1);
    drawn[y] = text;
    colors[y] = color;
}
bool __stdcall Claim(McPanelSide, const wchar_t*) { return true; }
std::wstring destination, viewedPath;
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
void __stdcall GetPath(McPanelSide, wchar_t* out, int size) { wcsncpy_s(out, size, destination.c_str(), _TRUNCATE); }
void __stdcall Refresh(McPanelSide) {}
void __stdcall Log(McLogLevel, const wchar_t*) {}
int __stdcall Choose(const wchar_t*, const wchar_t* const*, int, const McDialogOption*, int) { return choiceResult; }
void __stdcall Message(const wchar_t*, const wchar_t* const*, int) { ++choiceResult; }
void __stdcall ViewFile(const wchar_t* path) { viewedPath = path; }
// FEA-0008: scripted answer for the host's text prompt (nullptr = Esc).
const wchar_t* promptAnswer = nullptr;
std::wstring lastPromptInitial;
int messages = 0;
bool __stdcall Prompt(const wchar_t*, const wchar_t* initial, wchar_t* out, int size) {
    lastPromptInitial = initial ? initial : L"";
    if (!promptAnswer) return false;
    wcsncpy_s(out, size, promptAnswer, _TRUNCATE);
    return true;
}
void __stdcall CountMessage(const wchar_t*, const wchar_t* const*, int) { ++messages; }
const ArchiveEntry* FindEntry(const ArchiveView& v, const std::wstring& name) {
    auto it = std::find_if(v.entries.begin(), v.entries.end(), [&](const ArchiveEntry& e) { return e.name == name; });
    return it == v.entries.end() ? nullptr : &*it;
}
int failures = 0;
void Check(bool condition, const char* label) {
    if (!condition) { printf("FAIL: %s\n", label); ++failures; }
}
void Render(McPanelSide side, int height = 5) {
    drawn.clear();
    McPlugin_OnPanelDraw(side, 80, (uint16_t)height, true);
}
}

int wmain(int argc, wchar_t** argv) {
    if (argc != 2) return 2;
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
    if (GetFileAttributesW(argv[1]) != INVALID_FILE_ATTRIBUTES) {
        printf("Test ZIP path must not already exist.\n");
        return 2;
    }
    McPluginHost host{};
    host.PanelDrawText = Capture;
    host.RequestPanelContent = Claim;
    host.GetPanelPath = GetPath;
    host.RefreshPanel = Refresh;
    host.ShowChoice = Choose;
    host.ShowMessage = Message;
    host.ShowFileViewer = ViewFile;
    host.Log = Log;
    host.BeginProgress = BeginProgress;
    host.UpdateProgress = UpdateProgress;
    host.EndProgress = EndProgress;
    host.ShowTextPrompt = Prompt;
    // g_host is only used while this harness runs; host outlives every use.
    // cppcheck-suppress danglingLifetime
    g_host = &host;
    for (size_t width : {size_t(0), size_t(2), size_t(20), size_t(40), size_t(60), size_t(80)}) {
        auto columns = FitArchiveColumns(width);
        auto header = FormatArchiveLine(columns, L"", L"Name", L"Size", L"Packed", L"Attr");
        auto row = FormatArchiveLine(columns, L">* ", L"very-long-\x4E2D\x6587-filename-that-must-fit.txt",
                                     L"123", L"45", L"00000020");
        Check(mc::StringDisplayWidth(header) == (int)width, "header fits pane width");
        Check(mc::StringDisplayWidth(row) == (int)width, "Unicode row fits pane width");
        size_t start = columns.markerWidth + columns.nameWidth;
        for (int i = 0; i < columns.detailCount; ++i) {
            ++start;
            const wchar_t* titles[] = {L"Size", L"Packed", L"Attr"};
            const wchar_t* values[] = {L"123", L"45", L"00000020"};
            size_t headerEnd = mc::StringDisplayWidth(header.substr(0, header.find(titles[i]))) + wcslen(titles[i]);
            size_t rowEnd = mc::StringDisplayWidth(row.substr(0, row.find(values[i]))) + wcslen(values[i]);
            Check(headerEnd == start + kArchiveDetailWidths[i], "header detail column boundary");
            Check(rowEnd == headerEnd, "row detail aligns with header");
            start += kArchiveDetailWidths[i];
        }
    }
    std::vector<ArchiveEntry> entries;
    for (int i = 0; i < 20; ++i) {
        wchar_t name[32]; swprintf_s(name, L"file%02d.txt", i);
        entries.push_back({name, {42}, 0, 0});
    }
    entries.push_back({L"folder/child.txt", {43}, 0, 0});
    Check(WriteArchiveEntries(argv[1], entries), "create test ZIP");
    for (McPanelSide side : {kMcPanelLeft, kMcPanelRight}) {
        Check(McPlugin_EnterArchive(side, argv[1]), "enter real ZIP");
        Render(side);
        Check(drawn[1].find(L">  folder/") == 0, "initial visible cursor");
        McPlugin_OnPanelKey(side, VK_DOWN, ENHANCED_KEY);
        Render(side);
        Check(drawn[2].find(L">  file00.txt") == 0, "Down moves drawn cursor");
        McPlugin_OnPanelKey(side, VK_UP, ENHANCED_KEY);
        Render(side);
        Check(drawn[1].front() == L'>', "Up moves drawn cursor");
        McPlugin_OnPanelKey(side, VK_UP, 0);
        Check(ViewFor(side).cursor == 0, "Up clamps at first entry");
        McPlugin_OnPanelKey(side, VK_END, 0);
        Render(side);
        Check(drawn[4].find(L">  file19.txt") == 0, "End scrolls to last entry");
        McPlugin_OnPanelKey(side, VK_UP, 0);
        Render(side);
        Check(drawn[3].find(L">  file18.txt") == 0, "Up moves within scrolled viewport");
        McPlugin_OnPanelClick(side, 0, 1, false);
        Check(ViewFor(side).cursor == ViewFor(side).topIndex, "click uses viewport origin");
        McPlugin_OnPanelKey(side, VK_HOME, 0);
        McPlugin_OnPanelKey(side, VK_NEXT, 0);
        Check(ViewFor(side).cursor == 4, "PageDown uses visible row count");
        Render(side, 3);
        Check(drawn[2].front() == L'>', "resize keeps cursor visible");
        McPlugin_OnPanelKey(side, VK_HOME, 0);
        McPlugin_OnPanelKey(side, VK_RETURN, 0);
        Render(side);
        Check(drawn[1].find(L">  ../") == 0, "enter directory resets cursor");
        McPlugin_OnPanelKey(side, VK_DOWN, 0);
        Render(side);
        Check(drawn[2].find(L">  child.txt") == 0, "Down inside directory");
        McPlugin_OnPanelKey(side, VK_BACK, 0);
        Check(ViewFor(side).folder.empty(), "Back returns to archive root");
    }
    auto& view = ViewFor(kMcPanelLeft);
    view.cursor = 1; view.topIndex = 0; view.visibleRows = 10;
    McPlugin_OnPanelMouse(kMcPanelLeft, 0, 2, RIGHTMOST_BUTTON_PRESSED, 0, 0);
    McPlugin_OnPanelMouse(kMcPanelLeft, 0, 2, 0, 0, 0);
    McPlugin_OnPanelMouse(kMcPanelLeft, 0, 4, RIGHTMOST_BUTTON_PRESSED, 0, LEFT_CTRL_PRESSED);
    Check(view.selected.size() == 3, "mouse range selection");
    McPlugin_OnPanelMouse(kMcPanelLeft, 0, 4, 0, 0, 0);
    Render(kMcPanelLeft, 10);
    Check(colors[2] == kMcPanelColorSelected, "selected non-cursor row coloring");
    view.cursor = 0;
    auto prefixes = SelectedPrefixes(view, CurrentItems(view));
    Check(prefixes.size() == 3 && prefixes[0] == L"file00.txt", "selection excludes unrelated cursor");
    Check(!MatchesAny(L"file00.txt.backup", {L"file00.txt"}), "file selection matches exact filename");
    destination = std::wstring(argv[1]) + L".extract";
    Check(CreateDirectoryW(destination.c_str(), nullptr) != 0, "create extraction fixture");
    McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, 0);
    for (int i = 0; i < 3; ++i) {
        wchar_t name[32]; swprintf_s(name, L"file%02d.txt", i);
        auto out = JoinPath(destination, name);
        Check(ReadFileBytes(out) == std::vector<uint8_t>{42}, "F5 copies selected contents");
        DeleteFileW(out.c_str());
    }
    RemoveDirectoryW(destination.c_str());
    view.selected.clear(); view.cursor = 1;
    McPlugin_OnPanelKey(kMcPanelLeft, VK_F3, 0);
    Check(!viewedPath.empty(), "F3 invokes host viewer");
    Check(GetFileAttributesW(viewedPath.c_str()) == INVALID_FILE_ATTRIBUTES, "F3 removes temporary file");
    choiceResult = 0;
    McPlugin_OnPanelKey(kMcPanelLeft, 'A', LEFT_CTRL_PRESSED);
    // IS-0014 (ZIP-010): edits are deferred until the archive is left; write
    // them out explicitly before each on-disk check below.
    Check(SaveArchiveIfDirty(view), "save deferred attribute change");
    ArchiveView reloaded;
    Check(LoadArchive(argv[1], reloaded), "reload archive after attributes change");
    Check(reloaded.entries[0].attributes & FILE_ATTRIBUTE_READONLY, "attributes persist in ZIP");
    choiceResult = -1;
    McPlugin_OnPanelKey(kMcPanelLeft, VK_F8, 0);
    Check(view.entries.size() == 21, "cancel deletion preserves entries");
    choiceResult = 0;
    McPlugin_OnPanelKey(kMcPanelLeft, VK_F8, 0);
    Check(view.entries.size() == 20, "confirm deletion removes only cursor entry");
    Check(SaveArchiveIfDirty(view), "save deferred deletion");
    Check(LoadArchive(argv[1], reloaded) && reloaded.entries.size() == 20, "deletion persists in ZIP");
    Check(CreateDirectoryW(destination.c_str(), nullptr) != 0, "create move fixture");
    view.cursor = 1;
    McPlugin_OnPanelKey(kMcPanelLeft, VK_F6, 0);
    auto movedPath = JoinPath(destination, L"file01.txt");
    Check(ReadFileBytes(movedPath) == std::vector<uint8_t>{42}, "F6 writes destination contents");
    Check(SaveArchiveIfDirty(view), "save deferred move-out");
    Check(LoadArchive(argv[1], reloaded) && reloaded.entries.size() == 19, "F6 removes moved entry from archive");
    DeleteFileW(movedPath.c_str()); RemoveDirectoryW(destination.c_str());

    // --- FEA-0008 (ZIP-011): Shift+F5 copy within the archive ---------------
    {
        std::vector<ArchiveEntry> es = {
            {L"a.txt", {1, 2, 3}, FILE_ATTRIBUTE_READONLY, 0},
            {L"dir/", {}, FILE_ATTRIBUTE_DIRECTORY, 0},
            {L"dir/x.txt", {7}, 0, 0},
            {L"dir/sub/y.txt", {8}, 0, 0},
        };
        auto resolve = [&](const std::wstring& folder, const std::wstring& key, const std::wstring& typed) {
            return ResolveArchiveCopyTarget(es, folder, key, typed);
        };
        Check(resolve(L"", L"a.txt", L"b.txt").targetKey == L"b.txt", "copy-as: bare name is a sibling");
        Check(resolve(L"dir/", L"dir/x.txt", L"z.txt").targetKey == L"dir/z.txt", "copy-as: relative to current folder");
        Check(resolve(L"", L"a.txt", L"new\\deep\\c.txt").targetKey == L"new/deep/c.txt", "copy-as: backslashes normalized");
        Check(resolve(L"", L"a.txt", L"dir").targetKey == L"dir/a.txt", "copy-as: existing folder receives the copy");
        Check(resolve(L"", L"a.txt", L"newdir/").targetKey == L"newdir/a.txt", "copy-as: trailing slash means into");
        Check(resolve(L"", L"a.txt", L"  \"q.txt\" ").targetKey == L"q.txt", "copy-as: trims spaces and quotes");
        auto empty = resolve(L"", L"a.txt", L"   ");
        Check(empty.targetKey.empty() && empty.error.empty(), "copy-as: empty input is a silent no-op");
        for (const wchar_t* bad : {L"../out.txt", L"/abs.txt", L"C:\\x.txt", L"a/./b.txt", L"x//b.txt"})
            Check(!resolve(L"", L"a.txt", bad).error.empty(), "copy-as: path leaving the archive refused");
        Check(!resolve(L"", L"a.txt", L"A.TXT").error.empty(), "copy-as: unchanged name (any case) refused");
        Check(!resolve(L"", L"dir/", L"dir").error.empty(), "copy-as: unchanged folder name refused");
        Check(!resolve(L"", L"dir/", L"dir/sub/dir2").error.empty(), "copy-as: folder into itself refused");
        Check(resolve(L"", L"dir/", L"dir2").targetKey == L"dir2/", "copy-as: folder target is a folder key");
        Check(!resolve(L"", L"dir/", L"a.txt").error.empty(), "copy-as: file in the way of a folder copy refused");
        Check(!resolve(L"", L"dir/x.txt", L"a.txt/n.txt").error.empty(), "copy-as: file in the way of a parent folder refused");

        auto plan = PlanArchiveCopy(es, L"dir/", L"dir2/");
        Check(plan.size() == 3, "copy-as: folder plan covers marker and nested files");
        bool nested = false;
        for (const auto& step : plan) nested |= step.first == L"dir/sub/y.txt" && step.second == L"dir2/sub/y.txt";
        Check(nested, "copy-as: nested path preserved under the new folder");

        // End to end through the real key handler.
        view.entries = es; view.folder.clear(); view.selected.clear(); view.dirty = false;
        host.ShowMessage = CountMessage;
        auto cursorTo = [&](const std::wstring& key) {
            auto items = CurrentItems(view);
            for (size_t i = 0; i < items.size(); ++i) if (items[i].key == key) view.cursor = (int)i;
        };
        destination = std::wstring(argv[1]) + L".shiftf5";
        Check(CreateDirectoryW(destination.c_str(), nullptr) != 0, "create Shift+F5 fixture");
        cursorTo(L"a.txt");
        promptAnswer = L"b.txt";
        McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, SHIFT_PRESSED);
        Check(lastPromptInitial == L"a.txt", "Shift+F5 prompt pre-filled with the item name");
        const ArchiveEntry* b = FindEntry(view, L"b.txt");
        Check(b && b->bytes == std::vector<uint8_t>{1, 2, 3} && b->attributes == FILE_ATTRIBUTE_READONLY,
              "Shift+F5 copies bytes and attributes");
        Check(FindEntry(view, L"a.txt") && FindEntry(view, L"a.txt")->bytes == std::vector<uint8_t>{1, 2, 3},
              "Shift+F5 leaves the original untouched");
        Check(view.dirty, "Shift+F5 copy is deferred (archive marked dirty)");
        Check(CurrentItems(view)[view.cursor].key == L"b.txt", "cursor lands on the new copy");
        Check(GetFileAttributesW(JoinPath(destination, L"a.txt").c_str()) == INVALID_FILE_ATTRIBUTES,
              "Shift+F5 no longer extracts to the other panel");
        Check(RemoveDirectoryW(destination.c_str()) != 0, "other panel's directory left empty");

        for (auto& e : view.entries) if (e.name == L"b.txt") e.bytes = {9};
        size_t before = view.entries.size();
        cursorTo(L"a.txt");
        choiceResult = 2;  // Cancel
        McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, SHIFT_PRESSED);
        Check(view.entries.size() == before && FindEntry(view, L"b.txt")->bytes == std::vector<uint8_t>{9},
              "conflict Cancel changes nothing");
        cursorTo(L"a.txt");
        choiceResult = 1;  // Skip existing
        McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, SHIFT_PRESSED);
        Check(FindEntry(view, L"b.txt")->bytes == std::vector<uint8_t>{9}, "conflict Skip keeps the existing entry");
        cursorTo(L"a.txt");
        choiceResult = 0;  // Overwrite
        McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, SHIFT_PRESSED);
        Check(view.entries.size() == before && FindEntry(view, L"b.txt")->bytes == std::vector<uint8_t>{1, 2, 3},
              "conflict Overwrite replaces in place");

        cursorTo(L"dir/");
        promptAnswer = L"dir2";
        McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, SHIFT_PRESSED);
        Check(FindEntry(view, L"dir2/") && FindEntry(view, L"dir2/x.txt") && FindEntry(view, L"dir2/sub/y.txt"),
              "Shift+F5 copies a whole folder");

        before = view.entries.size();
        messages = 0;
        cursorTo(L"a.txt");
        promptAnswer = L"..\\evil.txt";
        McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, SHIFT_PRESSED);
        Check(messages == 1 && view.entries.size() == before, "unsafe name shows a message and changes nothing");
        promptAnswer = nullptr;
        McPlugin_OnPanelKey(kMcPanelLeft, VK_F5, SHIFT_PRESSED);
        Check(view.entries.size() == before, "Esc in the prompt changes nothing");

        Check(SaveArchiveIfDirty(view), "save Shift+F5 copies");
        Check(LoadArchive(argv[1], reloaded) && FindEntry(reloaded, L"b.txt") && FindEntry(reloaded, L"dir2/sub/y.txt"),
              "Shift+F5 copies persist after save");
        host.ShowMessage = Message;
        choiceResult = 0;
    }

    Check(!LoadArchive(std::wstring(argv[1]) + L".missing", reloaded), "missing archive reports failure");
    Check(!WriteArchiveEntries(JoinPath(std::wstring(argv[1]) + L".missing-folder", L"out.zip"), entries), "write failure reports failure");
    Check(progressBegins >= 10 && progressUpdates > progressBegins, "archive operations publish progress");
    Check(progressBegins == progressEnds && progressDepth == 0 && progressValid, "progress closes on success and failure without nesting");
    DeleteFileW(argv[1]);
    printf("ZIP navigation regression: %d failures\n", failures);
    return failures ? 1 : 0;
}
