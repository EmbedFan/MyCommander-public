#include "ExplorerReveal.h"

#include "PathUtil.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shellapi.h>
#include <shlobj.h>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")

namespace mc {

RevealTarget RevealTargetFor(const std::filesystem::path& folder, const std::vector<std::wstring>& names) {
    RevealTarget t;
    t.folder = folder;
    size_t count = 0;
    for (const std::wstring& n : names) {
        if (n.empty() || n == L"..") continue;
        ++count;
        if (!t.item) t.item = n;
    }
    t.moreThanOne = count > 1;
    return t;
}

std::wstring BuildExplorerSelectArguments(const std::filesystem::path& itemPath) {
    return L"/select,\"" + itemPath.wstring() + L"\"";
}

std::wstring RevealInExplorer(const RevealTarget& target) {
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(init);
    std::wstring error;

    if (!target.item) {
        const INT_PTR r = reinterpret_cast<INT_PTR>(
            ShellExecuteW(nullptr, L"open", target.folder.c_str(), nullptr, nullptr, SW_SHOWNORMAL));
        if (r <= 32) error = L"Explorer could not open the folder.";
    } else {
        const std::filesystem::path itemPath = target.folder / *target.item;
        bool done = false;
        PIDLIST_ABSOLUTE pidl = nullptr;
        if (SUCCEEDED(SHParseDisplayName(itemPath.c_str(), nullptr, &pidl, 0, nullptr)) && pidl != nullptr) {
            done = SUCCEEDED(SHOpenFolderAndSelectItems(pidl, 0, nullptr, 0));
            CoTaskMemFree(pidl);
        }
        if (!done) {  // fallback: explorer.exe /select,"<path>"
            const std::wstring args = BuildExplorerSelectArguments(itemPath);
            const INT_PTR r = reinterpret_cast<INT_PTR>(
                ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL));
            if (r <= 32) error = L"Explorer could not show the item.";
        }
    }
    if (uninit) CoUninitialize();
    return error;
}

}  // namespace mc
