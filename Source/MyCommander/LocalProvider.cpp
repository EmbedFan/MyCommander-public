#include "LocalProvider.h"

#include "PathUtil.h"

#include <windows.h>

namespace mc {

namespace fs = std::filesystem;

namespace {

uint64_t ToTicks(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

ProviderEntry FromFindData(const WIN32_FIND_DATAW& fd) {
    ProviderEntry e;
    e.name = fd.cFileName;
    e.attributes = fd.dwFileAttributes;
    e.isDirectory = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    e.isLink = (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    e.size = e.isDirectory ? 0 : (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
    e.modifiedUtc = ToTicks(fd.ftLastWriteTime);
    return e;
}

ProviderStatus Win32Failure(const std::wstring& path, std::wstring& error) {
    DWORD code = GetLastError();
    error = path + L": " + FormatWinError(code);
    return (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) ? ProviderStatus::NotFound
                                                                          : ProviderStatus::Failed;
}

class LocalReadStream final : public IReadStream {
public:
    explicit LocalReadStream(HANDLE h) : h_(h) {}
    ~LocalReadStream() override { CloseHandle(h_); }
    ProviderStatus Read(void* buffer, size_t capacity, size_t& got, std::wstring& error) override {
        DWORD read = 0;
        if (!ReadFile(h_, buffer, static_cast<DWORD>(capacity), &read, nullptr)) {
            error = FormatWinError(GetLastError());
            got = 0;
            return ProviderStatus::Failed;
        }
        got = read;
        return ProviderStatus::Ok;
    }

private:
    HANDLE h_;
};

class LocalWriteStream final : public IWriteStream {
public:
    LocalWriteStream(HANDLE h, fs::path target, fs::path part, uint64_t modifiedUtc)
        : h_(h), target_(std::move(target)), part_(std::move(part)), modifiedUtc_(modifiedUtc) {}
    ~LocalWriteStream() override { Abort(); }

    ProviderStatus Write(const void* data, size_t size, std::wstring& error) override {
        const char* p = static_cast<const char*>(data);
        while (size > 0) {
            DWORD chunk = static_cast<DWORD>(size > 0x40000000 ? 0x40000000 : size);
            DWORD written = 0;
            if (!WriteFile(h_, p, chunk, &written, nullptr)) {
                error = target_.wstring() + L": " + FormatWinError(GetLastError());
                return ProviderStatus::Failed;
            }
            p += written;
            size -= written;
        }
        return ProviderStatus::Ok;
    }

    ProviderStatus Finish(std::wstring& error) override {
        if (h_ == INVALID_HANDLE_VALUE) {
            error = L"stream already closed";
            return ProviderStatus::Failed;
        }
        CloseHandle(h_);
        h_ = INVALID_HANDLE_VALUE;
        finished_ = true;
        return CommitLocalPart(part_.wstring(), target_.wstring(), modifiedUtc_, error);
    }

    void Abort() override {
        if (h_ != INVALID_HANDLE_VALUE) {
            CloseHandle(h_);
            h_ = INVALID_HANDLE_VALUE;
        }
        if (!finished_) {
            DeleteFileW(ToLongPath(part_).c_str());
            finished_ = true;
        }
    }

private:
    HANDLE h_;
    fs::path target_;
    fs::path part_;
    uint64_t modifiedUtc_;
    bool finished_ = false;
};

}  // namespace

ProviderStatus CommitLocalPart(const std::wstring& part, const std::wstring& target, uint64_t modifiedUtc,
                               std::wstring& error) {
    fs::path partPath = ToLongPath(fs::path(part));
    fs::path targetPath = ToLongPath(fs::path(target));
    if (modifiedUtc != 0) {
        HANDLE h = CreateFileW(partPath.c_str(), FILE_WRITE_ATTRIBUTES, 0, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h != INVALID_HANDLE_VALUE) {
            FILETIME ft{static_cast<DWORD>(modifiedUtc), static_cast<DWORD>(modifiedUtc >> 32)};
            SetFileTime(h, nullptr, nullptr, &ft);
            CloseHandle(h);
        }
    }
    // A read-only target would refuse the replace; the copy is meant to
    // overwrite it (the conflict prompt already asked).
    DWORD attrs = GetFileAttributesW(targetPath.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(targetPath.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    if (!MoveFileExW(partPath.c_str(), targetPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        error = target + L": " + FormatWinError(GetLastError());
        DeleteFileW(partPath.c_str());
        return ProviderStatus::Failed;
    }
    return ProviderStatus::Ok;
}

void DiscardLocalPart(const std::wstring& part) { DeleteFileW(ToLongPath(fs::path(part)).c_str()); }

uint32_t LocalProvider::Capabilities() const {
    return CapRead | CapWrite | CapMkdir | CapDelete | CapRename | CapAttributes;
}

std::wstring LocalProvider::Join(const std::wstring& dir, const std::wstring& name) const {
    return (fs::path(dir) / name).wstring();
}

std::wstring LocalProvider::Parent(const std::wstring& path) const {
    fs::path p(path);
    if (IsFilesystemRoot(p)) return path;
    fs::path parent = p.parent_path();
    return parent.empty() ? path : parent.wstring();
}

bool LocalProvider::IsRoot(const std::wstring& path) const { return IsFilesystemRoot(fs::path(path)); }

bool LocalProvider::SamePath(const std::wstring& a, const std::wstring& b) const {
    // Lexical and case-insensitive first: the copy engine also compares paths
    // that don't exist yet (a destination), which PathsEquivalent can't.
    auto norm = [](const std::wstring& s) {
        std::wstring n = fs::path(s).lexically_normal().make_preferred().wstring();
        while (n.size() > 3 && n.back() == L'\\') n.pop_back();
        return n;
    };
    std::wstring x = norm(a), y = norm(b);
    if (CompareStringOrdinal(x.c_str(), static_cast<int>(x.size()), y.c_str(), static_cast<int>(y.size()), TRUE) ==
        CSTR_EQUAL)
        return true;
    return PathsEquivalent(fs::path(a), fs::path(b));  // e.g. an 8.3 short name of the same folder
}

ProviderStatus LocalProvider::List(const std::wstring& dir, std::vector<ProviderEntry>& entries,
                                   const ProviderCancelPoll& cancel, std::wstring& error) {
    entries.clear();
    fs::path pattern = ToLongPath(fs::path(dir)) / L"*";
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW(pattern.c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) {
        if (GetLastError() == ERROR_FILE_NOT_FOUND) return ProviderStatus::Ok;  // an empty volume root
        return Win32Failure(dir, error);
    }
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        entries.push_back(FromFindData(fd));
        if (cancel && entries.size() % 256 == 0 && cancel()) {
            FindClose(h);
            return ProviderStatus::Cancelled;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return ProviderStatus::Ok;
}

ProviderStatus LocalProvider::Stat(const std::wstring& path, ProviderEntry& entry, const ProviderCancelPoll&,
                                   std::wstring& error) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ToLongPath(fs::path(path)).c_str(), GetFileExInfoStandard, &data))
        return Win32Failure(path, error);
    entry = ProviderEntry{};
    entry.name = ProviderLeafName(*this, path);
    entry.attributes = data.dwFileAttributes;
    entry.isDirectory = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    entry.isLink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    entry.size = entry.isDirectory ? 0 : (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
    entry.modifiedUtc = ToTicks(data.ftLastWriteTime);
    return ProviderStatus::Ok;
}

ProviderStatus LocalProvider::OpenRead(const std::wstring& path, std::unique_ptr<IReadStream>& stream,
                                       const ProviderCancelPoll&, std::wstring& error) {
    HANDLE h = CreateFileW(ToLongPath(fs::path(path)).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE,
                           nullptr, OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Win32Failure(path, error);
    stream = std::make_unique<LocalReadStream>(h);
    return ProviderStatus::Ok;
}

ProviderStatus LocalProvider::OpenWrite(const std::wstring& path, uint64_t, uint64_t modifiedUtc,
                                        std::unique_ptr<IWriteStream>& stream, const ProviderCancelPoll&,
                                        std::wstring& error) {
    fs::path target(path);
    fs::path part = fs::path(path + kLocalPartSuffix);
    HANDLE h = CreateFileW(ToLongPath(part).c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return Win32Failure(path, error);
    stream = std::make_unique<LocalWriteStream>(h, target, part, modifiedUtc);
    return ProviderStatus::Ok;
}

ProviderStatus LocalProvider::MakeDirectory(const std::wstring& path, const ProviderCancelPoll&, std::wstring& error) {
    if (!CreateDirectoryW(ToLongPath(fs::path(path)).c_str(), nullptr)) return Win32Failure(path, error);
    return ProviderStatus::Ok;
}

ProviderStatus LocalProvider::Delete(const std::wstring& path, bool isDirectory, const ProviderCancelPoll&,
                                     std::wstring& error) {
    fs::path p = ToLongPath(fs::path(path));
    DWORD attrs = GetFileAttributesW(p.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY))
        SetFileAttributesW(p.c_str(), attrs & ~FILE_ATTRIBUTE_READONLY);
    // A junction or directory symlink is removed with RemoveDirectoryW, which
    // deletes the link itself, never its target.
    bool dirLike = isDirectory || (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_DIRECTORY));
    BOOL ok = dirLike ? RemoveDirectoryW(p.c_str()) : DeleteFileW(p.c_str());
    if (!ok) return Win32Failure(path, error);
    return ProviderStatus::Ok;
}

ProviderStatus LocalProvider::SetAttributes(const std::wstring& path, uint32_t attributes, std::wstring& error) {
    constexpr uint32_t kSettable =
        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
    uint32_t bits = attributes & kSettable;
    if (!SetFileAttributesW(ToLongPath(fs::path(path)).c_str(), bits ? bits : FILE_ATTRIBUTE_NORMAL))
        return Win32Failure(path, error);
    return ProviderStatus::Ok;
}

ProviderStatus LocalProvider::Rename(const std::wstring& from, const std::wstring& to, const ProviderCancelPoll&,
                                     std::wstring& error) {
    if (!MoveFileExW(ToLongPath(fs::path(from)).c_str(), ToLongPath(fs::path(to)).c_str(), 0))
        return Win32Failure(from, error);
    return ProviderStatus::Ok;
}

}  // namespace mc
