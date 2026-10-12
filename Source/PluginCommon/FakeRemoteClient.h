#pragma once

// EXT-004 (D7): an in-memory remote::Client for the remote-panel harnesses --
// a tiny file tree with folders, files and symbolic links, so every panel flow
// (browse, transfers, conflicts, F7/F2/F8, links) can be tested with no
// network and no server. Header-only, test code only.

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

#include "RemoteClient.h"

#include <map>
#include <string>
#include <vector>

namespace fakeremote {

struct Node {
    bool dir = false;
    bool link = false;
    std::wstring target;  // a link's absolute target
    std::string data;     // a file's content
};

using Tree = std::map<std::wstring, Node>;  // absolute path -> node ("/" is implicit)

class FakeClient final : public remote::Client {
public:
    // `tree` outlives the client, so a test can inspect it after a disconnect.
    FakeClient(Tree& tree, std::wstring expectedSecret, std::wstring home)
        : tree_(tree), expectedSecret_(std::move(expectedSecret)), home_(std::move(home)) {}

    remote::Status Connect(const remote::Connection&, const std::wstring& secret, const remote::CancelFn&,
                           std::wstring& error) override {
        if (!expectedSecret_.empty() && secret != expectedSecret_) {
            error = L"Wrong user name or password.";
            return remote::Status::Failed;
        }
        connected_ = true;
        if (trace_) trace_(L"connected");
        return remote::Status::Ok;
    }
    void Disconnect() override { connected_ = false; }
    void SetTrace(remote::TraceFn trace) override { trace_ = std::move(trace); }

    remote::Status HomeDirectory(std::wstring& path, const remote::CancelFn&, std::wstring& error) override {
        if (!connected_) return Lost(error);
        path = home_;
        return remote::Status::Ok;
    }

    remote::Status List(const std::wstring& dir, std::vector<remote::RemoteEntry>& entries, const remote::CancelFn&,
                        std::wstring& error) override {
        if (!connected_) return Lost(error);
        std::wstring real = Resolve(dir);
        if (real != L"/" && (!tree_.count(real) || !tree_[real].dir)) return Failed(error, dir + L": No such folder.");
        entries.clear();
        for (const auto& [path, node] : tree_) {
            if (path == real || remote::ParentRemotePath(path) != real) continue;
            remote::RemoteEntry e;
            e.name = path.substr(path.find_last_of(L'/') + 1);
            e.isLink = node.link;
            const Node* target = node.link ? Find(node.target) : &node;
            e.isDirectory = target && target->dir;
            e.size = target && !target->dir ? target->data.size() : 0;
            e.permissions = e.isDirectory ? L"rwxr-xr-x" : L"rw-r--r--";
            entries.push_back(e);
        }
        return remote::Status::Ok;
    }

    remote::Status Download(const std::wstring& remotePath, const std::wstring& localPath, uint64_t,
                            const remote::ProgressFn& progress, const remote::CancelFn&, std::wstring& error) override {
        if (!connected_) return Lost(error);
        const Node* node = Find(Resolve(remotePath));
        if (!node || node->dir) return Failed(error, remotePath + L": No such file.");
        HANDLE h = CreateFileW(localPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Failed(error, L"Cannot create " + localPath);
        DWORD written = 0;
        WriteFile(h, node->data.data(), static_cast<DWORD>(node->data.size()), &written, nullptr);
        CloseHandle(h);
        if (progress) progress(node->data.size(), node->data.size());
        return remote::Status::Ok;
    }

    remote::Status Upload(const std::wstring& localPath, const std::wstring& remotePath,
                          const remote::ProgressFn& progress, const remote::CancelFn&, std::wstring& error) override {
        if (!connected_) return Lost(error);
        HANDLE h = CreateFileW(localPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) return Failed(error, L"Cannot read " + localPath);
        std::string data;
        char buffer[65536];
        DWORD read = 0;
        while (ReadFile(h, buffer, sizeof(buffer), &read, nullptr) && read > 0) data.append(buffer, read);
        CloseHandle(h);
        Node node;
        node.data = data;
        tree_[remotePath] = node;
        if (progress) progress(data.size(), data.size());
        return remote::Status::Ok;
    }

    remote::Status MakeRemoteDirectory(const std::wstring& path, const remote::CancelFn&, std::wstring& error) override {
        if (!connected_) return Lost(error);
        if (tree_.count(path)) return Failed(error, path + L": already exists.");
        Node node;
        node.dir = true;
        tree_[path] = node;
        return remote::Status::Ok;
    }

    remote::Status RemoveRemoteDirectory(const std::wstring& path, const remote::CancelFn&, std::wstring& error) override {
        if (!connected_) return Lost(error);
        if (!tree_.count(path) || !tree_[path].dir) return Failed(error, path + L": not a folder.");
        for (const auto& [p, n] : tree_)
            if (p != path && remote::ParentRemotePath(p) == path) return Failed(error, path + L": not empty.");
        tree_.erase(path);
        return remote::Status::Ok;
    }

    remote::Status DeleteRemoteFile(const std::wstring& path, const remote::CancelFn&, std::wstring& error) override {
        if (!connected_) return Lost(error);
        auto it = tree_.find(path);
        if (it == tree_.end() || it->second.dir) return Failed(error, path + L": not a file.");
        tree_.erase(it);  // a link is removed itself, never its target
        return remote::Status::Ok;
    }

    remote::Status RenameRemote(const std::wstring& from, const std::wstring& to, const remote::CancelFn&,
                                std::wstring& error) override {
        if (!connected_) return Lost(error);
        if (!tree_.count(from)) return Failed(error, from + L": No such file.");
        if (tree_.count(to)) return Failed(error, to + L": already exists.");
        Tree moved;
        for (auto it = tree_.begin(); it != tree_.end();) {
            if (it->first == from || it->first.rfind(from + L"/", 0) == 0) {
                moved[to + it->first.substr(from.size())] = it->second;
                it = tree_.erase(it);
            } else {
                ++it;
            }
        }
        tree_.insert(moved.begin(), moved.end());
        return remote::Status::Ok;
    }

private:
    Tree& tree_;
    std::wstring expectedSecret_;
    std::wstring home_;
    bool connected_ = false;
    remote::TraceFn trace_;

    const Node* Find(const std::wstring& path) const {
        auto it = tree_.find(path);
        return it == tree_.end() ? nullptr : &it->second;
    }
    // Follows links in any path component (absolute link targets only).
    std::wstring Resolve(const std::wstring& path) const {
        std::wstring resolved;
        size_t start = 1;
        while (start <= path.size()) {
            size_t slash = path.find(L'/', start);
            std::wstring part = path.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
            if (!part.empty()) {
                resolved += L"/" + part;
                const Node* node = Find(resolved);
                if (node && node->link) resolved = node->target;
            }
            if (slash == std::wstring::npos) break;
            start = slash + 1;
        }
        return resolved.empty() ? L"/" : resolved;
    }
    static remote::Status Failed(std::wstring& error, const std::wstring& text) {
        error = text;
        return remote::Status::Failed;
    }
    static remote::Status Lost(std::wstring& error) {
        error = L"Not connected.";
        return remote::Status::ConnectionLost;
    }
};

}  // namespace fakeremote
