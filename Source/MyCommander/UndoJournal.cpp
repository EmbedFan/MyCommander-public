#include "UndoJournal.h"

#include "FileOps.h"
#include "PathUtil.h"

#include <windows.h>

#include <algorithm>

namespace fs = std::filesystem;

namespace mc {

namespace {

constexpr uint32_t kSettableAttributes =
    FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;

uint64_t Ticks(const FILETIME& ft) { return (static_cast<uint64_t>(ft.dwHighDateTime) << 32) | ft.dwLowDateTime; }

void Accumulate(const fs::path& path, PathFingerprint& out, int depth) {
    WIN32_FILE_ATTRIBUTE_DATA data{};
    if (!GetFileAttributesExW(ToLongPath(path).c_str(), GetFileExInfoStandard, &data)) return;
    const bool isDir = (data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
    const bool isLink = (data.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) != 0;
    out.newestWrite = (std::max)(out.newestWrite, Ticks(data.ftLastWriteTime));
    if (!isDir) {
        ++out.files;
        out.bytes += (static_cast<uint64_t>(data.nFileSizeHigh) << 32) | data.nFileSizeLow;
        return;
    }
    if (isLink || depth > 64) return;
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((ToLongPath(path) / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr,
                                FIND_FIRST_EX_LARGE_FETCH);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        std::wstring name = fd.cFileName;
        if (name == L"." || name == L"..") continue;
        Accumulate(path / name, out, depth + 1);
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

bool IsEmptyDirectory(const fs::path& path) {
    WIN32_FIND_DATAW fd{};
    HANDLE h = FindFirstFileExW((ToLongPath(path) / L"*").c_str(), FindExInfoBasic, &fd, FindExSearchNameMatch, nullptr, 0);
    if (h == INVALID_HANDLE_VALUE) return GetLastError() == ERROR_FILE_NOT_FOUND;
    bool empty = true;
    do {
        std::wstring name = fd.cFileName;
        if (name != L"." && name != L"..") {
            empty = false;
            break;
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
    return empty;
}

std::wstring Changed(const fs::path& path) { return L"\"" + path.filename().wstring() + L"\" has changed since"; }

}  // namespace

PathFingerprint FingerprintOf(const fs::path& path) {
    PathFingerprint f;
    DWORD attrs = GetAttributesOrInvalid(path);
    if (attrs == INVALID_FILE_ATTRIBUTES) return f;
    f.exists = true;
    f.attributes = attrs;
    f.isDirectory = (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0;
    Accumulate(path, f, 0);
    return f;
}

void UndoJournal::Push(UndoEntry entry) {
    entries_.push_back(std::move(entry));
    if (entries_.size() > kCapacity) entries_.erase(entries_.begin());
}

UndoItem MakeMovedItem(const fs::path& before, const fs::path& after) {
    UndoItem item;
    item.before = before;
    item.after = after;
    item.expected = FingerprintOf(after);
    return item;
}

UndoItem MakeCreatedItem(const fs::path& source, const fs::path& created) {
    UndoItem item;
    item.before = source;
    item.after = created;
    item.expected = FingerprintOf(created);
    return item;
}

UndoItem MakeCreatedItem(const fs::path& created) { return MakeCreatedItem(created, created); }

UndoItem MakeAttributeItem(const fs::path& path, uint32_t attributesBefore) {
    UndoItem item;
    item.before = path;
    item.after = path;
    item.attributesBefore = attributesBefore;
    item.expected = FingerprintOf(path);
    return item;
}

UndoItem MakeDeletedItem(const fs::path& original, const PathFingerprint& before) {
    UndoItem item;
    item.before = original;
    item.after = original;
    item.expected = before;
    return item;
}

UndoCheck CheckUndo(const UndoEntry& entry) {
    UndoCheck result;
    if (entry.items.empty()) {
        result.problem = L"Nothing is recorded for this operation.";
        return result;
    }
    for (const UndoItem& item : entry.items) {
        PathFingerprint now = FingerprintOf(item.after);
        switch (entry.kind) {
            case UndoKind::Delete:
                if (now.exists) {
                    result.problem = L"\"" + item.after.filename().wstring() + L"\" exists again at the original place.";
                    return result;
                }
                if (!FingerprintOf(item.after.parent_path()).isDirectory) {
                    result.problem = L"The original folder is gone: " + item.after.parent_path().wstring();
                    return result;
                }
                break;
            case UndoKind::Rename:
            case UndoKind::Move: {
                if (!(now == item.expected)) {
                    result.problem = Changed(item.after) + L" the operation, so it can't be undone.";
                    return result;
                }
                if (FingerprintOf(item.before).exists) {
                    result.problem = L"\"" + item.before.filename().wstring() + L"\" exists again at the original place.";
                    return result;
                }
                if (!FingerprintOf(item.before.parent_path()).isDirectory) {
                    result.problem = L"The original folder is gone: " + item.before.parent_path().wstring();
                    return result;
                }
                break;
            }
            case UndoKind::Copy:
                if (!(now == item.expected)) {
                    result.problem = Changed(item.after) + L" it was copied (edited, moved or removed).";
                    return result;
                }
                break;
            case UndoKind::Attributes:
                if (!now.exists || now.attributes != item.expected.attributes) {
                    result.problem = Changed(item.after) + L" its attributes were set.";
                    return result;
                }
                break;
            case UndoKind::NewFolder:
                if (!now.exists || !now.isDirectory || !IsEmptyDirectory(item.after)) {
                    result.problem = L"The new folder \"" + item.after.filename().wstring() +
                                     L"\" is gone or no longer empty, so it is left alone.";
                    return result;
                }
                break;
            case UndoKind::NewFile:
                if (!now.exists || now.isDirectory || now.bytes != 0) {
                    result.problem = L"The new file \"" + item.after.filename().wstring() +
                                     L"\" is gone or no longer empty, so it is left alone.";
                    return result;
                }
                break;
        }
    }
    result.ok = true;
    return result;
}

UndoResult PerformUndo(const UndoEntry& entry, const UndoActions& actions) {
    UndoResult result;
    UndoCheck check = CheckUndo(entry);
    if (!check.ok) {
        result.error = check.problem;
        return result;
    }
    if (entry.kind == UndoKind::Delete) {  // verify every item first: no partial restore
        for (const UndoItem& item : entry.items) {
            std::wstring problem;
            if (!actions.locate || !actions.locate(item.before, entry.startedUtc, item.expected, problem)) {
                result.error = L"\"" + item.before.filename().wstring() + L"\": " +
                               (problem.empty() ? std::wstring(L"not found in the Recycle Bin.") : problem);
                return result;
            }
        }
    }
    for (const UndoItem& item : entry.items) {
        std::wstring error;
        bool ok = false;
        switch (entry.kind) {
            case UndoKind::Delete:
                if (!actions.restore) {
                    error = L"No Recycle Bin available.";
                } else {
                    ok = actions.restore(item.before, entry.startedUtc, item.expected, error);
                }
                break;
            case UndoKind::Rename: {
                RenameResult r = RenameItem(item.after.parent_path(), item.after.filename().wstring(),
                                            item.before.filename().wstring());
                ok = r.ok;
                error = r.error;
                break;
            }
            case UndoKind::Move: {
                auto refuse = [](const fs::path&) {
                    ConflictAskResult r;
                    r.choice = ConflictChoice::Cancel;
                    return r;
                };
                OperationOutcome o = MoveItems(item.after.parent_path(), {item.after.filename().wstring()},
                                               item.before.parent_path(), refuse, nullptr, nullptr);
                ok = o.succeeded == 1 && o.failed.empty() && !o.cancelled;
                if (!o.failed.empty()) error = o.failed.front().second;
                break;
            }
            case UndoKind::Copy:
                if (!actions.recycle) {
                    error = L"No Recycle Bin available.";
                } else {
                    ok = actions.recycle(item.after, error);
                }
                break;
            case UndoKind::Attributes: {
                DWORD current = GetAttributesOrInvalid(item.after);
                if (current == INVALID_FILE_ATTRIBUTES) {
                    error = L"Cannot read the attributes.";
                    break;
                }
                uint32_t bits = (current & ~kSettableAttributes) | (item.attributesBefore & kSettableAttributes);
                ok = SetFileAttributesW(ToLongPath(item.after).c_str(), bits ? bits : FILE_ATTRIBUTE_NORMAL) != 0;
                if (!ok) error = FormatWinError(GetLastError());
                break;
            }
            case UndoKind::NewFolder:
                ok = RemoveDirectoryW(ToLongPath(item.after).c_str()) != 0;
                if (!ok) error = FormatWinError(GetLastError());
                break;
            case UndoKind::NewFile:
                ok = DeleteFileW(ToLongPath(item.after).c_str()) != 0;
                if (!ok) error = FormatWinError(GetLastError());
                break;
        }
        if (!ok) {
            result.error = L"\"" + item.after.filename().wstring() + L"\": " + error;
            return result;
        }
        ++result.reversed;
    }
    result.ok = true;
    return result;
}

}  // namespace mc
