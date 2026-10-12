// FEA-0002 (MC Plugin), Phase 1 & 2: the smallest complete example plugin --
// walked through chapter-by-chapter in
// Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md's own "Worked example"
// section. Registers one File-menu item ("Example Plugin Say Hello") that
// shows a message dialog, one keyboard shortcut (Ctrl+Shift+Y) that does
// the same, and a few lines of Markdown help content that appear under a
// "Plugins" section on the host's own F1 shortcut-reference screen --
// deliberately minimal: enough to demonstrate identification, the Init/
// Shutdown lifecycle, menu integration, a keyboard shortcut, a host dialog
// call, host logging, and Help content, without any feature-specific logic
// distracting from the plugin-authoring mechanics themselves.
//
// Build this as its own DLL (ExamplePlugin.vcxproj, not part of
// MyCommander.sln), then copy the resulting ExamplePlugin.dll into
// MyCommander.exe's own Plugins\ subfolder to try it.

#include "../MyCommanderPluginSdk/McPluginAbi.h"
#include "../MyCommanderPluginSdk/McPluginHost.h"
#include "../MyCommanderPluginSdk/McPluginTypes.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cwchar>
#include <string>
#include <vector>

namespace {

// The host hands us this pointer once, at Init, and it stays valid for our
// whole lifetime -- see McPluginHost.h's own comment on why it's safe to
// just keep it in a plain global rather than re-fetching it.
const McPluginHost* g_host = nullptr;

// Arbitrary, chosen by this plugin -- meaningful only to us, handed back
// unchanged by the host whenever this command fires (menu click or
// shortcut press).
constexpr uint32_t kSayHelloCommandId = 1;

void SayHello() {
    if (!g_host) return;
    const wchar_t* lines[] = {L"Hello from ExamplePlugin!"};
    g_host->ShowMessage(L"Example Plugin", lines, 1);
    g_host->Log(kMcLogInfo, L"Said hello.");
}

// #7 (Phase 2): rendered by the host's own deliberately minimal Markdown
// renderer (headings, bullet items, plain paragraphs, inline code spans
// stripped rather than colored) as a "Example Plugin" sub-heading inside
// F1's own "Plugins" section -- static storage, so the returned pointer
// stays valid for the whole time this DLL is loaded, matching
// McPlugin_GetHelpMarkdownFn's own lifetime requirement.
const wchar_t* const kHelpMarkdown =
    L"The smallest complete MC Plugin -- one menu item, one shortcut.\n"
    L"\n"
    L"# Commands\n"
    L"\n"
    L"- File menu: `Example Plugin Say Hello`\n"
    L"- Shortcut: `Ctrl+Shift+Y`\n"
    L"\n"
    L"# Viewer\n"
    L"\n"
    L"- Shift+F3 on any file: `Hex dump (plugin)` shows its bytes in hex and ASCII\n";

// API 1.10 (EXT-003): one "00000010  4D 5A 90 00 ... |MZ..|" line per 16 bytes.
std::wstring HexLine(unsigned long long offset, const unsigned char* bytes, size_t count) {
    static const wchar_t kHex[] = L"0123456789ABCDEF";
    wchar_t head[24];
    swprintf_s(head, L"%08llX  ", offset);
    std::wstring line = head;
    for (size_t i = 0; i < 16; ++i) {
        if (i < count) {
            line += kHex[bytes[i] >> 4];
            line += kHex[bytes[i] & 15];
            line += L' ';
        } else {
            line += L"   ";
        }
        if (i == 7) line += L' ';
    }
    line += L" |";
    for (size_t i = 0; i < count; ++i) line += (bytes[i] >= 0x20 && bytes[i] < 0x7F) ? static_cast<wchar_t>(bytes[i]) : L'.';
    line += L'|';
    return line;
}

}  // namespace

extern "C" __declspec(dllexport) const McPluginMetadata* __stdcall McPlugin_GetInfo(void) {
    static const McPluginMetadata info = {
        sizeof(McPluginMetadata),
        MC_PLUGIN_API_VERSION_MAJOR,
        MC_PLUGIN_API_VERSION_MINOR,
        MC_PLUGIN_MAGIC,
        L"com.mycommander.exampleplugin",
        L"Example Plugin",
        L"The smallest complete MC Plugin -- one menu item, one shortcut.",
        L"1.0.0",
        L"MyCommander project",
        kMcPluginCapMenu | kMcPluginCapShortcuts | kMcPluginCapHelp | kMcPluginCapViewer,
    };
    return &info;
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_Init(const McPluginHost* host) {
    g_host = host;

    // #6: append one item to the built-in File menu ('F' is its mnemonic
    // -- see MenuBar.h's BuildMenus()). 'Y' is this item's own mnemonic
    // (Alt+F opens File, then Y chooses this item) -- unique within File's
    // own dropdown is all that's required (MenuBar.h's mnemonics are
    // scoped per-menu, not global).
    bool menuOk = g_host->AddMenuItem(L'F', L"Example Plugin Say Hello", L'Y', kSayHelloCommandId,
                                      kMcMenuPlacementAppend, 0);

    // #12: Ctrl+Shift+Y -- rejected (registerOk == false) if that
    // combination is already claimed by a built-in shortcut or another
    // plugin; this example just logs either outcome rather than treating
    // a conflict as a fatal Init failure, matching the Developer Guide's
    // own "prefer to degrade gracefully" recommendation.
    McShortcutSpec shortcut{};
    shortcut.virtualKeyCode = 'Y';
    shortcut.ctrl = true;
    shortcut.shift = true;
    shortcut.alt = false;
    shortcut.commandId = kSayHelloCommandId;
    bool shortcutOk = g_host->RegisterShortcut(&shortcut);

    g_host->Log(kMcLogInfo, menuOk ? L"Menu item registered." : L"Menu item registration failed.");
    g_host->Log(shortcutOk ? kMcLogInfo : kMcLogWarning,
               shortcutOk ? L"Shortcut registered." : L"Shortcut Ctrl+Shift+Y was already taken.");
    return true;  // still activate even if the shortcut collided -- the menu item alone is still useful
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_Shutdown(void) {
    // #5: the host has already removed our menu item and shortcut by the
    // time this runs -- nothing left for us to release, since this
    // example holds no other resources (no memory, handles, or threads of
    // its own).
    g_host = nullptr;
}

// #6: called for our own menu item; #12: called for our own shortcut --
// both funnel into the same SayHello() since they're the same logical
// command reached two different ways.
extern "C" __declspec(dllexport) void __stdcall McPlugin_OnMenuAction(uint32_t commandId) {
    if (commandId == kSayHelloCommandId) SayHello();
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnShortcut(uint32_t commandId) {
    if (commandId == kSayHelloCommandId) SayHello();
}

// #7: required by kMcPluginCapHelp -- the host calls this fresh every time
// the user presses F1, never caching the result.
extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetHelpMarkdown(void) {
    return kHelpMarkdown;
}

// --- API 1.10 (EXT-003): a viewer plugin ------------------------------------

// Offered for every file, but only in Shift+F3 -- a hex dump is never the
// obvious viewer for a format, so it never takes over F3 (kMcViewDefault).
// The user can still make it a file type's F3 default from File > Assign
// File to App, or as "plugin:com.mycommander.exampleplugin" in
// file-actions.conf.
extern "C" __declspec(dllexport) McViewClaim __stdcall McPlugin_CanViewFile(const wchar_t*) { return kMcViewOffer; }

extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetViewerName(void) { return L"Hex dump"; }

extern "C" __declspec(dllexport) bool __stdcall McPlugin_RenderView(const wchar_t* filePath, const McViewWriter* writer) {
    if (!filePath || !writer) return false;
    HANDLE file = CreateFileW(filePath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
                              OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    std::wstring title = std::wstring(L"Hex dump: ") + filePath;
    writer->SetTitle(writer->context, title.c_str());

    std::vector<unsigned char> buffer(64 * 1024);
    unsigned long long offset = 0;
    DWORD read = 0;
    while (ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr) && read > 0) {
        for (DWORD i = 0; i < read; i += 16) {
            size_t count = (read - i) < 16 ? (read - i) : 16;
            std::wstring line = HexLine(offset + i, buffer.data() + i, count);
            writer->AppendLine(writer->context, line.c_str());
        }
        offset += read;
        // Esc between 64 KB chunks; the host also caps the output's size.
        if (g_host && g_host->PollCancel && g_host->PollCancel()) {
            writer->AppendLine(writer->context, L"... (stopped)");
            break;
        }
    }
    CloseHandle(file);
    return true;
}
