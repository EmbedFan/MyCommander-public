#pragma once

// EXT-002 (D1): the local disk as an IFileProvider. Paths are ordinary
// Windows paths ("C:\a\b", "\\server\share\x"); every Win32 call goes through
// PathUtil.h's ToLongPath. Local <-> local operations keep using FileOps (its
// CopyFileExW fast path, Recycle Bin, elevation); this provider is the local
// side of a copy to or from any other provider.

#include "FileProvider.h"

namespace mc {

// Suffix of the temporary file a write goes to before Finish renames it
// over the target.
inline constexpr wchar_t kLocalPartSuffix[] = L".mc-part";

// Moves a finished temporary file `part` over `target` (replacing it, also
// when read-only), first stamping `modifiedUtc` (0 = keep). On failure the
// part file is deleted and `target` is untouched.
ProviderStatus CommitLocalPart(const std::wstring& part, const std::wstring& target, uint64_t modifiedUtc,
                               std::wstring& error);
// Deletes a temporary file that won't be committed.
void DiscardLocalPart(const std::wstring& part);

class LocalProvider final : public IFileProvider {
public:
    uint32_t Capabilities() const override;
    std::wstring DisplayRoot() const override { return L""; }
    std::wstring Kind() const override { return L"local"; }
    bool IsLocal() const override { return true; }

    std::wstring Join(const std::wstring& dir, const std::wstring& name) const override;
    std::wstring Parent(const std::wstring& path) const override;
    bool IsRoot(const std::wstring& path) const override;
    bool SamePath(const std::wstring& a, const std::wstring& b) const override;

    ProviderStatus List(const std::wstring& dir, std::vector<ProviderEntry>& entries, const ProviderCancelPoll& cancel,
                        std::wstring& error) override;
    ProviderStatus Stat(const std::wstring& path, ProviderEntry& entry, const ProviderCancelPoll& cancel,
                        std::wstring& error) override;
    ProviderStatus OpenRead(const std::wstring& path, std::unique_ptr<IReadStream>& stream,
                            const ProviderCancelPoll& cancel, std::wstring& error) override;
    ProviderStatus OpenWrite(const std::wstring& path, uint64_t sizeHint, uint64_t modifiedUtc,
                             std::unique_ptr<IWriteStream>& stream, const ProviderCancelPoll& cancel,
                             std::wstring& error) override;
    ProviderStatus MakeDirectory(const std::wstring& path, const ProviderCancelPoll& cancel,
                                 std::wstring& error) override;
    ProviderStatus Delete(const std::wstring& path, bool isDirectory, const ProviderCancelPoll& cancel,
                          std::wstring& error) override;
    ProviderStatus Rename(const std::wstring& from, const std::wstring& to, const ProviderCancelPoll& cancel,
                          std::wstring& error) override;
    ProviderStatus SetAttributes(const std::wstring& path, uint32_t attributes, std::wstring& error) override;
};

}  // namespace mc
