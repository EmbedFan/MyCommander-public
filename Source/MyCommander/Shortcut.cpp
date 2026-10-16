#include "Shortcut.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <objbase.h>
#include <shlobj.h>
#include <shobjidl.h>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "uuid.lib")

namespace mc {

namespace {

std::wstring HResultText(const wchar_t* what, HRESULT hr) {
    wchar_t buf[96];
    swprintf_s(buf, L"%ls failed (0x%08X)", what, static_cast<unsigned>(hr));
    return buf;
}

}  // namespace

std::wstring CreateShortcutFile(const std::filesystem::path& linkPath, const std::filesystem::path& targetPath,
                                const std::filesystem::path& workingDir, const std::wstring& description) {
    // COM may already be initialized (the application or the installer did it); a different threading model
    // (RPC_E_CHANGED_MODE) is fine for these in-process interfaces and must not be un-initialized by us.
    const HRESULT init = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    const bool uninit = SUCCEEDED(init);

    std::wstring error;
    IShellLinkW* shellLink = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_IShellLinkW,
                                  reinterpret_cast<void**>(&shellLink));
    if (FAILED(hr)) {
        error = HResultText(L"Creating the shell link", hr);
    } else {
        shellLink->SetPath(targetPath.c_str());
        if (!workingDir.empty()) shellLink->SetWorkingDirectory(workingDir.c_str());
        if (!description.empty()) shellLink->SetDescription(description.c_str());
        IPersistFile* persistFile = nullptr;
        hr = shellLink->QueryInterface(IID_IPersistFile, reinterpret_cast<void**>(&persistFile));
        if (FAILED(hr)) {
            error = HResultText(L"Preparing the shortcut file", hr);
        } else {
            hr = persistFile->Save(linkPath.c_str(), TRUE);
            if (FAILED(hr)) error = HResultText(L"Saving the shortcut", hr);
            persistFile->Release();
        }
        shellLink->Release();
    }
    if (uninit) CoUninitialize();
    return error;
}

std::wstring ShortcutNameFor(const std::wstring& name, const std::function<bool(const std::wstring&)>& exists) {
    if (name.empty()) return L"";
    const std::wstring base = name + L" - Shortcut";
    for (int n = 1; n < 10000; ++n) {
        const std::wstring candidate = base + (n == 1 ? L"" : L" (" + std::to_wstring(n) + L")") + L".lnk";
        if (candidate.size() > 255) return L"";
        if (!exists(candidate)) return candidate;
    }
    return L"";
}

}  // namespace mc
