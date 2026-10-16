#pragma once

// EXT-002: an in-memory IFileProvider ("/a/b" paths, case-sensitive). Used by
// the tests (FakeRemoteClient's role for the host) and, behind the
// MC_DEBUG_MEMORY_PROVIDER environment switch, as a sample tree a panel can
// open (phase 2). Fault injection lets tests exercise failures, dropped
// connections and reconnects without a server.

#include "FileProvider.h"

#include <map>
#include <string>

namespace mc {

class MemoryProvider final : public IFileProvider {
public:
    explicit MemoryProvider(std::wstring displayRoot = L"mem://",
                            uint32_t caps = CapRead | CapWrite | CapMkdir | CapDelete | CapRename);

    // --- building and inspecting the tree (tests) ---
    void AddDirectory(const std::wstring& path, uint64_t modifiedUtc = 0);
    void AddFile(const std::wstring& path, const std::string& content, uint64_t modifiedUtc = 0);
    // A link at `path`; `toDirectory` says what it points to. Opening it as a
    // folder lists `target`; reading it as a file reads `target`.
    void AddLink(const std::wstring& path, const std::wstring& target, bool toDirectory);
    bool Exists(const std::wstring& path) const;
    bool IsDirectory(const std::wstring& path) const;
    std::string Content(const std::wstring& path) const;
    uint64_t Modified(const std::wstring& path) const;
    size_t NodeCount() const { return nodes_.size(); }

    // --- fault injection ---
    // The next write stream fails once its total passes `bytes` (0 = off).
    void FailNextWriteAfter(uint64_t bytes) { failWriteAfter_ = bytes; }
    // After `bytes` more bytes are read or written, the connection drops:
    // every call returns ConnectionLost until Reconnect (0 = off).
    void DropConnectionAfter(uint64_t bytes) { dropAfter_ = bytes; }
    // Fails every call naming `path` (Delete, Rename, MakeDirectory, OpenWrite).
    void FailPath(const std::wstring& path) { failPath_ = path; }
    void SetCapabilities(uint32_t caps) { caps_ = caps; }
    // Adds CapFileTransfer (whole-file DownloadToFile/UploadFromFile), like
    // the FTP/SFTP plugins, and counts the calls.
    void EnableFileTransfer(bool on) { transfer_ = on; }
    int DownloadCount() const { return downloads_; }
    int UploadCount() const { return uploads_; }
    void SetPendingChanges(bool pending) { pending_ = pending; }
    bool Connected() const { return connected_; }
    int ReconnectCount() const { return reconnects_; }
    int CommitCount() const { return commits_; }
    int ReloadCount() const { return reloads_; }
    uint32_t Attributes(const std::wstring& path) const;

    // --- IFileProvider ---
    uint32_t Capabilities() const override { return caps_ | (transfer_ ? CapFileTransfer : 0u); }
    std::wstring DisplayRoot() const override { return displayRoot_; }
    std::wstring Kind() const override { return L"memory"; }
    std::wstring Join(const std::wstring& dir, const std::wstring& name) const override;
    std::wstring Parent(const std::wstring& path) const override;
    bool IsRoot(const std::wstring& path) const override { return path == L"/" || path.empty(); }

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
    bool HasPendingChanges() const override { return pending_; }
    ProviderStatus Reconnect(const ProviderCancelPoll& cancel, std::wstring& error) override;
    ProviderStatus SetAttributes(const std::wstring& path, uint32_t attributes, std::wstring& error) override;
    ProviderStatus Reload(std::wstring& error) override;
    ProviderStatus DownloadToFile(const std::wstring& path, const std::wstring& localPath, uint64_t sizeHint,
                                  const ProviderByteProgress& progress, std::wstring& error) override;
    ProviderStatus UploadFromFile(const std::wstring& localPath, const std::wstring& path, uint64_t modifiedUtc,
                                  const ProviderByteProgress& progress, std::wstring& error) override;

    // Internal (streams): counts transferred bytes toward DropConnectionAfter.
    bool ConsumeBytes(uint64_t bytes);
    void StoreFile(const std::wstring& path, std::string content, uint64_t modifiedUtc);

private:
    struct Node {
        bool isDirectory = false;
        bool isLink = false;
        std::wstring linkTarget;
        std::string data;
        uint64_t modified = 0;
        uint32_t attributes = 0;
    };
    std::map<std::wstring, Node> nodes_;
    std::wstring displayRoot_;
    uint32_t caps_;
    uint64_t failWriteAfter_ = 0;
    uint64_t dropAfter_ = 0;
    std::wstring failPath_;
    bool connected_ = true;
    bool pending_ = false;
    int reconnects_ = 0;
    int reloads_ = 0;
    bool transfer_ = false;
    int downloads_ = 0;
    int uploads_ = 0;
    int commits_ = 0;

    static std::wstring Normalize(const std::wstring& path);
    // Follows a link at `path` (one level) to the node it names.
    const Node* Resolve(const std::wstring& path, std::wstring* resolvedPath = nullptr) const;
    ProviderStatus CheckConnected(std::wstring& error) const;
    ProviderStatus CheckFailPath(const std::wstring& path, std::wstring& error) const;
    bool HasChildren(const std::wstring& path) const;
};

}  // namespace mc
