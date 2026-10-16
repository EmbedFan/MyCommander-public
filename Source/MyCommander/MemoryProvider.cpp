#include "MemoryProvider.h"

#include <windows.h>

#include <algorithm>
#include <cstring>

namespace mc {

namespace {

class MemoryReadStream final : public IReadStream {
public:
    MemoryReadStream(MemoryProvider& owner, std::string data) : owner_(owner), data_(std::move(data)) {}
    ProviderStatus Read(void* buffer, size_t capacity, size_t& got, std::wstring& error) override {
        got = (std::min)(capacity, data_.size() - offset_);
        if (got > 0 && !owner_.ConsumeBytes(got)) {
            got = 0;
            error = L"the connection was closed by the server";
            return ProviderStatus::ConnectionLost;
        }
        std::memcpy(buffer, data_.data() + offset_, got);
        offset_ += got;
        return ProviderStatus::Ok;
    }

private:
    MemoryProvider& owner_;
    std::string data_;
    size_t offset_ = 0;
};

class MemoryWriteStream final : public IWriteStream {
public:
    MemoryWriteStream(MemoryProvider& owner, std::wstring path, uint64_t modified, uint64_t failAfter)
        : owner_(owner), path_(std::move(path)), modified_(modified), failAfter_(failAfter) {}
    ProviderStatus Write(const void* data, size_t size, std::wstring& error) override {
        if (done_) {
            error = L"stream closed";
            return ProviderStatus::Failed;
        }
        if (!owner_.ConsumeBytes(size)) {
            error = L"the connection was closed by the server";
            return ProviderStatus::ConnectionLost;
        }
        buffer_.append(static_cast<const char*>(data), size);
        if (failAfter_ != 0 && buffer_.size() > failAfter_) {
            error = L"disk full (injected)";
            return ProviderStatus::Failed;
        }
        return ProviderStatus::Ok;
    }
    ProviderStatus Finish(std::wstring& error) override {
        if (done_) {
            error = L"stream closed";
            return ProviderStatus::Failed;
        }
        done_ = true;
        owner_.StoreFile(path_, std::move(buffer_), modified_);
        return ProviderStatus::Ok;
    }
    void Abort() override { done_ = true; }

private:
    MemoryProvider& owner_;
    std::wstring path_;
    uint64_t modified_;
    uint64_t failAfter_;
    std::string buffer_;
    bool done_ = false;
};

}  // namespace

MemoryProvider::MemoryProvider(std::wstring displayRoot, uint32_t caps)
    : displayRoot_(std::move(displayRoot)), caps_(caps) {
    nodes_[L"/"].isDirectory = true;
}

std::wstring MemoryProvider::Normalize(const std::wstring& path) {
    if (path.empty()) return L"/";
    std::wstring p = path;
    while (p.size() > 1 && p.back() == L'/') p.pop_back();
    if (p[0] != L'/') p = L"/" + p;
    return p;
}

std::wstring MemoryProvider::Join(const std::wstring& dir, const std::wstring& name) const {
    std::wstring d = Normalize(dir);
    return d == L"/" ? L"/" + name : d + L"/" + name;
}

std::wstring MemoryProvider::Parent(const std::wstring& path) const {
    std::wstring p = Normalize(path);
    if (p == L"/") return p;
    size_t slash = p.rfind(L'/');
    return slash == 0 ? L"/" : p.substr(0, slash);
}

void MemoryProvider::AddDirectory(const std::wstring& path, uint64_t modifiedUtc) {
    std::wstring p = Normalize(path);
    if (p != L"/") AddDirectory(Parent(p));
    Node& n = nodes_[p];
    n.isDirectory = true;
    if (modifiedUtc) n.modified = modifiedUtc;
}

void MemoryProvider::AddFile(const std::wstring& path, const std::string& content, uint64_t modifiedUtc) {
    std::wstring p = Normalize(path);
    AddDirectory(Parent(p));
    Node& n = nodes_[p];
    n = Node{};
    n.data = content;
    n.modified = modifiedUtc;
}

void MemoryProvider::AddLink(const std::wstring& path, const std::wstring& target, bool toDirectory) {
    std::wstring p = Normalize(path);
    AddDirectory(Parent(p));
    Node& n = nodes_[p];
    n = Node{};
    n.isLink = true;
    n.isDirectory = toDirectory;
    n.linkTarget = Normalize(target);
}

bool MemoryProvider::Exists(const std::wstring& path) const { return nodes_.count(Normalize(path)) != 0; }

bool MemoryProvider::IsDirectory(const std::wstring& path) const {
    auto it = nodes_.find(Normalize(path));
    return it != nodes_.end() && it->second.isDirectory;
}

std::string MemoryProvider::Content(const std::wstring& path) const {
    const Node* n = Resolve(path);
    return n ? n->data : std::string();
}

// cppcheck-suppress unusedFunction -- used by PluginProviderAdapterTests and ProviderCopyTests (not scanned)
uint64_t MemoryProvider::Modified(const std::wstring& path) const {
    auto it = nodes_.find(Normalize(path));
    return it == nodes_.end() ? 0 : it->second.modified;
}

const MemoryProvider::Node* MemoryProvider::Resolve(const std::wstring& path, std::wstring* resolvedPath) const {
    std::wstring p = Normalize(path);
    auto it = nodes_.find(p);
    if (it == nodes_.end()) return nullptr;
    if (it->second.isLink) {
        p = it->second.linkTarget;
        it = nodes_.find(p);
        if (it == nodes_.end()) return nullptr;
    }
    if (resolvedPath) *resolvedPath = p;
    return &it->second;
}

bool MemoryProvider::HasChildren(const std::wstring& path) const {
    std::wstring prefix = Normalize(path);
    prefix = prefix == L"/" ? prefix : prefix + L"/";
    auto it = nodes_.upper_bound(prefix);
    if (prefix == L"/") return nodes_.size() > 1;
    return it != nodes_.end() && it->first.compare(0, prefix.size(), prefix) == 0;
}

bool MemoryProvider::ConsumeBytes(uint64_t bytes) {
    if (!connected_) return false;
    if (dropAfter_ == 0) return true;
    if (bytes >= dropAfter_) {
        dropAfter_ = 0;
        connected_ = false;
        return false;
    }
    dropAfter_ -= bytes;
    return true;
}

void MemoryProvider::StoreFile(const std::wstring& path, std::string content, uint64_t modifiedUtc) {
    Node& n = nodes_[Normalize(path)];
    n = Node{};
    n.data = std::move(content);
    n.modified = modifiedUtc;
    pending_ = true;
}

ProviderStatus MemoryProvider::CheckConnected(std::wstring& error) const {
    if (connected_) return ProviderStatus::Ok;
    error = L"not connected";
    return ProviderStatus::ConnectionLost;
}

ProviderStatus MemoryProvider::CheckFailPath(const std::wstring& path, std::wstring& error) const {
    if (!failPath_.empty() && Normalize(path) == Normalize(failPath_)) {
        error = path + L": permission denied (injected)";
        return ProviderStatus::Failed;
    }
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::List(const std::wstring& dir, std::vector<ProviderEntry>& entries,
                                    const ProviderCancelPoll& cancel, std::wstring& error) {
    entries.clear();
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    if (cancel && cancel()) return ProviderStatus::Cancelled;
    std::wstring resolved;
    const Node* n = Resolve(dir, &resolved);
    if (!n) {
        error = dir + L": no such folder";
        return ProviderStatus::NotFound;
    }
    if (!n->isDirectory) {
        error = dir + L": not a folder";
        return ProviderStatus::Failed;
    }
    for (const auto& [path, node] : nodes_) {
        if (path == L"/" || Parent(path) != resolved) continue;
        ProviderEntry e;
        e.name = path.substr(path.rfind(L'/') + 1);
        e.isDirectory = node.isDirectory;
        e.isLink = node.isLink;
        const Node* target = node.isLink ? Resolve(path) : &node;
        e.size = (target && !target->isDirectory) ? target->data.size() : 0;
        e.modifiedUtc = node.modified;
        e.attributes = node.attributes;
        entries.push_back(std::move(e));
    }
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::Stat(const std::wstring& path, ProviderEntry& entry, const ProviderCancelPoll&,
                                    std::wstring& error) {
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    std::wstring p = Normalize(path);
    auto it = nodes_.find(p);
    if (it == nodes_.end()) {
        error = path + L": not found";
        return ProviderStatus::NotFound;
    }
    entry = ProviderEntry{};
    entry.name = p == L"/" ? p : p.substr(p.rfind(L'/') + 1);
    entry.isDirectory = it->second.isDirectory;
    entry.isLink = it->second.isLink;
    const Node* target = Resolve(p);
    entry.size = (target && !target->isDirectory) ? target->data.size() : 0;
    entry.modifiedUtc = it->second.modified;
    entry.attributes = it->second.attributes;
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::OpenRead(const std::wstring& path, std::unique_ptr<IReadStream>& stream,
                                        const ProviderCancelPoll&, std::wstring& error) {
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    const Node* n = Resolve(path);
    if (!n) {
        error = path + L": not found";
        return ProviderStatus::NotFound;
    }
    if (n->isDirectory) {
        error = path + L": is a folder";
        return ProviderStatus::Failed;
    }
    stream = std::make_unique<MemoryReadStream>(*this, n->data);
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::OpenWrite(const std::wstring& path, uint64_t, uint64_t modifiedUtc,
                                         std::unique_ptr<IWriteStream>& stream, const ProviderCancelPoll&,
                                         std::wstring& error) {
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    if (!(caps_ & CapWrite)) {
        error = L"read-only";
        return ProviderStatus::NotSupported;
    }
    if (CheckFailPath(path, error) != ProviderStatus::Ok) return ProviderStatus::Failed;
    std::wstring p = Normalize(path);
    if (!IsDirectory(Parent(p))) {
        error = path + L": the folder does not exist";
        return ProviderStatus::NotFound;
    }
    if (IsDirectory(p)) {
        error = path + L": is a folder";
        return ProviderStatus::Failed;
    }
    uint64_t failAfter = failWriteAfter_;
    failWriteAfter_ = 0;
    stream = std::make_unique<MemoryWriteStream>(*this, p, modifiedUtc, failAfter);
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::MakeDirectory(const std::wstring& path, const ProviderCancelPoll&,
                                             std::wstring& error) {
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    if (!(caps_ & CapMkdir)) return ProviderStatus::NotSupported;
    if (CheckFailPath(path, error) != ProviderStatus::Ok) return ProviderStatus::Failed;
    std::wstring p = Normalize(path);
    if (Exists(p)) {
        error = path + L": already exists";
        return ProviderStatus::Failed;
    }
    if (!IsDirectory(Parent(p))) {
        error = path + L": the parent folder does not exist";
        return ProviderStatus::NotFound;
    }
    nodes_[p].isDirectory = true;
    pending_ = true;
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::Delete(const std::wstring& path, bool, const ProviderCancelPoll&, std::wstring& error) {
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    if (!(caps_ & CapDelete)) return ProviderStatus::NotSupported;
    if (CheckFailPath(path, error) != ProviderStatus::Ok) return ProviderStatus::Failed;
    std::wstring p = Normalize(path);
    auto it = nodes_.find(p);
    if (it == nodes_.end() || p == L"/") {
        error = path + L": not found";
        return ProviderStatus::NotFound;
    }
    if (it->second.isDirectory && !it->second.isLink && HasChildren(p)) {
        error = path + L": the folder is not empty";
        return ProviderStatus::Failed;
    }
    nodes_.erase(it);
    pending_ = true;
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::Rename(const std::wstring& from, const std::wstring& to, const ProviderCancelPoll&,
                                      std::wstring& error) {
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    if (!(caps_ & CapRename)) return ProviderStatus::NotSupported;
    if (CheckFailPath(from, error) != ProviderStatus::Ok) return ProviderStatus::Failed;
    std::wstring a = Normalize(from), b = Normalize(to);
    if (!Exists(a)) {
        error = from + L": not found";
        return ProviderStatus::NotFound;
    }
    if (Exists(b)) {
        error = to + L": already exists";
        return ProviderStatus::Failed;
    }
    if (!IsDirectory(Parent(b))) {
        error = to + L": the folder does not exist";
        return ProviderStatus::NotFound;
    }
    // Move the node and, for a folder, everything beneath it.
    std::map<std::wstring, Node> moved;
    std::wstring prefix = a + L"/";
    for (auto it = nodes_.begin(); it != nodes_.end();) {
        if (it->first == a || it->first.compare(0, prefix.size(), prefix) == 0) {
            moved[b + it->first.substr(a.size())] = std::move(it->second);
            it = nodes_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto& [k, v] : moved) nodes_[k] = std::move(v);
    pending_ = true;
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::Commit(const ProviderCancelPoll&, std::wstring&) {
    ++commits_;
    pending_ = false;
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::Reconnect(const ProviderCancelPoll&, std::wstring&) {
    connected_ = true;
    ++reconnects_;
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::DownloadToFile(const std::wstring& path, const std::wstring& localPath, uint64_t,
                                              const ProviderByteProgress& progress, std::wstring& error) {
    if (!transfer_) return ProviderStatus::NotSupported;
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    const Node* n = Resolve(path);
    if (!n || n->isDirectory) {
        error = path + L": not a file";
        return n ? ProviderStatus::Failed : ProviderStatus::NotFound;
    }
    ++downloads_;
    std::string data = n->data;
    HANDLE h = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = localPath + L": cannot create";
        return ProviderStatus::Failed;
    }
    size_t done = 0;
    ProviderStatus status = ProviderStatus::Ok;
    while (done < data.size() || done == 0) {
        size_t chunk = (std::min)(static_cast<size_t>(65536), data.size() - done);
        // chunk is 0 for an empty file (the loop runs once when done == 0), so the test is needed;
        // cppcheck assumes done < data.size().
        // cppcheck-suppress knownConditionTrueFalse
        if (chunk > 0 && !ConsumeBytes(chunk)) {
            error = L"the connection was closed by the server";
            status = ProviderStatus::ConnectionLost;
            break;
        }
        DWORD written = 0;
        // cppcheck-suppress knownConditionTrueFalse -- empty file: chunk == 0 (see above)
        if (chunk > 0) WriteFile(h, data.data() + done, static_cast<DWORD>(chunk), &written, nullptr);
        done += chunk;
        if (progress && !progress(done, data.size())) {
            status = ProviderStatus::Cancelled;
            break;
        }
        if (chunk == 0) break;
    }
    CloseHandle(h);
    if (status != ProviderStatus::Ok) DeleteFileW(localPath.c_str());
    return status;
}

ProviderStatus MemoryProvider::UploadFromFile(const std::wstring& localPath, const std::wstring& path,
                                              uint64_t modifiedUtc, const ProviderByteProgress& progress,
                                              std::wstring& error) {
    if (!transfer_) return ProviderStatus::NotSupported;
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    if (CheckFailPath(path, error) != ProviderStatus::Ok) return ProviderStatus::Failed;
    if (!IsDirectory(Parent(Normalize(path)))) {
        error = path + L": the folder does not exist";
        return ProviderStatus::NotFound;
    }
    ++uploads_;
    HANDLE h = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        error = localPath + L": cannot open";
        return ProviderStatus::Failed;
    }
    LARGE_INTEGER size{};
    GetFileSizeEx(h, &size);
    std::string data;
    char buffer[65536];
    DWORD read = 0;
    ProviderStatus status = ProviderStatus::Ok;
    while (ReadFile(h, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        if (!ConsumeBytes(read)) {
            error = L"the connection was closed by the server";
            status = ProviderStatus::ConnectionLost;
            break;
        }
        data.append(buffer, read);
        if (progress && !progress(data.size(), static_cast<uint64_t>(size.QuadPart))) {
            status = ProviderStatus::Cancelled;
            break;
        }
        if (failWriteAfter_ != 0 && data.size() > failWriteAfter_) {
            failWriteAfter_ = 0;
            error = L"disk full (injected)";
            status = ProviderStatus::Failed;
            break;
        }
    }
    CloseHandle(h);
    if (status == ProviderStatus::Ok) StoreFile(path, std::move(data), modifiedUtc);  // only now: the old file survives a failure
    return status;
}

uint32_t MemoryProvider::Attributes(const std::wstring& path) const {
    auto it = nodes_.find(Normalize(path));
    return it == nodes_.end() ? 0 : it->second.attributes;
}

ProviderStatus MemoryProvider::SetAttributes(const std::wstring& path, uint32_t attributes, std::wstring& error) {
    if (CheckConnected(error) != ProviderStatus::Ok) return ProviderStatus::ConnectionLost;
    if (!(caps_ & CapAttributes)) return ProviderStatus::NotSupported;
    if (CheckFailPath(path, error) != ProviderStatus::Ok) return ProviderStatus::Failed;
    auto it = nodes_.find(Normalize(path));
    if (it == nodes_.end()) {
        error = path + L": not found";
        return ProviderStatus::NotFound;
    }
    it->second.attributes = attributes;
    pending_ = true;
    return ProviderStatus::Ok;
}

ProviderStatus MemoryProvider::Reload(std::wstring&) {
    ++reloads_;
    return ProviderStatus::Ok;
}

}  // namespace mc
