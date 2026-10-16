#include "ProviderCopy.h"

#include "LocalProvider.h"

#include <windows.h>

#include <algorithm>
#include <memory>

namespace mc {

namespace {

constexpr wchar_t kStrOntoItself[] = L"cannot copy an item onto itself";
constexpr wchar_t kStrIntoItself[] = L"cannot copy or move a folder into itself";
constexpr wchar_t kStrFolderInTheWay[] = L"a folder with this name already exists";
constexpr wchar_t kStrFileInTheWay[] = L"a file with this name already exists";

enum class Step { Done, Skipped, Failed, Cancelled };

class Engine {
public:
    Engine(IFileProvider& src, IFileProvider& dst, const ProviderOpHandlers& h) : src_(src), dst_(dst), h_(h) {}

    OperationOutcome outcome;

    bool Cancelled() const { return h_.cancelPoll && h_.cancelPoll(); }
    ProviderCancelPoll Poll() const { return h_.cancelPoll; }
    bool SameProvider() const { return &src_ == &dst_; }

    bool StartItem(const std::wstring& name, int index, int total) {
        if (h_.onItemStart && !h_.onItemStart(name, index, total)) {
            outcome.cancelled = true;
            return false;
        }
        return true;
    }

    void Note(const std::wstring& message) {
        if (h_.onNote) h_.onNote(message);
    }

    // Runs `op` until it succeeds, the user skips or cancels, or (without an
    // error handler) once, recording the failure.
    Step Attempt(const std::wstring& name, const std::function<ProviderStatus(std::wstring&)>& op) {
        while (true) {
            std::wstring err;
            ProviderStatus s = op(err);
            if (s == ProviderStatus::Ok) return Step::Done;
            if (s == ProviderStatus::Cancelled) return Step::Cancelled;
            if (err.empty()) err = ProviderStatusText(s);
            if (s == ProviderStatus::ConnectionLost) err = std::wstring(L"connection lost: ") + err;
            if (!h_.onError) {
                outcome.failed.emplace_back(name, err);
                return Step::Failed;
            }
            ErrorChoice choice = ErrorChoice::Skip;
            if (!errorBlanket_) {
                ErrorAskResult r = h_.onError(name, err, 0);
                choice = r.choice;
                if (r.applyToAll) errorBlanket_ = true;
            }
            if (choice == ErrorChoice::Retry || choice == ErrorChoice::ElevateAndRetry) {
                if (s == ProviderStatus::ConnectionLost) ReconnectAll();
                continue;
            }
            if (choice == ErrorChoice::Skip) {
                ++outcome.skipped;
                return Step::Skipped;
            }
            return Step::Cancelled;
        }
    }

    enum class Resolve { Proceed, Skip, Fail, Cancel };

    // A file is about to be written at Join(dir, name). Asks about an
    // existing file there; may change `name` (Rename). `exists` says whether
    // the final target exists (Overwrite).
    Resolve ResolveFileConflict(IFileProvider& target, const std::wstring& dir, std::wstring& name, bool& exists,
                                const std::wstring& itemName) {
        for (int round = 0; round < 32; ++round) {
            ProviderEntry existing;
            std::wstring err;
            std::wstring path = target.Join(dir, name);
            ProviderStatus st = target.Stat(path, existing, Poll(), err);
            if (st == ProviderStatus::Cancelled) return Resolve::Cancel;
            if (st != ProviderStatus::Ok) {
                exists = false;  // NotFound, or a failure the write itself will report
                return Resolve::Proceed;
            }
            if (existing.isDirectory) {
                outcome.failed.emplace_back(itemName, path + L": " + kStrFolderInTheWay);
                return Resolve::Fail;
            }
            exists = true;
            ConflictChoice choice;
            std::wstring renameTo;
            if (haveConflictBlanket_) {
                choice = conflictBlanket_;
            } else if (h_.onConflict) {
                ConflictAskResult r = h_.onConflict(std::filesystem::path(path));
                choice = r.choice;
                renameTo = r.renameTo;
                if (r.applyToAll && choice != ConflictChoice::Rename) {
                    haveConflictBlanket_ = true;
                    conflictBlanket_ = choice;
                }
            } else {
                choice = ConflictChoice::Skip;
            }
            switch (choice) {
            case ConflictChoice::Overwrite: return Resolve::Proceed;
            case ConflictChoice::Skip: ++outcome.skipped; return Resolve::Skip;
            case ConflictChoice::Cancel: return Resolve::Cancel;
            case ConflictChoice::Rename:
                if (renameTo.empty() || renameTo == name) {
                    ++outcome.skipped;
                    return Resolve::Skip;
                }
                name = renameTo;
                continue;  // the new name may collide too
            }
        }
        outcome.failed.emplace_back(itemName, kStrFileInTheWay);
        return Resolve::Fail;
    }

    // One file from `from` to `to` in 64 KB chunks (the generic path).
    ProviderStatus Stream(IFileProvider& from, const std::wstring& fromPath, IFileProvider& to,
                          const std::wstring& toPath, uint64_t size, uint64_t modifiedUtc, std::wstring& err) {
        std::unique_ptr<IReadStream> in;
        ProviderStatus st = from.OpenRead(fromPath, in, Poll(), err);
        if (st != ProviderStatus::Ok) return st;
        std::unique_ptr<IWriteStream> out;
        st = to.OpenWrite(toPath, size, modifiedUtc, out, Poll(), err);
        if (st != ProviderStatus::Ok) return st;
        std::vector<char> buffer(kProviderCopyChunk);
        uint64_t done = 0;
        if (h_.onByteProgress) h_.onByteProgress(0, size);
        while (true) {
            if (Cancelled()) {
                out->Abort();
                return ProviderStatus::Cancelled;
            }
            size_t got = 0;
            st = in->Read(buffer.data(), buffer.size(), got, err);
            if (st != ProviderStatus::Ok) {
                out->Abort();
                return st;
            }
            if (got == 0) break;
            st = out->Write(buffer.data(), got, err);
            if (st != ProviderStatus::Ok) {
                out->Abort();
                return st;
            }
            done += got;
            if (h_.onByteProgress) h_.onByteProgress(done, (std::max)(done, size));
        }
        in.reset();
        return out->Finish(err);
    }

    // The progress callback handed to a whole-file transfer: reports bytes
    // and turns the cancel poll into "stop" (remembered, since the poll
    // itself consumes the key).
    ProviderByteProgress TransferProgress(bool& cancelRequested) {
        return [this, &cancelRequested](uint64_t done, uint64_t total) {
            if (h_.onByteProgress) h_.onByteProgress(done, (std::max)(done, total));
            if (Cancelled()) cancelRequested = true;
            return !cancelRequested;
        };
    }

    ProviderStatus DownloadToLocal(IFileProvider& from, const std::wstring& fromPath, const std::wstring& localTarget,
                                   uint64_t size, uint64_t modifiedUtc, std::wstring& err) {
        bool cancelRequested = false;
        std::wstring part = localTarget + kLocalPartSuffix;
        ProviderStatus st = from.DownloadToFile(fromPath, part, size, TransferProgress(cancelRequested), err);
        if (st != ProviderStatus::Ok) {
            DiscardLocalPart(part);
            return cancelRequested ? ProviderStatus::Cancelled : st;
        }
        return CommitLocalPart(part, localTarget, modifiedUtc, err);
    }

    ProviderStatus UploadFromLocal(const std::wstring& localSource, IFileProvider& to, const std::wstring& toPath,
                                   uint64_t modifiedUtc, std::wstring& err) {
        bool cancelRequested = false;
        ProviderStatus st = to.UploadFromFile(localSource, toPath, modifiedUtc, TransferProgress(cancelRequested), err);
        return (st != ProviderStatus::Ok && cancelRequested) ? ProviderStatus::Cancelled : st;
    }

    // Copies one file, choosing the path: a whole-file transfer when one side
    // is local and the other has CapFileTransfer; staging through a local
    // temporary file between two providers when either has it; else streams.
    ProviderStatus StreamFile(const std::wstring& srcPath, const ProviderEntry& e, const std::wstring& dstPath,
                              std::wstring& err) {
        bool srcFast = (src_.Capabilities() & CapFileTransfer) != 0;
        bool dstFast = (dst_.Capabilities() & CapFileTransfer) != 0;
        if (srcFast && dst_.IsLocal()) return DownloadToLocal(src_, srcPath, dstPath, e.size, e.modifiedUtc, err);
        if (dstFast && src_.IsLocal()) return UploadFromLocal(srcPath, dst_, dstPath, e.modifiedUtc, err);
        if (!srcFast && !dstFast) return Stream(src_, srcPath, dst_, dstPath, e.size, e.modifiedUtc, err);

        std::wstring stage = StagingFile();
        ProviderStatus st = srcFast ? DownloadToLocal(src_, srcPath, stage, e.size, 0, err)
                                    : Stream(src_, srcPath, local_, stage, e.size, 0, err);
        if (st == ProviderStatus::Ok) {
            st = dstFast ? UploadFromLocal(stage, dst_, dstPath, e.modifiedUtc, err)
                         : Stream(local_, stage, dst_, dstPath, e.size, e.modifiedUtc, err);
        }
        DiscardLocalPart(stage);
        return st;
    }

    // Read-only / hidden / system / archive travel with a copied file to any
    // target that can store them (an archive entry's attributes land on the
    // extracted file and back -- ZIP-003). Best effort: never fails a copy.
    void PropagateAttributes(const std::wstring& dstPath, uint32_t attributes) {
        constexpr uint32_t kSettable =
            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
        if (!(dst_.Capabilities() & CapAttributes) || !(attributes & kSettable)) return;
        std::wstring ignored;
        dst_.SetAttributes(dstPath, attributes & kSettable, ignored);
    }

    static std::wstring StagingFile() {
        wchar_t tempDir[MAX_PATH] = {};
        GetTempPathW(MAX_PATH, tempDir);
        static unsigned counter = 0;
        return std::wstring(tempDir) + L"mc-stage-" + std::to_wstring(GetCurrentProcessId()) + L"-" +
               std::to_wstring(++counter) + L".tmp";
    }

    // Copies one node (file or folder tree) to Join(dstDir, targetName).
    // `clean` turns false when anything under it wasn't copied.
    Step CopyNode(const std::wstring& srcPath, const ProviderEntry& e, const std::wstring& dstDir,
                  std::wstring targetName, const std::wstring& itemName, bool& clean) {
        if (Cancelled()) return Step::Cancelled;
        std::wstring dstPath = dst_.Join(dstDir, targetName);
        if (e.isDirectory) {
            if (e.isLink) {
                Note(srcPath + L": a link to a folder is not followed; skipped");
                ++outcome.skipped;
                clean = false;
                return Step::Skipped;
            }
            if (SameProvider() && ProviderPathIsSameOrWithin(src_, srcPath, dstPath)) {
                outcome.failed.emplace_back(itemName, kStrIntoItself);
                clean = false;
                return Step::Failed;
            }
            ProviderEntry existing;
            std::wstring err;
            ProviderStatus st = dst_.Stat(dstPath, existing, Poll(), err);
            if (st == ProviderStatus::Cancelled) return Step::Cancelled;
            if (st == ProviderStatus::Ok && !existing.isDirectory) {
                outcome.failed.emplace_back(itemName, dstPath + L": " + kStrFileInTheWay);
                clean = false;
                return Step::Failed;
            }
            if (st != ProviderStatus::Ok) {
                Step made = Attempt(itemName, [&](std::wstring& e2) { return dst_.MakeDirectory(dstPath, Poll(), e2); });
                if (made != Step::Done) {
                    clean = false;
                    return made;
                }
            }
            std::vector<ProviderEntry> children;
            Step listed = Attempt(itemName, [&](std::wstring& e2) { return src_.List(srcPath, children, Poll(), e2); });
            if (listed != Step::Done) {
                clean = false;
                return listed;
            }
            for (const auto& c : children) {
                Step s = CopyNode(src_.Join(srcPath, c.name), c, dstPath, c.name, itemName, clean);
                if (s == Step::Cancelled) return s;
            }
            return clean ? Step::Done : Step::Failed;
        }

        bool exists = false;
        Resolve r = ResolveFileConflict(dst_, dstDir, targetName, exists, itemName);
        if (r == Resolve::Cancel) return Step::Cancelled;
        if (r != Resolve::Proceed) {
            clean = false;
            return r == Resolve::Skip ? Step::Skipped : Step::Failed;
        }
        dstPath = dst_.Join(dstDir, targetName);
        if (SameProvider() && src_.SamePath(srcPath, dstPath)) {
            outcome.failed.emplace_back(itemName, kStrOntoItself);
            clean = false;
            return Step::Failed;
        }
        Step s = Attempt(itemName, [&](std::wstring& err) { return StreamFile(srcPath, e, dstPath, err); });
        if (s != Step::Done) clean = false;
        else PropagateAttributes(dstPath, e.attributes);
        return s;
    }

    // Deletes one node on `p`; folders children first, links never followed.
    Step DeleteNode(IFileProvider& p, const std::wstring& path, const ProviderEntry& e, const std::wstring& itemName,
                    bool& clean) {
        if (Cancelled()) return Step::Cancelled;
        if (e.isDirectory && !e.isLink) {
            std::vector<ProviderEntry> children;
            Step listed = Attempt(itemName, [&](std::wstring& err) { return p.List(path, children, Poll(), err); });
            if (listed != Step::Done) {
                clean = false;
                return listed;
            }
            for (const auto& c : children) {
                Step s = DeleteNode(p, p.Join(path, c.name), c, itemName, clean);
                if (s == Step::Cancelled) return s;
            }
            if (!clean) return Step::Failed;
        }
        Step s = Attempt(itemName, [&](std::wstring& err) { return p.Delete(path, e.isDirectory && !e.isLink, Poll(), err); });
        if (s != Step::Done) clean = false;
        return s;
    }

    Step StatItem(IFileProvider& p, const std::wstring& path, ProviderEntry& e, const std::wstring& itemName) {
        return Attempt(itemName, [&](std::wstring& err) { return p.Stat(path, e, Poll(), err); });
    }

private:
    IFileProvider& src_;
    IFileProvider& dst_;
    const ProviderOpHandlers& h_;
    LocalProvider local_;  // the staging side between two providers
    bool errorBlanket_ = false;
    bool haveConflictBlanket_ = false;
    ConflictChoice conflictBlanket_ = ConflictChoice::Skip;

    void ReconnectAll() {
        std::wstring ignored;
        src_.Reconnect(Poll(), ignored);
        if (!SameProvider()) dst_.Reconnect(Poll(), ignored);
    }
};

OperationOutcome CopyImpl(IFileProvider& source, const std::wstring& sourceDir,
                          const std::vector<std::pair<std::wstring, std::wstring>>& items, IFileProvider& target,
                          const std::wstring& targetDir, const ProviderOpHandlers& handlers) {
    Engine engine(source, target, handlers);
    int total = static_cast<int>(items.size());
    for (int i = 0; i < total; ++i) {
        const std::wstring& name = items[i].first;
        if (!engine.StartItem(name, i, total)) break;
        std::wstring srcPath = source.Join(sourceDir, name);
        ProviderEntry e;
        Step s = engine.StatItem(source, srcPath, e, name);
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s != Step::Done) continue;
        bool clean = true;
        s = engine.CopyNode(srcPath, e, targetDir, items[i].second, name, clean);
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s == Step::Done) ++engine.outcome.succeeded;
    }
    return engine.outcome;
}

}  // namespace

OperationOutcome ProviderCopyItems(IFileProvider& source, const std::wstring& sourceDir,
                                   const std::vector<std::wstring>& items, IFileProvider& target,
                                   const std::wstring& targetDir, const ProviderOpHandlers& handlers) {
    std::vector<std::pair<std::wstring, std::wstring>> pairs;
    for (const auto& n : items) pairs.emplace_back(n, n);
    return CopyImpl(source, sourceDir, pairs, target, targetDir, handlers);
}

OperationOutcome ProviderCopyItemAs(IFileProvider& source, const std::wstring& sourceDir, const std::wstring& item,
                                    IFileProvider& target, const std::wstring& targetDir, const std::wstring& newName,
                                    const ProviderOpHandlers& handlers) {
    return CopyImpl(source, sourceDir, {{item, newName}}, target, targetDir, handlers);
}

OperationOutcome ProviderMoveItems(IFileProvider& source, const std::wstring& sourceDir,
                                   const std::vector<std::wstring>& items, IFileProvider& target,
                                   const std::wstring& targetDir, const ProviderOpHandlers& handlers) {
    Engine engine(source, target, handlers);
    bool canRename = engine.SameProvider() && (source.Capabilities() & CapRename);
    int total = static_cast<int>(items.size());
    for (int i = 0; i < total; ++i) {
        const std::wstring& name = items[i];
        if (!engine.StartItem(name, i, total)) break;
        std::wstring srcPath = source.Join(sourceDir, name);
        ProviderEntry e;
        Step s = engine.StatItem(source, srcPath, e, name);
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s != Step::Done) continue;
        std::wstring dstPath = target.Join(targetDir, name);

        if (engine.SameProvider()) {
            if (source.SamePath(srcPath, dstPath)) {
                engine.outcome.failed.emplace_back(name, L"source and destination are the same");
                continue;
            }
            if (e.isDirectory && !e.isLink && ProviderPathIsSameOrWithin(source, srcPath, dstPath)) {
                engine.outcome.failed.emplace_back(name, kStrIntoItself);
                continue;
            }
        }

        if (canRename) {
            ProviderEntry existing;
            std::wstring err;
            ProviderStatus st = target.Stat(dstPath, existing, handlers.cancelPoll, err);
            if (st == ProviderStatus::Cancelled) {
                engine.outcome.cancelled = true;
                break;
            }
            bool targetFree = st != ProviderStatus::Ok;
            bool fileOntoFile = st == ProviderStatus::Ok && !existing.isDirectory && !e.isDirectory;
            if (targetFree || fileOntoFile) {
                std::wstring finalName = name;
                bool exists = false;
                if (fileOntoFile) {
                    auto r = engine.ResolveFileConflict(target, targetDir, finalName, exists, name);
                    if (r == Engine::Resolve::Cancel) {
                        engine.outcome.cancelled = true;
                        break;
                    }
                    if (r != Engine::Resolve::Proceed) continue;
                }
                std::wstring finalPath = target.Join(targetDir, finalName);
                s = engine.Attempt(name, [&](std::wstring& e2) {
                    if (exists) {
                        ProviderStatus d = target.Delete(finalPath, false, handlers.cancelPoll, e2);
                        if (d != ProviderStatus::Ok && d != ProviderStatus::NotFound) return d;
                        exists = false;
                    }
                    return source.Rename(srcPath, finalPath, handlers.cancelPoll, e2);
                });
                if (s == Step::Cancelled) {
                    engine.outcome.cancelled = true;
                    break;
                }
                if (s == Step::Done) ++engine.outcome.succeeded;
                continue;
            }
            // A folder onto an existing folder: merge by copy, then delete.
        }

        bool clean = true;
        s = engine.CopyNode(srcPath, e, targetDir, name, name, clean);
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s != Step::Done || !clean) continue;
        bool deleted = true;
        s = engine.DeleteNode(source, srcPath, e, name, deleted);
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s == Step::Done) ++engine.outcome.succeeded;
    }
    return engine.outcome;
}

ProviderCopyAsTarget ResolveProviderCopyAs(IFileProvider& provider, const std::wstring& currentDir,
                                           const std::wstring& itemName, const std::wstring& typed) {
    ProviderCopyAsTarget result;
    std::wstring text = typed;
    size_t first = text.find_first_not_of(L" \t"), last = text.find_last_not_of(L" \t");
    text = first == std::wstring::npos ? L"" : text.substr(first, last - first + 1);
    if (text.size() >= 2 && text.front() == L'"' && text.back() == L'"') text = text.substr(1, text.size() - 2);
    std::replace(text.begin(), text.end(), L'\\', L'/');
    bool into = false;
    while (!text.empty() && text.back() == L'/') {
        text.pop_back();
        into = true;
    }
    if (text.empty()) {
        result.problem = CopyAsProblem::Empty;
        return result;
    }
    std::vector<std::wstring> segments;
    for (size_t pos = 0;;) {
        size_t slash = text.find(L'/', pos);
        segments.push_back(text.substr(pos, slash == std::wstring::npos ? std::wstring::npos : slash - pos));
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }
    for (const auto& s : segments) {
        if (s.empty() || s == L"." || s == L".." || s.find(L':') != std::wstring::npos) {
            result.problem = CopyAsProblem::Unsafe;
            return result;
        }
    }
    std::wstring targetDir = currentDir;
    for (size_t i = 0; i + 1 < segments.size(); ++i) targetDir = provider.Join(targetDir, segments[i]);
    std::wstring newName = segments.back();
    ProviderEntry existing;
    std::wstring ignored;
    if (!into && provider.Stat(provider.Join(targetDir, newName), existing, {}, ignored) == ProviderStatus::Ok &&
        existing.isDirectory && !provider.SamePath(provider.Join(targetDir, newName), provider.Join(currentDir, itemName)))
        into = true;
    if (into) {
        targetDir = provider.Join(targetDir, newName);
        newName = itemName;
    }
    std::wstring source = provider.Join(currentDir, itemName);
    std::wstring target = provider.Join(targetDir, newName);
    if (provider.SamePath(target, source)) {
        result.problem = CopyAsProblem::SameItem;
        return result;
    }
    ProviderEntry item;
    if (provider.Stat(source, item, {}, ignored) == ProviderStatus::Ok && item.isDirectory && !item.isLink &&
        ProviderPathIsSameOrWithin(provider, source, targetDir)) {
        result.problem = CopyAsProblem::IntoItself;
        return result;
    }
    result.targetDir = targetDir;
    result.newName = newName;
    return result;
}

OperationOutcome ProviderToggleAttributes(IFileProvider& provider, const std::wstring& dir,
                                          const std::vector<std::wstring>& items, uint32_t attributeBit,
                                          const ProviderOpHandlers& handlers) {
    Engine engine(provider, provider, handlers);
    int total = static_cast<int>(items.size());
    for (int i = 0; i < total; ++i) {
        const std::wstring& name = items[i];
        if (!engine.StartItem(name, i, total)) break;
        std::wstring path = provider.Join(dir, name);
        Step s = engine.Attempt(name, [&](std::wstring& err) {
            ProviderEntry e;
            ProviderStatus st = provider.Stat(path, e, handlers.cancelPoll, err);
            if (st != ProviderStatus::Ok) return st;
            return provider.SetAttributes(path, e.attributes ^ attributeBit, err);
        });
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s == Step::Done) ++engine.outcome.succeeded;
    }
    return engine.outcome;
}

OperationOutcome ProviderDeleteItems(IFileProvider& provider, const std::wstring& dir,
                                     const std::vector<std::wstring>& items, const ProviderOpHandlers& handlers) {
    Engine engine(provider, provider, handlers);
    int total = static_cast<int>(items.size());
    for (int i = 0; i < total; ++i) {
        const std::wstring& name = items[i];
        if (!engine.StartItem(name, i, total)) break;
        std::wstring path = provider.Join(dir, name);
        ProviderEntry e;
        Step s = engine.StatItem(provider, path, e, name);
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s != Step::Done) continue;
        bool clean = true;
        s = engine.DeleteNode(provider, path, e, name, clean);
        if (s == Step::Cancelled) {
            engine.outcome.cancelled = true;
            break;
        }
        if (s == Step::Done) ++engine.outcome.succeeded;
    }
    return engine.outcome;
}

}  // namespace mc
