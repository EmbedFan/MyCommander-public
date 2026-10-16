#include "RecycleBin.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <objbase.h>
#include <oleauto.h>
#include <shlobj.h>
#include <shobjidl.h>
#include <shlwapi.h>
#include <wrl/client.h>

#include <algorithm>
#include <cwchar>

#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "oleaut32.lib")
#pragma comment(lib, "shlwapi.lib")

namespace fs = std::filesystem;
using Microsoft::WRL::ComPtr;

namespace mc {

namespace {

bool IEquals(const std::wstring& a, const std::wstring& b) {
    return CompareStringOrdinal(a.c_str(), static_cast<int>(a.size()), b.c_str(), static_cast<int>(b.size()), TRUE) ==
           CSTR_EQUAL;
}

std::wstring FormatBytes(uint64_t bytes) {
    wchar_t buf[48];
    if (bytes < 1024) {
        swprintf_s(buf, L"%llu B", static_cast<unsigned long long>(bytes));
    } else if (bytes < 1024ull * 1024) {
        swprintf_s(buf, L"%.1f KB", static_cast<double>(bytes) / 1024.0);
    } else if (bytes < 1024ull * 1024 * 1024) {
        swprintf_s(buf, L"%.1f MB", static_cast<double>(bytes) / (1024.0 * 1024.0));
    } else {
        swprintf_s(buf, L"%.2f GB", static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0));
    }
    return buf;
}

std::wstring FormatLocalTime(uint64_t ticks) {
    if (ticks == 0) return L"";
    FILETIME utc{static_cast<DWORD>(ticks), static_cast<DWORD>(ticks >> 32)}, local{};
    SYSTEMTIME st{};
    if (!FileTimeToLocalFileTime(&utc, &local) || !FileTimeToSystemTime(&local, &st)) return L"";
    wchar_t buf[32];
    swprintf_s(buf, L"%04d-%02d-%02d %02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute);
    return buf;
}

// --- the real bin ----------------------------------------------------------------------

// The shell's own property keys for a Recycle Bin item (stable since
// Windows Vista, language independent -- unlike the column headers, which are
// localized): {9B174B33-40FF-11D2-A27E-00C04FC30871}, pid 2 = original folder,
// pid 3 = date deleted.
constexpr PROPERTYKEY kDeletedFrom = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 2};
constexpr PROPERTYKEY kDateDeleted = {{0x9B174B33, 0x40FF, 0x11D2, {0xA2, 0x7E, 0x00, 0xC0, 0x4F, 0xC3, 0x08, 0x71}}, 3};
// System.Size: {B725F130-47EF-101A-A5F1-02608C9EEBAC}, pid 12
constexpr PROPERTYKEY kSize = {{0xB725F130, 0x47EF, 0x101A, {0xA5, 0xF1, 0x02, 0x60, 0x8C, 0x9E, 0xEB, 0xAC}}, 12};

class ComScope {
public:
    ComScope() {
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
        initialized_ = SUCCEEDED(hr);  // RPC_E_CHANGED_MODE: already initialized differently, still usable
    }
    ~ComScope() {
        if (initialized_) CoUninitialize();
    }
    ComScope(const ComScope&) = delete;
    ComScope& operator=(const ComScope&) = delete;

private:
    bool initialized_ = false;
};

bool VariantString(const VARIANT& v, std::wstring& out) {
    if ((v.vt == VT_BSTR || v.vt == VT_LPWSTR) && v.bstrVal) {
        out = v.bstrVal;
        return true;
    }
    return false;
}

uint64_t VariantTicks(const VARIANT& v) {
    if (v.vt == VT_DATE) {
        SYSTEMTIME local{}, utc{};
        FILETIME ft{};
        if (VariantTimeToSystemTime(v.date, &local) && TzSpecificLocalTimeToSystemTime(nullptr, &local, &utc) &&
            SystemTimeToFileTime(&utc, &ft))
            return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime;
    }
    return 0;
}

uint64_t VariantSize(const VARIANT& v) {
    switch (v.vt) {
        case VT_UI8: return v.ullVal;
        case VT_I8: return v.llVal < 0 ? 0 : static_cast<uint64_t>(v.llVal);
        case VT_UI4: return v.ulVal;
        case VT_I4: return v.lVal < 0 ? 0 : static_cast<uint64_t>(v.lVal);
        case VT_BSTR: return v.bstrVal ? _wcstoui64(v.bstrVal, nullptr, 10) : 0;
        default: return 0;
    }
}

class ShellRecycleBin final : public IRecycleBin {
public:
    ~ShellRecycleBin() override { FreeChildren(); }

    bool Open(std::wstring& error) {
        PIDLIST_ABSOLUTE pidl = nullptr;
        HRESULT hr = SHGetKnownFolderIDList(FOLDERID_RecycleBinFolder, 0, nullptr, &pidl);
        if (FAILED(hr) || !pidl) {
            error = L"The Recycle Bin is not available.";
            return false;
        }
        hr = SHBindToObject(nullptr, pidl, nullptr, IID_PPV_ARGS(&folder_));
        CoTaskMemFree(pidl);
        if (FAILED(hr) || !folder_) {
            error = L"Cannot open the Recycle Bin.";
            return false;
        }
        return true;
    }

    std::vector<RecycleBinItem> List(std::wstring& error) override {
        FreeChildren();
        std::vector<RecycleBinItem> items;
        ComPtr<IEnumIDList> enumerator;
        HRESULT hr = folder_->EnumObjects(nullptr, SHCONTF_FOLDERS | SHCONTF_NONFOLDERS | SHCONTF_INCLUDEHIDDEN, &enumerator);
        if (FAILED(hr) || !enumerator) {
            error = L"Cannot list the Recycle Bin.";
            return items;
        }
        PITEMID_CHILD child = nullptr;
        ULONG fetched = 0;
        while (enumerator->Next(1, &child, &fetched) == S_OK && fetched == 1) {
            RecycleBinItem item;
            STRRET sr{};
            wchar_t buf[MAX_PATH * 2];
            if (SUCCEEDED(folder_->GetDisplayNameOf(child, SHGDN_NORMAL, &sr)) && SUCCEEDED(StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf))))
                item.displayName = buf;
            std::wstring parsing;
            sr = STRRET{};
            if (SUCCEEDED(folder_->GetDisplayNameOf(child, SHGDN_FORPARSING, &sr)) && SUCCEEDED(StrRetToBufW(&sr, child, buf, ARRAYSIZE(buf))))
                parsing = buf;
            // "Hide extensions" shows the name without it; the stored $R file keeps it.
            std::wstring ext = fs::path(parsing).extension().wstring();
            if (!ext.empty() && (item.displayName.size() < ext.size() ||
                                 !IEquals(item.displayName.substr(item.displayName.size() - ext.size()), ext)))
                item.displayName += ext;

            std::wstring folderText;
            VARIANT v;
            VariantInit(&v);
            if (SUCCEEDED(folder_->GetDetailsEx(child, &kDeletedFrom, &v))) VariantString(v, folderText);
            VariantClear(&v);
            if (SUCCEEDED(folder_->GetDetailsEx(child, &kDateDeleted, &v))) item.deletedUtc = VariantTicks(v);
            VariantClear(&v);
            if (SUCCEEDED(folder_->GetDetailsEx(child, &kSize, &v))) item.sizeBytes = VariantSize(v);
            VariantClear(&v);
            SFGAOF attrs = SFGAO_FOLDER;
            ComPtr<IShellFolder> base;
            if (SUCCEEDED(folder_.As(&base)) && SUCCEEDED(base->GetAttributesOf(1, const_cast<PCUITEMID_CHILD*>(&child), &attrs)))
                item.isDirectory = (attrs & SFGAO_FOLDER) != 0;

            item.originalPath = fs::path(folderText) / item.displayName;
            item.handle = children_.size();
            children_.push_back(child);
            items.push_back(std::move(item));
        }
        SortNewestFirst(items);
        return items;
    }

    bool Restore(const RecycleBinItem& item, std::wstring& error) override {
        if (item.handle >= children_.size()) {
            error = L"The item is no longer in the Recycle Bin list.";
            return false;
        }
        PCUITEMID_CHILD child = children_[item.handle];
        ComPtr<IContextMenu> menu;
        HRESULT hr = folder_->GetUIObjectOf(nullptr, 1, &child, IID_IContextMenu, nullptr, reinterpret_cast<void**>(menu.GetAddressOf()));
        if (FAILED(hr) || !menu) {
            error = L"The Recycle Bin item cannot be restored.";
            return false;
        }
        HMENU popup = CreatePopupMenu();
        if (popup) menu->QueryContextMenu(popup, 0, 1, 0x7FFF, CMF_NORMAL);
        CMINVOKECOMMANDINFO info{};
        info.cbSize = sizeof(info);
        info.fMask = CMIC_MASK_FLAG_NO_UI;
        info.lpVerb = "undelete";
        info.nShow = SW_HIDE;
        hr = menu->InvokeCommand(&info);
        if (popup) DestroyMenu(popup);
        std::error_code ec;
        if (FAILED(hr) || !fs::exists(item.originalPath, ec)) {
            error = L"Windows could not restore \"" + item.displayName + L"\".";
            return false;
        }
        return true;
    }

private:
    void FreeChildren() {
        for (PITEMID_CHILD c : children_) CoTaskMemFree(c);
        children_.clear();
    }

    ComScope com_;  // declared first: the shell objects below are released before CoUninitialize
    ComPtr<IShellFolder2> folder_;
    std::vector<PITEMID_CHILD> children_;
};

}  // namespace

std::unique_ptr<IRecycleBin> OpenShellRecycleBin(std::wstring& error) {
    auto bin = std::make_unique<ShellRecycleBin>();
    if (!bin->Open(error)) return nullptr;
    return bin;
}

RestoreCheck CheckRestore(const RecycleBinItem& item, const std::function<bool(const fs::path&)>& exists) {
    RestoreCheck check;
    if (item.originalPath.empty() || !item.originalPath.has_filename()) {
        check.message = L"The original location of \"" + item.displayName + L"\" is unknown.";
        return check;
    }
    if (exists && exists(item.originalPath)) {
        check.message = L"\"" + item.displayName +
                        L"\": something with this name is already there. Move or rename it first.";
        return check;
    }
    check.ok = true;
    return check;
}

std::wstring FormatRecycleBinRow(const RecycleBinItem& item) {
    std::wstring folder = item.originalPath.parent_path().wstring();
    std::wstring size = item.isDirectory ? std::wstring(L"<DIR>") : FormatBytes(item.sizeBytes);
    return item.displayName + L"  |  " + folder + L"  |  " + FormatLocalTime(item.deletedUtc) + L"  |  " + size;
}

void SortNewestFirst(std::vector<RecycleBinItem>& items) {
    std::stable_sort(items.begin(), items.end(), [](const RecycleBinItem& a, const RecycleBinItem& b) {
        if (a.deletedUtc != b.deletedUtc) return a.deletedUtc > b.deletedUtc;
        return _wcsicmp(a.displayName.c_str(), b.displayName.c_str()) < 0;
    });
}

const RecycleBinItem* FindInBin(const std::vector<RecycleBinItem>& items, const fs::path& originalPath,
                                uint64_t notBeforeUtc, uint64_t sizeBytes, bool matchSize) {
    const RecycleBinItem* best = nullptr;
    for (const RecycleBinItem& item : items) {
        if (!IEquals(item.originalPath.wstring(), originalPath.wstring())) continue;
        if (item.deletedUtc < notBeforeUtc) continue;
        if (matchSize && item.sizeBytes != sizeBytes) continue;
        if (!best || item.deletedUtc > best->deletedUtc) best = &item;
    }
    return best;
}

}  // namespace mc
