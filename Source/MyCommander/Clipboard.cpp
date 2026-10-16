#include "Clipboard.h"
#include "ClipboardPaste.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

namespace mc {

std::wstring FormatNamesForClipboard(const std::vector<ClipboardFileDetail>& items, ClipboardNameFormat format) {
    std::wstring out;
    for (const auto& item : items) {
        switch (format) {
            case ClipboardNameFormat::Names:
                out += item.name;
                break;
            case ClipboardNameFormat::FullPaths:
                out += item.fullPath.wstring();
                break;
            case ClipboardNameFormat::FullPathsWithDetails:
                out += item.fullPath.wstring() + L"\t" +
                       (item.isDirectory ? L"<DIR>" : std::to_wstring(item.size)) + L"\t" + item.modifiedText;
                break;
        }
        out += L"\r\n";
    }
    return out;
}

bool CopyTextToClipboard(const std::wstring& text) {
    if (!OpenClipboard(nullptr)) return false;
    bool ok = false;
    if (EmptyClipboard()) {
        const size_t bytes = (text.size() + 1) * sizeof(wchar_t);
        HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, bytes);
        if (mem != nullptr) {
            void* dst = GlobalLock(mem);
            if (dst != nullptr) {
                memcpy(dst, text.c_str(), bytes);
                GlobalUnlock(mem);
                ok = SetClipboardData(CF_UNICODETEXT, mem) != nullptr;
            }
            if (!ok) GlobalFree(mem);  // SetClipboardData failed: ownership never transferred
        }
    }
    CloseClipboard();
    return ok;
}

bool ReadClipboardFileList(std::vector<std::filesystem::path>& out, bool& isCut) {
    out.clear();
    isCut = false;
    if (!OpenClipboard(nullptr)) return false;
    struct Closer {
        ~Closer() { CloseClipboard(); }
    } closer;
    HANDLE drop = GetClipboardData(CF_HDROP);
    if (drop == nullptr) return false;
    const void* data = GlobalLock(drop);
    if (data == nullptr) return false;
    out = ParseDropFiles(data, GlobalSize(drop));
    GlobalUnlock(drop);
    if (out.empty()) return false;

    static const UINT effectFormat = RegisterClipboardFormatW(L"Preferred DropEffect");
    HANDLE effect = GetClipboardData(effectFormat);
    if (effect != nullptr && GlobalSize(effect) >= sizeof(DWORD)) {
        const void* e = GlobalLock(effect);
        if (e != nullptr) {
            DWORD value = 0;
            memcpy(&value, e, sizeof(value));
            GlobalUnlock(effect);
            isCut = (value & 2) != 0 && (value & 1) == 0;  // DROPEFFECT_MOVE without COPY
        }
    }
    return true;
}

} // namespace mc
