#pragma once

// EXT-002 (D3): a plugin's McFsProvider table (API 1.12) as the host's
// IFileProvider. Every call into the plugin is SEH-wrapped (PLUG-009): after
// a structured exception the adapter logs once, marks itself crashed and
// answers every later call with Failed -- the panel shows the error instead
// of the host going down. `scope(true/false)` brackets each call so the
// plugin's own host callbacks (PollCancel, Log, dialogs) are attributed to
// it. Paths are joined and split here from the table's rootPath/separator.
// No PluginManager/Console dependency, so it is unit-tested with a fake table.

#include "../MyCommanderPluginSdk/McFileSystem.h"
#include "FileProvider.h"

#include <functional>
#include <string>

namespace mc {

class PluginProviderAdapter final : public IFileProvider {
public:
    using Scope = std::function<void(bool entering)>;
    using CrashReport = std::function<void(const std::wstring& what)>;

    // False (and nothing taken over) when `table` lacks a required entry
    // point for the capabilities it declares; `problem` says which.
    static bool Validate(const McFsProvider& table, std::wstring& problem);

    PluginProviderAdapter(const McFsProvider& table, std::wstring ownerId, Scope scope = {}, CrashReport onCrash = {});
    ~PluginProviderAdapter() override;  // calls the table's Close exactly once
    PluginProviderAdapter(const PluginProviderAdapter&) = delete;
    PluginProviderAdapter& operator=(const PluginProviderAdapter&) = delete;

    const std::wstring& OwnerId() const { return ownerId_; }
    void* Context() const { return table_.context; }
    bool Crashed() const { return crashed_; }

    uint32_t Capabilities() const override;
    std::wstring DisplayRoot() const override { return displayRoot_; }
    std::wstring Kind() const override { return kind_; }
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
    ProviderStatus Commit(const ProviderCancelPoll& cancel, std::wstring& error) override;
    bool HasPendingChanges() const override;
    ProviderStatus Reconnect(const ProviderCancelPoll& cancel, std::wstring& error) override;
    bool AsksBeforeLeaving() const override { return confirmOnLeave_; }
    ProviderStatus SetAttributes(const std::wstring& path, uint32_t attributes, std::wstring& error) override;
    ProviderStatus Reload(std::wstring& error) override;
    ProviderStatus DownloadToFile(const std::wstring& path, const std::wstring& localPath, uint64_t sizeHint,
                                  const ProviderByteProgress& progress, std::wstring& error) override;
    ProviderStatus UploadFromFile(const std::wstring& localPath, const std::wstring& path, uint64_t modifiedUtc,
                                  const ProviderByteProgress& progress, std::wstring& error) override;

    // Internal (stream classes): one guarded call; false = the plugin crashed.
    bool Guard(const wchar_t* what, const std::function<bool()>& sehCall) const;
    const McFsProvider& Table() const { return table_; }

private:
    McFsProvider table_{};
    std::wstring ownerId_;
    std::wstring displayRoot_;
    std::wstring kind_;
    std::wstring root_;
    wchar_t separator_ = L'/';
    bool caseInsensitive_ = false;
    bool confirmOnLeave_ = false;
    Scope scope_;
    CrashReport onCrash_;
    mutable bool crashed_ = false;

    std::wstring Normalize(const std::wstring& path) const;
    static ProviderStatus CrashedStatus(std::wstring& error);
};

}  // namespace mc
