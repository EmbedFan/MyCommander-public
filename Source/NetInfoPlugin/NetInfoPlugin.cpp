// FEA-0010 (NET-001...NET-010): the Network Info MC Plugin. Shows this computer's shares, the Wi-Fi networks in
// range (the connected one emitted) and the machines found on the network, in the panel that is not active.
// Esc (or the menu item / Alt+F11 again) closes it and the file panel underneath comes back as it was.
//
// This file is only the adapter: McPluginHost <-> netinfo::IPanelHost, and the plugin exports. The behaviour is
// in NetInfoController.cpp, the data and drawing rules in NetInfoModel.cpp, the Windows calls in
// NetInfoSources.cpp. Read-only everywhere: nothing connects, disconnects, reads a saved key or stores a credential.

#include "../MyCommanderPluginSdk/McPluginAbi.h"
#include "../MyCommanderPluginSdk/McPluginHost.h"
#include "../MyCommanderPluginSdk/McPluginTypes.h"

#include "NetInfoController.h"
#include "NetInfoSources.h"
#include "Version.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace {

const McPluginHost* g_host = nullptr;

constexpr uint32_t kToggleCommandId = 1;

McPanelColor ToHostColor(netinfo::DrawColor c) {
    switch (c) {
        case netinfo::DrawColor::Highlight: return kMcPanelColorHighlight;
        case netinfo::DrawColor::Section: return kMcPanelColorSection;
        case netinfo::DrawColor::Warning: return kMcPanelColorWarning;
        case netinfo::DrawColor::Selected: return kMcPanelColorSelected;
        default: return kMcPanelColorNormal;
    }
}

McPanelSide ToSide(int side) { return side == 0 ? kMcPanelLeft : kMcPanelRight; }

class HostAdapter : public netinfo::IPanelHost {
public:
    int ActiveSide() override { return g_host && g_host->GetActiveSide && g_host->GetActiveSide() == kMcPanelRight ? 1 : 0; }
    bool IsInProvider(int side) override {
        return g_host && g_host->IsPanelInProvider && g_host->IsPanelInProvider(ToSide(side));
    }
    bool RequestContent(int side, const std::wstring& title) override {
        return g_host && g_host->RequestPanelContent(ToSide(side), title.c_str());
    }
    void ReleaseContent(int side) override {
        if (g_host) g_host->ReleasePanelContent(ToSide(side));
    }
    std::wstring PanelPath(int side) override {
        wchar_t buffer[2048] = {};
        if (g_host && g_host->GetPanelPath) g_host->GetPanelPath(ToSide(side), buffer, 2048);
        return buffer;
    }
    void RefreshPanel(int side) override {
        if (g_host && g_host->RefreshPanel) g_host->RefreshPanel(ToSide(side));
    }
    bool CanNavigate() override {
        return g_host && g_host->structSize > offsetof(McPluginHost, NavigatePanel) && g_host->NavigatePanel != nullptr;
    }
    bool NavigatePanel(int side, const std::wstring& path) override {
        return CanNavigate() && g_host->NavigatePanel(ToSide(side), path.c_str());
    }
    void ShowMessage(const std::wstring& title, const std::vector<std::wstring>& lines) override {
        if (!g_host) return;
        std::vector<const wchar_t*> raw;
        for (const auto& l : lines) raw.push_back(l.c_str());
        g_host->ShowMessage(title.c_str(), raw.data(), static_cast<int>(raw.size()));
    }
    int ShowChoice(const std::wstring& title, const std::vector<std::wstring>& lines,
                   const std::vector<std::pair<wchar_t, std::wstring>>& options) override {
        if (!g_host) return -1;
        std::vector<const wchar_t*> raw;
        for (const auto& l : lines) raw.push_back(l.c_str());
        std::vector<McDialogOption> opts;
        for (const auto& o : options) opts.push_back({o.first, o.second.c_str()});
        return g_host->ShowChoice(title.c_str(), raw.data(), static_cast<int>(raw.size()), opts.data(),
                                  static_cast<int>(opts.size()));
    }
    void PutText(int side, int x, int y, const std::wstring& text, netinfo::DrawColor color) override {
        if (g_host) g_host->PanelDrawText(ToSide(side), static_cast<int16_t>(x), static_cast<int16_t>(y), text.c_str(), ToHostColor(color));
    }
    void Log(int level, const std::wstring& message) override {
        if (!g_host) return;
        const McLogLevel l = level <= 0 ? kMcLogError : level == 1 ? kMcLogWarning : level == 2 ? kMcLogInfo : kMcLogDebug;
        g_host->Log(l, message.c_str());
    }
};

HostAdapter g_adapter;
std::unique_ptr<netinfo::Controller> g_controller;

const wchar_t* const kHelpMarkdown =
    L"Shows the network area in the panel that is not active: this computer's shared folders and mapped drives, the\n"
    L"Wi-Fi networks in range (the one this computer is connected to is marked with `*`), and the machines found on\n"
    L"the network.\n"
    L"\n"
    L"# Commands\n"
    L"\n"
    L"- Network menu: `Network info` (or `Alt+F11`) opens it, and closes it again\n"
    L"- `Esc` in the network panel closes it; the file panel comes back as it was\n"
    L"- `Up` `Down` `PgUp` `PgDn` `Home` `End` move; `Enter` on a machine shows its shares\n"
    L"- `Enter` on a folder share opens it in the other panel\n"
    L"- `Space` or `F3` shows the details of a row; `F5` or `Ctrl+R` scans again\n"
    L"\n"
    L"# Limits\n"
    L"\n"
    L"- Windows no longer browses the network by itself: machines appear once this computer has contacted them\n"
    L"- Windows 11 hides Wi-Fi names unless Location access for desktop apps is on\n"
    L"- Read-only: it never connects to a network or reads a saved password\n";

}  // namespace

extern "C" __declspec(dllexport) const McPluginMetadata* __stdcall McPlugin_GetInfo(void) {
    static const McPluginMetadata info = {
        sizeof(McPluginMetadata),
        MC_PLUGIN_API_VERSION_MAJOR,
        MC_PLUGIN_API_VERSION_MINOR,
        MC_PLUGIN_MAGIC,
        L"com.mycommander.netinfo",
        L"Network Info",
        L"Shows shares, Wi-Fi networks and machines of the network in a panel.",
        MCPLUGIN_VERSION_WSTR,
        L"MyCommander project",
        kMcPluginCapMenu | kMcPluginCapShortcuts | kMcPluginCapHelp | kMcPluginCapPanel,
    };
    return &info;
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_Init(const McPluginHost* host) {
    g_host = host;
    // A scan thread may still be inside a Windows call when the panel closes (it works on private state and can
    // no longer change anything); pinning the DLL means such a thread can never run unloaded code.
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_PIN | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       reinterpret_cast<LPCWSTR>(&McPlugin_GetInfo), &self);

    bool menuOk = host->AddMenuItem(L'N', L"Network info", L'I', kToggleCommandId, kMcMenuPlacementAppend, 0);
    McShortcutSpec shortcut{};
    // Alt+F11: Windows Terminal keeps Ctrl+Shift+N (new window) and never lets the application see it.
    shortcut.virtualKeyCode = VK_F11;
    shortcut.ctrl = false;
    shortcut.shift = false;
    shortcut.alt = true;
    shortcut.commandId = kToggleCommandId;
    bool shortcutOk = host->RegisterShortcut(&shortcut);
    host->Log(menuOk ? kMcLogInfo : kMcLogWarning, menuOk ? L"Menu item registered." : L"Menu item registration failed.");
    host->Log(shortcutOk ? kMcLogInfo : kMcLogWarning,
              shortcutOk ? L"Shortcut Alt+F11 registered." : L"Shortcut Alt+F11 was already taken.");
    try {
        g_controller = std::make_unique<netinfo::Controller>(g_adapter, netinfo::MakeWindowsSource());
    } catch (...) {
        host->Log(kMcLogError, L"Network info could not start.");
        return false;
    }
    return true;
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_Shutdown(void) {
    if (g_controller) {
        try {
            g_controller->Shutdown();
        } catch (...) {
        }
        g_controller.reset();
    }
    g_host = nullptr;
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnMenuAction(uint32_t commandId) {
    if (commandId != kToggleCommandId || !g_controller) return;
    try {
        g_controller->Toggle();
    } catch (...) {
        if (g_host) g_host->Log(kMcLogError, L"Network info failed.");
    }
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnShortcut(uint32_t commandId) { McPlugin_OnMenuAction(commandId); }

extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetHelpMarkdown(void) { return kHelpMarkdown; }

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelDraw(McPanelSide side, uint16_t width, uint16_t height,
                                                                     bool active) {
    if (!g_controller) return;
    try {
        g_controller->OnDraw(side == kMcPanelLeft ? 0 : 1, width, height, active, GetTickCount64());
    } catch (...) {
    }
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelKey(McPanelSide side, uint16_t virtualKeyCode,
                                                                    uint32_t controlKeyState) {
    if (!g_controller) return;
    try {
        g_controller->OnKey(side == kMcPanelLeft ? 0 : 1, virtualKeyCode, controlKeyState);
    } catch (...) {
    }
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelClick(McPanelSide side, int16_t relativeX, int16_t relativeY,
                                                                      bool doubleClick) {
    if (!g_controller) return;
    try {
        g_controller->OnClick(side == kMcPanelLeft ? 0 : 1, relativeX, relativeY, doubleClick);
    } catch (...) {
    }
}
