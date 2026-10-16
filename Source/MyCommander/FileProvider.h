#pragma once

// EXT-002 (D1): the common file-system provider interface. The local disk
// (LocalProvider), in-memory test trees (MemoryProvider) and plugin file
// systems (PluginProviderAdapter: ZIP archives, FTP, SFTP) all implement it,
// so the host can list, copy, move, delete and rename on any of them with one
// code path (ProviderCopy). Paths are provider-native strings: "C:\a\b" for
// the local disk, "/a/b" for a server or an archive. Pure: no Console/Dialog.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace mc {

struct ProviderEntry {
    std::wstring name;
    bool isDirectory = false;
    // A symbolic link or other reparse point. With isDirectory, a link to a
    // folder: it can be opened, but copy and delete never descend into it
    // (SEC-006).
    bool isLink = false;
    uint64_t size = 0;
    uint64_t modifiedUtc = 0;  // FILETIME ticks, 0 = unknown
    uint32_t attributes = 0;   // Win32 FILE_ATTRIBUTE_* where meaningful, else 0
    std::wstring permissions;  // "rwxr-xr-x" on servers, empty elsewhere
};

enum class ProviderStatus { Ok, Failed, Cancelled, ConnectionLost, NotSupported, NotFound };

enum ProviderCaps : uint32_t {
    CapRead = 1,
    CapWrite = 2,
    CapMkdir = 4,
    CapDelete = 8,
    CapRename = 16,
    CapAttributes = 32,
    CapServerSideCopy = 64,
    // Whole-file DownloadToFile/UploadFromFile (network protocols that move a
    // file in one call). ProviderCopy prefers it whenever one side is the
    // local disk, and stages through a local temporary file between two
    // providers when either side has it.
    CapFileTransfer = 128,
};

using ProviderCancelPoll = std::function<bool()>;
// Byte progress for a whole-file transfer; returning false cancels it.
using ProviderByteProgress = std::function<bool(uint64_t done, uint64_t total)>;

class IReadStream {
public:
    virtual ~IReadStream() = default;
    // `got` == 0 with Ok means end of file.
    virtual ProviderStatus Read(void* buffer, size_t capacity, size_t& got, std::wstring& error) = 0;
};

// Data written is invisible under the target name until Finish succeeds;
// Abort (or destroying the stream without Finish) discards it and leaves an
// existing target untouched (FTP-003/SFTP-003/FOP-012).
class IWriteStream {
public:
    virtual ~IWriteStream() = default;
    virtual ProviderStatus Write(const void* data, size_t size, std::wstring& error) = 0;
    virtual ProviderStatus Finish(std::wstring& error) = 0;
    virtual void Abort() = 0;
};

class IFileProvider {
public:
    virtual ~IFileProvider() = default;

    virtual uint32_t Capabilities() const = 0;
    // Shown in the panel header in front of the path: "sftp://bob@host",
    // "C:\x\a.zip". Empty for the local disk.
    virtual std::wstring DisplayRoot() const = 0;
    // Short scheme-like name for messages: "local", "sftp", "zip".
    virtual std::wstring Kind() const = 0;
    // True when the provider's paths are plain local Windows paths, so the
    // host may hand them to FileOps, external programs and the viewer.
    virtual bool IsLocal() const { return false; }

    virtual std::wstring Join(const std::wstring& dir, const std::wstring& name) const = 0;
    virtual std::wstring Parent(const std::wstring& path) const = 0;
    virtual bool IsRoot(const std::wstring& path) const = 0;
    virtual bool SamePath(const std::wstring& a, const std::wstring& b) const { return a == b; }

    virtual ProviderStatus List(const std::wstring& dir, std::vector<ProviderEntry>& entries,
                                const ProviderCancelPoll& cancel, std::wstring& error) = 0;
    // NotFound when nothing exists at `path`.
    virtual ProviderStatus Stat(const std::wstring& path, ProviderEntry& entry, const ProviderCancelPoll& cancel,
                                std::wstring& error) = 0;
    virtual ProviderStatus OpenRead(const std::wstring& path, std::unique_ptr<IReadStream>& stream,
                                    const ProviderCancelPoll& cancel, std::wstring& error) = 0;
    // Replaces `path` on Finish. `modifiedUtc` (0 = unknown) is applied where
    // the provider can store it.
    virtual ProviderStatus OpenWrite(const std::wstring& path, uint64_t sizeHint, uint64_t modifiedUtc,
                                     std::unique_ptr<IWriteStream>& stream, const ProviderCancelPoll& cancel,
                                     std::wstring& error) = 0;
    virtual ProviderStatus MakeDirectory(const std::wstring& path, const ProviderCancelPoll& cancel,
                                         std::wstring& error) = 0;
    // Removes one file, link, or EMPTY directory; trees go through
    // ProviderDeleteItems, which never follows a link. `isDirectory` is true
    // only for a real folder -- a link, even one to a folder, comes with
    // false and is removed as itself.
    virtual ProviderStatus Delete(const std::wstring& path, bool isDirectory, const ProviderCancelPoll& cancel,
                                  std::wstring& error) = 0;
    // Never overwrites an existing target.
    virtual ProviderStatus Rename(const std::wstring& from, const std::wstring& to, const ProviderCancelPoll& cancel,
                                  std::wstring& error) = 0;
    // Persists pending changes (ZIP: save on leave). Ok when nothing is pending.
    virtual ProviderStatus Commit(const ProviderCancelPoll&, std::wstring&) { return ProviderStatus::Ok; }
    virtual bool HasPendingChanges() const { return false; }
    // Ask "Leave <DisplayRoot()>?" even when nothing is pending (ZIP-007).
    virtual bool AsksBeforeLeaving() const { return false; }
    // CapAttributes: stores Win32 FILE_ATTRIBUTE_* bits on `path`.
    virtual ProviderStatus SetAttributes(const std::wstring&, uint32_t, std::wstring&) {
        return ProviderStatus::NotSupported;
    }
    // The user asked to re-read (Ctrl+R): drop caches / re-read the source.
    virtual ProviderStatus Reload(std::wstring&) { return ProviderStatus::Ok; }
    // Re-opens a session after ConnectionLost.
    virtual ProviderStatus Reconnect(const ProviderCancelPoll&, std::wstring&) { return ProviderStatus::NotSupported; }

    // CapFileTransfer. DownloadToFile writes `path` into the local file
    // `localPath` (created/truncated; the caller renames it into place).
    // UploadFromFile replaces `path` with the local file only on success --
    // an existing file there survives a failure or a cancel, and no partial
    // upload is left behind (FTP-003/SFTP-003).
    virtual ProviderStatus DownloadToFile(const std::wstring&, const std::wstring&, uint64_t /*sizeHint*/,
                                          const ProviderByteProgress&, std::wstring&) {
        return ProviderStatus::NotSupported;
    }
    virtual ProviderStatus UploadFromFile(const std::wstring&, const std::wstring&, uint64_t /*modifiedUtc*/,
                                          const ProviderByteProgress&, std::wstring&) {
        return ProviderStatus::NotSupported;
    }
};

const wchar_t* ProviderStatusText(ProviderStatus status);

// True when `path` is `ancestor` itself or lies anywhere beneath it, walking
// Parent() up to the root (no string-prefix guessing about separators).
bool ProviderPathIsSameOrWithin(const IFileProvider& provider, const std::wstring& ancestor, const std::wstring& path);

// The last component of `path` ("b" for "/a/b"), via Parent().
std::wstring ProviderLeafName(const IFileProvider& provider, const std::wstring& path);

}  // namespace mc
