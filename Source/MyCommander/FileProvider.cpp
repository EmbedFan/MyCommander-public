#include "FileProvider.h"

namespace mc {

const wchar_t* ProviderStatusText(ProviderStatus status) {
    switch (status) {
    case ProviderStatus::Ok: return L"OK";
    case ProviderStatus::Failed: return L"failed";
    case ProviderStatus::Cancelled: return L"cancelled";
    case ProviderStatus::ConnectionLost: return L"connection lost";
    case ProviderStatus::NotSupported: return L"not supported here";
    case ProviderStatus::NotFound: return L"not found";
    }
    return L"failed";
}

bool ProviderPathIsSameOrWithin(const IFileProvider& provider, const std::wstring& ancestor, const std::wstring& path) {
    std::wstring current = path;
    for (int guard = 0; guard < 4096; ++guard) {
        if (provider.SamePath(current, ancestor)) return true;
        if (provider.IsRoot(current)) return false;
        std::wstring parent = provider.Parent(current);
        if (parent == current) return false;
        current = parent;
    }
    return false;
}

std::wstring ProviderLeafName(const IFileProvider& provider, const std::wstring& path) {
    if (provider.IsRoot(path)) return path;
    std::wstring parent = provider.Parent(path);
    if (parent == path || parent.size() >= path.size()) return path;
    std::wstring leaf = path.substr(parent.size());
    size_t start = leaf.find_first_not_of(L"\\/");
    if (start == std::wstring::npos) return L"";
    size_t end = leaf.find_last_not_of(L"\\/");
    return leaf.substr(start, end - start + 1);
}

}  // namespace mc
