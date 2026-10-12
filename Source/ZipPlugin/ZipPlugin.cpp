// FEA-0007: the ZIP MC Plugin.
//
// Phase 1 (§5): "Pack file" -- replaces MenuBar.h's removed "Pack Files"
// stub, shows a multi-field options dialog (compression level, recurse-into-
// subdirectories, a size cap, multi-archive splitting), then packs the
// active panel's current selection into a new .zip in the other panel's
// directory, preserving each entry's own Win32 attributes.
//
// Phase 2 (§6): "Unpack Package" -- replaces MenuBar.h's removed "Unpack
// Package" stub, extracts every selected .zip file's full contents into the
// other panel's directory (one subfolder per archive when more than one is
// selected at once, matching how mainstream archive tools already batch-
// extract), restoring each entry's stored Win32 attributes.
//
// See AIPrompt/features/FEA-0007-plan.md for the full design this
// implements, including the documented, deliberate simplifications:
// "without subdirectories" (Pack) flattens a selected folder's own direct
// files into the archive root rather than nesting them (so two same-named
// files from different selected folders can collide -- the later one is
// skipped, logged as a warning, not an interactive per-conflict prompt,
// the same non-interactive-batch-operation convention Unpack's own
// destination-collision handling reuses); the size cap is enforced as an
// after-the-fact notice when splitting is off (an accurate *before*-the-fact
// prediction of compressed size isn't practical to compute ahead of
// actually compressing); and multi-archive splitting bins entries by their
// own *uncompressed* size (an approximation, not a guaranteed
// post-compression size) into independent, separately-openable .zip files
// (name.1.zip, name.2.zip, ...), not true PKZIP-spec multi-volume spanning.
//
// Phase 1/2 declare only kMcPluginCapMenu -- kMcPluginCapArchiveHandler
// (claiming a .zip file as a virtual directory, FEA-0007 §2) and
// kMcPluginCapPanel (live archive browsing, §7) are Phase 3 concerns, not
// yet implemented; declaring them now with no matching exports would be a
// load-time warning for nothing gained, so unlike the plan's own "declare
// both from the start" suggestion, this implementation adds each
// capability only when the phase that actually uses it lands.

#include "../MyCommanderPluginSdk/McPluginAbi.h"
#include "../MyCommanderPluginSdk/McPluginHost.h"
#include "../MyCommanderPluginSdk/McPluginTypes.h"
#include "Version.h"
#include "../MyCommander/TextWidth.h"

#include "third_party/miniz/miniz.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <numeric>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

const McPluginHost* g_host = nullptr;
class ArchiveProgress {
    bool open_ = false;
public:
    ArchiveProgress(const wchar_t* title, uint64_t total, bool enabled = true) {
        if (enabled && g_host && g_host->BeginProgress && g_host->UpdateProgress && g_host->EndProgress)
            open_ = g_host->BeginProgress(title, total);
    }
    ~ArchiveProgress() { if (open_) g_host->EndProgress(); }
    ArchiveProgress(const ArchiveProgress&) = delete;
    ArchiveProgress& operator=(const ArchiveProgress&) = delete;
    void Update(uint64_t done, uint64_t total, const std::wstring& message) const {
        if (open_) g_host->UpdateProgress(done, total, message.c_str());
    }
};
constexpr uint32_t kPackCommandId = 1;
constexpr uint32_t kUnpackCommandId = 2;
constexpr uint32_t kArchiveInfoCommandId = 3;  // IS-0011

// The ZIP central directory file header's external-attributes field sits
// at this fixed, spec-defined byte offset (PKZIP APPNOTE.TXT's own Central
// Directory Structure) -- stable across every ZIP reader/writer for
// backward format compatibility, not an implementation detail of any one
// library. miniz's own public writer API (every mz_zip_writer_add_* variant,
// confirmed by reading miniz.c's source) never exposes a way to set this
// directly; it's always written as 0. Patched here, after the fact, on the
// in-memory archive buffer miniz itself produced -- verified against a
// standalone round-trip through miniz's own reader before this file was
// written (see the session history for FEA-0007's implementation).
constexpr size_t kCdhExternalAttrOfs = 38;
constexpr uint32_t kCentralDirHeaderSig = 0x02014b50u;
constexpr uint32_t kEndOfCentralDirSig = 0x06054b50u;

std::wstring JoinPath(const std::wstring& dir, const std::wstring& name) {
    if (dir.empty()) return name;
    wchar_t last = dir.back();
    return (last == L'\\' || last == L'/') ? dir + name : dir + L"\\" + name;
}

// Falls back to a generic name when `dir` has no filename component of its
// own (a bare drive root, e.g. "C:\") -- std::filesystem is deliberately
// not used here (this plugin links no C++ standard filesystem dependency),
// just plain wstring slicing.
std::wstring FolderNameOf(const std::wstring& dir) {
    std::wstring trimmed = dir;
    while (!trimmed.empty() && (trimmed.back() == L'\\' || trimmed.back() == L'/')) trimmed.pop_back();
    size_t slash = trimmed.find_last_of(L"\\/");
    std::wstring name = (slash == std::wstring::npos) ? trimmed : trimmed.substr(slash + 1);
    if (name.empty() || name.back() == L':') return L"Archive";
    return name;
}

void LogInfo(const std::wstring& message) {
    if (g_host) g_host->Log(kMcLogInfo, message.c_str());
}
void LogWarning(const std::wstring& message) {
    if (g_host) g_host->Log(kMcLogWarning, message.c_str());
}
void LogError(const std::wstring& message) {
    if (g_host) g_host->Log(kMcLogError, message.c_str());
}

std::vector<std::wstring> GetSelectedNamesList() {
    int count = g_host->GetSelectedNames(nullptr, 0, 0);
    if (count <= 0) return {};
    std::vector<std::wstring> buffers(static_cast<size_t>(count), std::wstring(1024, L'\0'));
    std::vector<wchar_t*> ptrs(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) ptrs[static_cast<size_t>(i)] = buffers[static_cast<size_t>(i)].data();
    int actual = g_host->GetSelectedNames(ptrs.data(), count, 1024);
    std::vector<std::wstring> result;
    int n = std::min(actual, count);
    for (int i = 0; i < n; ++i) result.emplace_back(ptrs[static_cast<size_t>(i)]);
    return result;
}

struct PackEntry {
    std::wstring sourcePath;   // real filesystem path to read bytes from
    std::wstring archiveName;  // forward-slash path inside the zip
    uint32_t attributes = 0;   // Win32 FILE_ATTRIBUTE_*
    uint64_t sizeBytes = 0;
};

// "Without subdirectories": only the direct files inside `dir` (one level,
// no further recursion), flattened -- archiveName is just the filename, not
// prefixed with `dir`'s own name.
void CollectDirectFiles(const std::wstring& dir, std::vector<PackEntry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) continue;  // one level only, no recursion
        PackEntry entry;
        entry.sourcePath = JoinPath(dir, fd.cFileName);
        entry.archiveName = fd.cFileName;
        entry.attributes = fd.dwFileAttributes;
        entry.sizeBytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
        out.push_back(std::move(entry));
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

// "With subdirectories": full recursion, archiveName keeps the directory
// structure under `archivePrefix`. Never descends into a reparse point
// (SEC-006's own guard elsewhere in this project, reused here against the
// same self-referential-link risk).
void CollectRecursive(const std::wstring& dir, const std::wstring& archivePrefix, std::vector<PackEntry>& out) {
    WIN32_FIND_DATAW fd;
    HANDLE h = FindFirstFileW(JoinPath(dir, L"*").c_str(), &fd);
    if (h == INVALID_HANDLE_VALUE) return;
    do {
        if (wcscmp(fd.cFileName, L".") == 0 || wcscmp(fd.cFileName, L"..") == 0) continue;
        std::wstring childReal = JoinPath(dir, fd.cFileName);
        std::wstring childArchive = archivePrefix.empty() ? fd.cFileName : archivePrefix + L"/" + fd.cFileName;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) continue;
        if (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
            CollectRecursive(childReal, childArchive, out);
        } else {
            PackEntry entry;
            entry.sourcePath = childReal;
            entry.archiveName = childArchive;
            entry.attributes = fd.dwFileAttributes;
            entry.sizeBytes = (static_cast<uint64_t>(fd.nFileSizeHigh) << 32) | fd.nFileSizeLow;
            out.push_back(std::move(entry));
        }
    } while (FindNextFileW(h, &fd));
    FindClose(h);
}

std::vector<uint8_t> ReadFileBytes(const std::wstring& path) {
    std::vector<uint8_t> data;
    HANDLE h = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                          FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (h == INVALID_HANDLE_VALUE) return data;
    LARGE_INTEGER size{};
    if (GetFileSizeEx(h, &size) && size.QuadPart > 0 && size.QuadPart <= 0xFFFFFFFFLL) {
        data.resize(static_cast<size_t>(size.QuadPart));
        DWORD bytesRead = 0;
        if (!ReadFile(h, data.data(), static_cast<DWORD>(data.size()), &bytesRead, nullptr)) {
            data.clear();
        } else {
            data.resize(bytesRead);
        }
    }
    CloseHandle(h);
    return data;
}

bool WriteFileBytes(const std::wstring& path, const std::vector<uint8_t>& data) {
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return false;
    BOOL ok = TRUE;
    DWORD written = 0;
    if (!data.empty()) ok = WriteFile(h, data.data(), static_cast<DWORD>(data.size()), &written, nullptr);
    CloseHandle(h);
    return ok && (data.empty() || written == data.size());
}

// In-archive entry names are UTF-8 (the modern, widely-supported ZIP
// convention) with forward slashes -- pure string data the OS never
// interprets as a real path, so this is unrelated to (and doesn't reinstate)
// any ANSI-codepage path risk.
std::string Utf8ArchiveName(const std::wstring& name) {
    std::wstring normalized = name;
    std::replace(normalized.begin(), normalized.end(), L'\\', L'/');
    int len = WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(), -1, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return std::string();
    std::string result(static_cast<size_t>(len - 1), '\0');
    WideCharToMultiByte(CP_UTF8, 0, normalized.c_str(), -1, result.data(), len, nullptr, nullptr);
    return result;
}

// The inverse of Utf8ArchiveName -- decodes an in-archive entry name (UTF-8,
// forward slashes) back to a wstring, still with forward slashes (the
// caller converts to backslashes for a real filesystem path).
std::wstring WideFromUtf8(const char* utf8) {
    if (!utf8 || !*utf8) return std::wstring();
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return std::wstring();
    std::wstring result(static_cast<size_t>(len - 1), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, result.data(), len);
    return result;
}

// Zip-slip guard: an archive (corrupted, or deliberately malicious) could
// name an entry "../../evil.exe" or "C:\evil.exe" to escape the chosen
// destination directory. Rejects any entry whose normalized, forward-slash
// name contains a ".." path segment, starts with a slash, or contains a
// drive-letter colon -- simple and self-contained (this plugin has no
// access to the host's own PathUtil.h subtree-containment helpers, which
// are host-internal, not part of the SDK a real third-party plugin author
// would ever see).
bool IsSafeArchiveEntryName(const std::wstring& name) {
    if (name.empty() || name.front() == L'/' || name.find(L':') != std::wstring::npos) return false;
    size_t start = 0;
    while (start <= name.size()) {
        size_t slash = name.find(L'/', start);
        std::wstring segment = name.substr(start, slash == std::wstring::npos ? std::wstring::npos : slash - start);
        if (segment == L"..") return false;
        if (slash == std::wstring::npos) break;
        start = slash + 1;
    }
    return true;
}

// Creates `dir` and every missing parent directory above it (CreateDirectoryW
// only ever creates one level at a time).
void EnsureDirectoryExists(const std::wstring& dir) {
    if (dir.empty() || GetFileAttributesW(dir.c_str()) != INVALID_FILE_ATTRIBUTES) return;
    size_t slash = dir.find_last_of(L"\\/");
    if (slash != std::wstring::npos) EnsureDirectoryExists(dir.substr(0, slash));
    CreateDirectoryW(dir.c_str(), nullptr);
}

// See this file's own top comment and kCdhExternalAttrOfs's doc comment for
// why this exists. Walks the central directory in the same order entries
// were added (the order miniz itself preserves) and overwrites each
// record's external-attributes field in place.
bool PatchExternalAttributes(std::vector<uint8_t>& zipData, const std::vector<uint32_t>& attributesInOrder) {
    if (zipData.size() < 22) return false;
    const uint8_t* eocd = zipData.data() + zipData.size() - 22;
    uint32_t eocdSig = eocd[0] | (static_cast<uint32_t>(eocd[1]) << 8) | (static_cast<uint32_t>(eocd[2]) << 16) |
                       (static_cast<uint32_t>(eocd[3]) << 24);
    if (eocdSig != kEndOfCentralDirSig) return false;

    uint16_t numEntries = static_cast<uint16_t>(eocd[10] | (eocd[11] << 8));
    uint32_t cdOfs = eocd[16] | (static_cast<uint32_t>(eocd[17]) << 8) | (static_cast<uint32_t>(eocd[18]) << 16) |
                     (static_cast<uint32_t>(eocd[19]) << 24);
    if (numEntries != attributesInOrder.size()) return false;

    size_t pos = cdOfs;
    for (size_t i = 0; i < numEntries; ++i) {
        if (pos + 46 > zipData.size()) return false;
        uint8_t* hdr = zipData.data() + pos;
        uint32_t hdrSig = hdr[0] | (static_cast<uint32_t>(hdr[1]) << 8) | (static_cast<uint32_t>(hdr[2]) << 16) |
                          (static_cast<uint32_t>(hdr[3]) << 24);
        if (hdrSig != kCentralDirHeaderSig) return false;

        uint32_t attrs = attributesInOrder[i];
        hdr[kCdhExternalAttrOfs + 0] = static_cast<uint8_t>(attrs);
        hdr[kCdhExternalAttrOfs + 1] = static_cast<uint8_t>(attrs >> 8);
        hdr[kCdhExternalAttrOfs + 2] = static_cast<uint8_t>(attrs >> 16);
        hdr[kCdhExternalAttrOfs + 3] = static_cast<uint8_t>(attrs >> 24);

        uint16_t nameLen = static_cast<uint16_t>(hdr[28] | (hdr[29] << 8));
        uint16_t extraLen = static_cast<uint16_t>(hdr[30] | (hdr[31] << 8));
        uint16_t commentLen = static_cast<uint16_t>(hdr[32] | (hdr[33] << 8));
        pos += 46 + nameLen + extraLen + commentLen;
    }
    return true;
}

// Packs `entries` into one complete, independently-valid archive at
// `outputPath`. The whole archive is built in memory (mz_zip_writer_init_heap)
// rather than through any of miniz's own char*-based file-writing functions,
// specifically so every real filesystem touch in this plugin goes through
// Win32's wide-string APIs (ReadFileBytes/WriteFileBytes above) -- miniz's
// own add_file/init_file take a narrow `const char*` path interpreted via
// the ANSI codepage, which would silently mishandle a Unicode filename this
// app must support (PathUtil.h's own "treat paths as data" discipline).
bool WriteZipArchive(const std::vector<PackEntry>& entries, const std::wstring& outputPath, mz_uint levelAndFlags,
                      ArchiveProgress& progress, uint64_t& processed, uint64_t total, bool& cancelled) {
    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) {
        LogError(L"Pack file: could not initialize archive writer.");
        return false;
    }

    std::vector<uint32_t> attrsInOrder;
    bool ok = true;
    for (const auto& entry : entries) {
        // IS-0010: packing had no progress/cancel at all before this --
        // same CancelPoll-shaped check as ExtractOrMove above.
        if (g_host->PollCancel && g_host->PollCancel()) { cancelled = true; ok = false; break; }
        progress.Update(processed++, total, entry.archiveName);
        std::vector<uint8_t> bytes = ReadFileBytes(entry.sourcePath);
        std::string archiveNameUtf8 = Utf8ArchiveName(entry.archiveName);
        const void* bufPtr = bytes.empty() ? static_cast<const void*>("") : static_cast<const void*>(bytes.data());
        if (!mz_zip_writer_add_mem(&zip, archiveNameUtf8.c_str(), bufPtr, bytes.size(), levelAndFlags)) {
            LogError(L"Pack file: failed adding \"" + entry.archiveName + L"\" to the archive.");
            ok = false;
            break;
        }
        attrsInOrder.push_back(entry.attributes);
    }

    void* heapBuf = nullptr;
    size_t heapSize = 0;
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&zip, &heapBuf, &heapSize) != 0;
    mz_zip_writer_end(&zip);
    if (!ok) {
        if (heapBuf) free(heapBuf);
        return false;
    }

    std::vector<uint8_t> zipBytes(static_cast<uint8_t*>(heapBuf), static_cast<uint8_t*>(heapBuf) + heapSize);
    free(heapBuf);

    if (!PatchExternalAttributes(zipBytes, attrsInOrder)) {
        LogWarning(L"Pack file: could not patch stored attributes for \"" + outputPath +
                  L"\" -- the archive is still valid, just without preserved attributes.");
    }
    if (!WriteFileBytes(outputPath, zipBytes)) {
        LogError(L"Pack file: could not write \"" + outputPath + L"\".");
        return false;
    }
    return true;
}

mz_uint CompressionLevelFromChoice(int choiceIndex) {
    switch (choiceIndex) {
        case 0: return MZ_NO_COMPRESSION;  // Store
        case 1: return 3;                  // Fast
        case 3: return MZ_BEST_COMPRESSION;  // Best
        case 2:
        default: return MZ_DEFAULT_LEVEL;  // Normal
    }
}

std::wstring InsertBinSuffix(const std::wstring& path, int binNumber) {
    size_t dot = path.find_last_of(L'.');
    size_t slash = path.find_last_of(L"\\/");
    std::wstring suffix = L"." + std::to_wstring(binNumber);
    if (dot != std::wstring::npos && (slash == std::wstring::npos || dot > slash)) {
        return path.substr(0, dot) + suffix + path.substr(dot);
    }
    return path + suffix;
}

void PackSelection() {
    if (!g_host) return;

    const wchar_t* compressionChoices[] = {L"Store", L"Fast", L"Normal", L"Best"};
    McFormField fields[4] = {};

    fields[0].structSize = sizeof(McFormField);
    fields[0].kind = kMcFormFieldChoice;
    fields[0].label = L"Compression";
    fields[0].choices = compressionChoices;
    fields[0].choiceCount = 4;
    fields[0].choiceIndex = 2;  // Normal

    fields[1].structSize = sizeof(McFormField);
    fields[1].kind = kMcFormFieldCheckbox;
    fields[1].label = L"Include subdirectories";
    fields[1].boolValue = true;

    fields[2].structSize = sizeof(McFormField);
    fields[2].kind = kMcFormFieldNumber;
    fields[2].label = L"Max archive size (MB), 0 = unlimited";
    fields[2].numberValue = 0;
    fields[2].numberMin = 0;
    fields[2].numberMax = 1000000;

    fields[3].structSize = sizeof(McFormField);
    fields[3].kind = kMcFormFieldCheckbox;
    fields[3].label = L"Split into multiple archives if needed";
    fields[3].boolValue = false;

    if (!g_host->ShowForm(L"Pack files", fields, 4)) {
        LogInfo(L"Pack file: cancelled (options dialog).");
        return;
    }

    bool includeSubdirs = fields[1].boolValue;
    int64_t maxSizeMB = fields[2].numberValue;
    bool splitEnabled = fields[3].boolValue;
    mz_uint levelAndFlags = CompressionLevelFromChoice(fields[0].choiceIndex);

    std::vector<std::wstring> selected = GetSelectedNamesList();
    if (selected.empty()) {
        const wchar_t* lines[] = {L"Nothing selected to pack."};
        g_host->ShowMessage(L"Pack file", lines, 1);
        return;
    }

    wchar_t sourceDirBuf[1024] = {};
    g_host->GetActivePanelPath(sourceDirBuf, 1024);
    std::wstring sourceDir = sourceDirBuf;

    std::vector<PackEntry> entries;
    std::set<std::wstring> seenArchiveNames;
    for (const auto& name : selected) {
        std::wstring fullPath = JoinPath(sourceDir, name);
        DWORD attrs = GetFileAttributesW(fullPath.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) {
            LogWarning(L"Pack file: could not access \"" + fullPath + L"\", skipped.");
            continue;
        }
        std::vector<PackEntry> collected;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            if (includeSubdirs) {
                CollectRecursive(fullPath, name, collected);
            } else {
                CollectDirectFiles(fullPath, collected);
            }
        } else {
            PackEntry entry;
            entry.sourcePath = fullPath;
            entry.archiveName = name;
            entry.attributes = attrs;
            LARGE_INTEGER size{};
            HANDLE h = CreateFileW(fullPath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                GetFileSizeEx(h, &size);
                CloseHandle(h);
            }
            entry.sizeBytes = static_cast<uint64_t>(size.QuadPart);
            collected.push_back(std::move(entry));
        }
        for (auto& entry : collected) {
            if (!seenArchiveNames.insert(entry.archiveName).second) {
                LogWarning(L"Pack file: duplicate archive path \"" + entry.archiveName + L"\", skipped.");
                continue;
            }
            entries.push_back(std::move(entry));
        }
    }

    if (entries.empty()) {
        const wchar_t* lines[] = {L"Nothing to pack (selection was empty or every item was inaccessible)."};
        g_host->ShowMessage(L"Pack file", lines, 1);
        return;
    }

    McPanelSide activeSide = g_host->GetActiveSide();
    McPanelSide otherSide = (activeSide == kMcPanelLeft) ? kMcPanelRight : kMcPanelLeft;
    wchar_t destDirBuf[1024] = {};
    g_host->GetPanelPath(otherSide, destDirBuf, 1024);
    std::wstring destDir = destDirBuf;

    std::wstring defaultOutput = JoinPath(destDir, FolderNameOf(sourceDir) + L".zip");
    wchar_t outBuffer[2048] = {};
    if (!g_host->ShowTextPrompt(L"Pack to", defaultOutput.c_str(), outBuffer, 2048)) {
        LogInfo(L"Pack file: cancelled (output location).");
        return;
    }
    std::wstring outputPath = outBuffer;
    if (outputPath.empty()) {
        LogWarning(L"Pack file: empty output path, cancelled.");
        return;
    }

    uint64_t maxSizeBytes = static_cast<uint64_t>(maxSizeMB) * 1024ull * 1024ull;

    std::vector<std::vector<PackEntry>> bins;
    if (maxSizeBytes == 0 || !splitEnabled) {
        bins.push_back(entries);
    } else {
        std::vector<PackEntry> current;
        uint64_t currentSize = 0;
        for (auto& entry : entries) {
            if (!current.empty() && currentSize + entry.sizeBytes > maxSizeBytes) {
                bins.push_back(std::move(current));
                current.clear();
                currentSize = 0;
            }
            currentSize += entry.sizeBytes;
            current.push_back(entry);
        }
        if (!current.empty()) bins.push_back(std::move(current));
    }

    int written = 0;
    uint64_t totalItems = entries.size();
    ArchiveProgress packProgress(L"Packing files", totalItems);
    uint64_t processed = 0;
    bool cancelled = false;
    for (size_t i = 0; i < bins.size() && !cancelled; ++i) {
        std::wstring binPath = (bins.size() == 1) ? outputPath : InsertBinSuffix(outputPath, static_cast<int>(i + 1));
        if (WriteZipArchive(bins[i], binPath, levelAndFlags, packProgress, processed, totalItems, cancelled)) {
            ++written;
            LogInfo(L"Pack file: wrote \"" + binPath + L"\" (" + std::to_wstring(bins[i].size()) + L" item(s)).");
        } else if (!cancelled) {
            const wchar_t* lines[] = {L"Failed writing the archive -- see the log for details."};
            g_host->ShowMessage(L"Pack file", lines, 1);
            return;
        }
    }
    if (cancelled) {
        LogInfo(L"Pack file: cancelled.");
        g_host->RefreshPanel(activeSide);
        g_host->RefreshPanel(otherSide);
        const wchar_t* lines[] = {L"Pack file cancelled."};
        g_host->ShowMessage(L"Pack file", lines, 1);
        return;
    }

    // §5's own after-the-fact notice: splitting wasn't requested, a cap was
    // configured, and the single resulting archive still exceeds it -- told
    // after the fact rather than predicted before compressing, since an
    // accurate prediction of compressed size isn't known ahead of time.
    if (maxSizeBytes != 0 && !splitEnabled && bins.size() == 1) {
        WIN32_FILE_ATTRIBUTE_DATA info{};
        if (GetFileAttributesExW(outputPath.c_str(), GetFileExInfoStandard, &info)) {
            uint64_t actualSize = (static_cast<uint64_t>(info.nFileSizeHigh) << 32) | info.nFileSizeLow;
            if (actualSize > maxSizeBytes) {
                const wchar_t* lines[] = {L"The resulting archive exceeds the configured maximum size."};
                g_host->ShowMessage(L"Pack file", lines, 1);
            }
        }
    }

    // Match the host's own Copy/Move convention: a completed operation
    // leaves the source selection cleared, which a real Panel::Refresh()
    // does as a side effect (every freshly-listed entry starts unselected).
    g_host->RefreshPanel(activeSide);
    g_host->RefreshPanel(otherSide);

    wchar_t summary[256];
    swprintf_s(summary, L"Packed %zu item(s) into %d archive(s).", entries.size(), written);
    const wchar_t* lines[] = {summary};
    g_host->ShowMessage(L"Pack file", lines, 1);
}

bool HasExtension(const std::wstring& name, const wchar_t* ext) {
    size_t extLen = wcslen(ext);
    if (name.size() < extLen) return false;
    return _wcsicmp(name.c_str() + (name.size() - extLen), ext) == 0;
}

// ARC-001: plain ZIP files under another extension that are worth browsing
// and unpacking -- Java/Android/browser/IDE/package-manager packages.
// Office/ODF/EPUB documents are ZIP containers too but deliberately not
// claimed: Enter on a .docx must keep opening the document. Note that
// saving an edit into a signed package (.jar/.apk/.vsix) invalidates its
// signature, exactly as any other ZIP tool would.
bool IsZipContainerName(const std::wstring& name) {
    static const wchar_t* const kExtensions[] = {L".zip", L".jar", L".war", L".ear", L".apk",
                                                 L".xpi", L".vsix", L".nupkg", L".whl"};
    return std::any_of(std::begin(kExtensions), std::end(kExtensions),
                       [&](const wchar_t* ext) { return HasExtension(name, ext); });
}

std::wstring StripExtension(const std::wstring& name) {
    size_t dot = name.find_last_of(L'.');
    return dot == std::wstring::npos ? name : name.substr(0, dot);
}

// Extracts every entry of the archive at `zipPath` into `destDir`
// (created if needed), restoring each entry's stored Win32 attributes.
// Reads the whole archive into memory first (ReadFileBytes, Unicode-safe)
// rather than any of miniz's own char*-based file-opening functions, the
// same discipline WriteZipArchive's own doc comment explains for writing.
// Returns the number of files actually extracted; entries whose own name
// fails IsSafeArchiveEntryName, or that would collide with a file already
// at the destination, are skipped and logged, not overwritten -- the same
// non-interactive-batch-operation convention Pack's own flattening
// collision case already uses.
int ExtractZipArchive(const std::wstring& zipPath, const std::wstring& destDir, ArchiveProgress& progress,
                       uint64_t& processed, bool& cancelled) {
    std::vector<uint8_t> zipBytes = ReadFileBytes(zipPath);
    if (zipBytes.empty()) {
        LogError(L"Unpack Package: could not read \"" + zipPath + L"\".");
        return 0;
    }

    mz_zip_archive zip;
    memset(&zip, 0, sizeof(zip));
    if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) {
        LogError(L"Unpack Package: \"" + zipPath + L"\" is not a valid archive (or is corrupted).");
        return 0;
    }

    int extracted = 0;
    mz_uint fileCount = mz_zip_reader_get_num_files(&zip);
    for (mz_uint i = 0; i < fileCount; ++i) {
        // IS-0010: unpacking had no progress/cancel at all before this --
        // same CancelPoll-shaped check as Pack/ExtractOrMove above.
        if (g_host->PollCancel && g_host->PollCancel()) { cancelled = true; break; }
        mz_zip_archive_file_stat stat;
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;

        std::wstring entryName = WideFromUtf8(stat.m_filename);
        progress.Update(processed++, 0, entryName);
        if (!IsSafeArchiveEntryName(entryName)) {
            LogWarning(L"Unpack Package: unsafe entry name \"" + entryName + L"\" in \"" + zipPath + L"\", skipped.");
            continue;
        }
        std::replace(entryName.begin(), entryName.end(), L'/', L'\\');
        std::wstring destPath = JoinPath(destDir, entryName);

        if (stat.m_is_directory) {
            // IS-0012: an explicit empty-directory marker entry (e.g. one
            // created via the archive browser's own F7) -- create it on
            // disk too, so it survives a real unpack rather than only ever
            // being implied by nested files the way every other directory
            // in an archive this plugin writes always was before this.
            while (!destPath.empty() && destPath.back() == L'\\') destPath.pop_back();
            EnsureDirectoryExists(destPath);
            continue;
        }

        if (GetFileAttributesW(destPath.c_str()) != INVALID_FILE_ATTRIBUTES) {
            LogWarning(L"Unpack Package: \"" + destPath + L"\" already exists, skipped.");
            continue;
        }

        size_t slash = destPath.find_last_of(L'\\');
        if (slash != std::wstring::npos) EnsureDirectoryExists(destPath.substr(0, slash));

        size_t extractedSize = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &extractedSize, 0);
        if (!data) {
            LogError(L"Unpack Package: failed extracting \"" + entryName + L"\" from \"" + zipPath + L"\".");
            continue;
        }
        std::vector<uint8_t> bytes(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + extractedSize);
        mz_free(data);

        if (!WriteFileBytes(destPath, bytes)) {
            LogError(L"Unpack Package: could not write \"" + destPath + L"\".");
            continue;
        }
        // Only the commonly Win32-settable bits -- stat.m_external_attr may
        // also carry bits (COMPRESSED, ENCRYPTED, REPARSE_POINT, ...) that
        // SetFileAttributesW either rejects or can't meaningfully set from
        // a plain write.
        constexpr uint32_t kSettableAttrs =
            FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
        uint32_t attrs = stat.m_external_attr & kSettableAttrs;
        if (attrs) SetFileAttributesW(destPath.c_str(), attrs);

        ++extracted;
    }
    mz_zip_reader_end(&zip);
    return extracted;
}

void UnpackSelection() {
    if (!g_host) return;

    std::vector<std::wstring> selected = GetSelectedNamesList();
    wchar_t sourceDirBuf[1024] = {};
    g_host->GetActivePanelPath(sourceDirBuf, 1024);
    std::wstring sourceDir = sourceDirBuf;

    std::vector<std::wstring> zipPaths;
    for (const auto& name : selected) {
        if (!IsZipContainerName(name)) {
            LogWarning(L"Unpack Package: \"" + name + L"\" is not a ZIP archive, skipped.");
            continue;
        }
        std::wstring fullPath = JoinPath(sourceDir, name);
        if (GetFileAttributesW(fullPath.c_str()) == INVALID_FILE_ATTRIBUTES) {
            LogWarning(L"Unpack Package: could not access \"" + fullPath + L"\", skipped.");
            continue;
        }
        zipPaths.push_back(fullPath);
    }

    if (zipPaths.empty()) {
        const wchar_t* lines[] = {L"No ZIP archive (.zip, .jar, ...) selected to unpack."};
        g_host->ShowMessage(L"Unpack Package", lines, 1);
        return;
    }

    McPanelSide activeSide = g_host->GetActiveSide();
    McPanelSide otherSide = (activeSide == kMcPanelLeft) ? kMcPanelRight : kMcPanelLeft;
    wchar_t destDirBuf[1024] = {};
    g_host->GetPanelPath(otherSide, destDirBuf, 1024);
    std::wstring destDir = destDirBuf;

    wchar_t outBuffer[2048] = {};
    if (!g_host->ShowTextPrompt(L"Unpack to", destDir.c_str(), outBuffer, 2048)) {
        LogInfo(L"Unpack Package: cancelled (destination).");
        return;
    }
    std::wstring baseDest = outBuffer;
    if (baseDest.empty()) {
        LogWarning(L"Unpack Package: empty destination, cancelled.");
        return;
    }
    EnsureDirectoryExists(baseDest);

    // One subfolder per archive when extracting more than one at once (the
    // same convention mainstream archive tools already default to) --
    // extracting a single archive goes straight into baseDest, no extra
    // nesting layer, matching Pack's own single-archive-to-flat-location
    // convention.
    int totalExtracted = 0;
    int archivesProcessed = 0;
    ArchiveProgress unpackProgress(L"Unpacking files", 0);
    uint64_t processed = 0;
    bool cancelled = false;
    for (const auto& zipPath : zipPaths) {
        std::wstring thisDest = baseDest;
        if (zipPaths.size() > 1) {
            size_t slash = zipPath.find_last_of(L"\\/");
            std::wstring fileName = (slash == std::wstring::npos) ? zipPath : zipPath.substr(slash + 1);
            thisDest = JoinPath(baseDest, StripExtension(fileName));
            EnsureDirectoryExists(thisDest);
        }
        int extracted = ExtractZipArchive(zipPath, thisDest, unpackProgress, processed, cancelled);
        totalExtracted += extracted;
        ++archivesProcessed;
        LogInfo(L"Unpack Package: extracted " + std::to_wstring(extracted) + L" item(s) from \"" + zipPath + L"\".");
        if (cancelled) break;
    }

    // Match the host's own Copy/Move convention: clear the source (the
    // selected .zip files' own panel) selection too, not just refresh the
    // destination.
    g_host->RefreshPanel(activeSide);
    g_host->RefreshPanel(otherSide);

    if (cancelled) {
        LogInfo(L"Unpack Package: cancelled.");
        wchar_t cancelSummary[256];
        swprintf_s(cancelSummary, L"Cancelled -- extracted %d item(s) from %d archive(s) before stopping.",
                   totalExtracted, archivesProcessed);
        const wchar_t* cancelLines[] = {cancelSummary};
        g_host->ShowMessage(L"Unpack Package", cancelLines, 1);
        return;
    }

    wchar_t summary[256];
    swprintf_s(summary, L"Extracted %d item(s) from %d archive(s).", totalExtracted, archivesProcessed);
    const wchar_t* lines[] = {summary};
    g_host->ShowMessage(L"Unpack Package", lines, 1);
}

// Phase 3: a deliberately small virtual-directory model over ZIP's flat
// central directory. The archive stays in memory while open; mutations are
// committed by rewriting a complete replacement archive.
struct ArchiveEntry {
    std::wstring name;
    std::vector<uint8_t> bytes;
    uint32_t attributes = 0;
    uint64_t compressedSize = 0;
};
struct ArchiveView {
    bool open = false;
    McPanelSide side = kMcPanelLeft;
    std::wstring path;
    std::wstring title;
    std::wstring folder; // forward-slash prefix, empty at archive root
    std::vector<ArchiveEntry> entries;
    std::set<std::wstring> selected;
    int cursor = 0;
    int topIndex = 0;
    int visibleRows = 10;
    bool mouseSelecting = false;
    bool mouseSelectValue = false;
    bool dirty = false;  // IS-0014: true once entries has unsaved changes
};
ArchiveView g_views[2];

ArchiveView& ViewFor(McPanelSide side) { return g_views[side == kMcPanelLeft ? 0 : 1]; }
std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}
bool LoadArchive(const std::wstring& path, ArchiveView& view) {
    ArchiveProgress progress(L"Opening ZIP archive", 0);
    progress.Update(0, 0, L"Reading archive from disk...");
    std::vector<uint8_t> zipBytes = ReadFileBytes(path);
    mz_zip_archive zip{};
    if (zipBytes.empty() || !mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) return false;
    view.entries.clear();
    for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&zip); i < count; ++i) {
        progress.Update(i, count, L"Reading archive entries...");
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        std::wstring name = WideFromUtf8(stat.m_filename);
        if (!IsSafeArchiveEntryName(name)) { LogWarning(L"Archive browser: unsafe entry skipped: " + name); continue; }
        if (stat.m_is_directory) {
            // IS-0012: an explicit empty-directory marker entry (name ends
            // in '/', zero bytes) -- loaded as a lightweight ArchiveEntry so
            // one created via F7 survives a save/close/reopen round trip.
            // CurrentItems already treats any entry whose name contains a
            // '/' as implying a directory, so no further special-casing is
            // needed to make it show up correctly.
            if (name.empty() || name.back() != L'/') name += L'/';
            view.entries.push_back(ArchiveEntry{name, {}, stat.m_external_attr, 0});
            continue;
        }
        progress.Update(i, count, name);
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, i, &size, 0);
        if (!data) { LogWarning(L"Archive browser: could not read " + name); continue; }
        ArchiveEntry entry{name, std::vector<uint8_t>(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size),
                           stat.m_external_attr, stat.m_comp_size};
        mz_free(data);
        view.entries.push_back(std::move(entry));
    }
    mz_zip_reader_end(&zip);
    return true;
}
// IS-0012: Ctrl+R, this project's own conventional "Refresh" key (see
// DEVELOPER_GUIDE.md's "Panel content" section) -- re-reads the archive
// from disk, picking up any external change to the underlying .zip file.
// LoadArchive only clears view.entries *after* confirming the read
// succeeded, so a failed reload (file deleted/corrupted externally) leaves
// the existing in-memory listing intact rather than wiping it.
void ReloadArchiveFromDisk(ArchiveView& view) {
    // IS-0014: a reload replaces view.entries wholesale from disk -- without
    // this check it would silently discard any not-yet-saved edit.
    if (view.dirty) {
        const wchar_t* lines[] = {L"This archive has unsaved changes -- reloading discards them."};
        McDialogOption options[] = {{L'Y', L"Reload anyway"}, {L'N', L"Cancel"}};
        if (g_host->ShowChoice(L"Refresh", lines, 1, options, 2) != 0) return;
    }
    if (LoadArchive(view.path, view)) {
        view.selected.clear();  // the entry set may have changed
        view.dirty = false;     // a fresh read from disk; any prior pending edits are gone
        LogInfo(L"Archive browser: reloaded \"" + view.path + L"\" from disk.");
    } else {
        const wchar_t* lines[] = {L"Could not reload the archive -- it may have been deleted or is no longer a valid ZIP file."};
        g_host->ShowMessage(L"Refresh", lines, 1);
    }
}
bool WriteArchiveEntries(const std::wstring& path, const std::vector<ArchiveEntry>& entries, ArchiveProgress* existing = nullptr) {
    ArchiveProgress ownProgress(L"Saving ZIP archive", entries.size(), existing == nullptr);
    auto& progress = existing ? *existing : ownProgress;
    progress.Update(0, entries.size(), L"Rebuilding archive...");
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return false;
    std::vector<uint32_t> attrs;
    bool ok = true;
    for (const auto& e : entries) {
        progress.Update(attrs.size(), entries.size(), L"Compressing: " + e.name);
        std::string name = Utf8ArchiveName(e.name);
        const void* data = e.bytes.empty() ? static_cast<const void*>("") : e.bytes.data();
        if (!mz_zip_writer_add_mem(&zip, name.c_str(), data, e.bytes.size(), MZ_DEFAULT_LEVEL)) { ok = false; break; }
        attrs.push_back(e.attributes);
    }
    void* heap = nullptr; size_t bytes = 0;
    progress.Update(attrs.size(), entries.size(), L"Finalizing archive...");
    if (ok) ok = mz_zip_writer_finalize_heap_archive(&zip, &heap, &bytes) != 0;
    mz_zip_writer_end(&zip);
    if (!ok) { if (heap) free(heap); return false; }
    std::vector<uint8_t> archive(static_cast<uint8_t*>(heap), static_cast<uint8_t*>(heap) + bytes);
    free(heap);
    PatchExternalAttributes(archive, attrs);
    progress.Update(entries.size(), entries.size(), L"Writing archive to disk...");
    return WriteFileBytes(path, archive);
}
// IS-0014: updates the in-memory listing and marks it unsaved -- deliberately
// no disk write here. Every mutating operation in this file (add/delete/
// attribute-toggle/create-directory/move-out) used to call WriteArchiveEntries
// immediately, repacking the *entire* archive on every single edit; now they
// all funnel through this instead, and the actual repack-and-write happens
// only once, via SaveArchiveIfDirty below, when the user leaves the archive.
void ApplyArchiveChanges(ArchiveView& view, std::vector<ArchiveEntry> replacement) {
    view.entries = std::move(replacement);
    view.dirty = true;
}
// The one place an actual repack-and-write happens for these deferred edits
// (Pack/Unpack's own one-shot operations are unrelated and unaffected). A
// no-op returning true if nothing is pending, so callers can call this
// unconditionally without checking `dirty` themselves first.
bool SaveArchiveIfDirty(ArchiveView& view, ArchiveProgress* existing = nullptr) {
    if (!view.dirty) return true;
    if (!WriteArchiveEntries(view.path, view.entries, existing)) return false;
    view.dirty = false;
    return true;
}
struct ViewItem { std::wstring label, key; bool directory = false; uint64_t size = 0, compressed = 0; uint32_t attrs = 0; };
std::vector<ViewItem> CurrentItems(const ArchiveView& view) {
    std::vector<ViewItem> result;
    if (!view.folder.empty()) result.push_back({L"..", L"..", true});
    std::set<std::wstring> dirs;
    for (const auto& e : view.entries) {
        if (e.name.rfind(view.folder, 0) != 0) continue;
        std::wstring rest = e.name.substr(view.folder.size());
        size_t slash = rest.find(L'/');
        if (slash != std::wstring::npos) dirs.insert(rest.substr(0, slash));
        else result.push_back({rest, e.name, false, e.bytes.size(), e.compressedSize, e.attributes});
    }
    std::transform(dirs.begin(), dirs.end(), std::back_inserter(result),
                   [&](const std::wstring& dir) { return ViewItem{dir, view.folder + dir + L"/", true}; });
    std::stable_sort(result.begin() + (view.folder.empty() ? 0 : 1), result.end(), [](const ViewItem& a, const ViewItem& b) {
        if (a.directory != b.directory) return a.directory > b.directory;
        return _wcsicmp(a.label.c_str(), b.label.c_str()) < 0;
    });
    return result;
}
std::vector<std::wstring> SelectedPrefixes(const ArchiveView& view, const std::vector<ViewItem>& items) {
    std::vector<std::wstring> selected;
    for (const auto& item : items) {
        if (item.key == L"..") continue;
        if (view.selected.count(item.key))
            selected.push_back(item.key);
    }
    if (selected.empty() && view.cursor >= 0 && view.cursor < (int)items.size() && items[view.cursor].key != L"..")
        selected.push_back(items[view.cursor].key);
    return selected;
}
bool MatchesAny(const std::wstring& name, const std::vector<std::wstring>& prefixes) {
    for (const auto& p : prefixes) if (name == p || (!p.empty() && p.back() == L'/' && name.rfind(p, 0) == 0)) return true;
    return false;
}
void ClampArchiveCursor(ArchiveView& view, int itemCount) {
    view.cursor = std::clamp(view.cursor, 0, std::max(0, itemCount - 1));
    if (view.cursor < view.topIndex) view.topIndex = view.cursor;
    if (view.cursor >= view.topIndex + view.visibleRows)
        view.topIndex = view.cursor - view.visibleRows + 1;
    view.topIndex = std::clamp(view.topIndex, 0, std::max(0, itemCount - view.visibleRows));
}
struct ArchiveColumns {
    size_t markerWidth = 0;
    size_t nameWidth = 0;
    int detailCount = 0;
};
constexpr size_t kArchiveDetailWidths[] = {10, 10, 8};
ArchiveColumns FitArchiveColumns(size_t width) {
    ArchiveColumns columns{std::min<size_t>(3, width), 0, 3};
    size_t reserved = std::accumulate(std::begin(kArchiveDetailWidths), std::end(kArchiveDetailWidths), columns.markerWidth,
                                      [](size_t sum, size_t detailWidth) { return sum + 1 + detailWidth; });
    // Keep a usable name field; drop attributes, then packed size, then size.
    while (columns.detailCount && width < reserved + 8) {
        reserved -= 1 + kArchiveDetailWidths[--columns.detailCount];
    }
    columns.nameWidth = width - reserved;
    return columns;
}
std::wstring ArchiveRightAlign(const std::wstring& text, size_t width) {
    std::wstring trimmed = mc::PadToDisplayWidth(text, width);
    size_t used = std::min<size_t>(width, mc::StringDisplayWidth(text));
    return std::wstring(width - used, L' ') + trimmed.substr(0, trimmed.size() - (width - used));
}
// Mirrors the host's own main.cpp FormatAttributes -- this plugin links no
// host internals, so the 4-char R/H/S/A convention is duplicated here rather
// than shared, matching how JoinPath/FolderNameOf are already duplicated
// small helpers rather than host dependencies.
std::wstring FormatZipAttributes(uint32_t attrs) {
    std::wstring s = L"----";
    if (attrs & FILE_ATTRIBUTE_READONLY) s[0] = L'R';
    if (attrs & FILE_ATTRIBUTE_HIDDEN) s[1] = L'H';
    if (attrs & FILE_ATTRIBUTE_SYSTEM) s[2] = L'S';
    if (attrs & FILE_ATTRIBUTE_ARCHIVE) s[3] = L'A';
    return s;
}
std::wstring FormatArchiveLine(const ArchiveColumns& columns, const std::wstring& marker,
                               const std::wstring& name, const std::wstring& size,
                               const std::wstring& packed, const std::wstring& attributes) {
    std::wstring line = mc::PadToDisplayWidth(marker, columns.markerWidth) +
                        mc::PadToDisplayWidth(name, columns.nameWidth);
    const std::wstring* details[] = {&size, &packed, &attributes};
    for (int i = 0; i < columns.detailCount; ++i)
        line += L" " + ArchiveRightAlign(*details[i], kArchiveDetailWidths[i]);
    return line;
}
void DrawArchive(McPanelSide side, uint16_t width, uint16_t height, bool active) {
    ArchiveView& view = ViewFor(side); if (!view.open || !g_host) return;
    auto items = CurrentItems(view);
    view.visibleRows = std::max(1, (int)height - 1);
    ClampArchiveCursor(view, (int)items.size());
    const auto columns = FitArchiveColumns(width);
    // IS-0014: a cheap, self-contained "unsaved changes" marker -- no host/
    // SDK change needed, since this is drawn the same way every other row
    // already is.
    std::wstring header = FormatArchiveLine(columns, L"", view.dirty ? L"Name *" : L"Name", L"Size", L"Packed", L"Attr");
    g_host->PanelDrawText(side, 0, 0, header.c_str(), kMcPanelColorSection);
    int rows = std::max(0, (int)height - 1), first = view.topIndex;
    for (int row = 0; row < rows && first + row < (int)items.size(); ++row) {
        const auto& item = items[first + row];
        bool atCursor = active && first + row == view.cursor;
        std::wstring marker = atCursor ? L">" : L" ";
        marker += view.selected.count(item.key) ? L"* " : L"  ";
        std::wstring attributes = item.directory ? L"" : FormatZipAttributes(item.attrs);
        std::wstring line = FormatArchiveLine(columns, marker,
            item.label + (item.directory ? L"/" : L""),
            item.directory ? L"<DIR>" : std::to_wstring(item.size),
            item.directory ? L"" : std::to_wstring(item.compressed), attributes);
        g_host->PanelDrawText(side, 0, (int16_t)(row + 1), line.c_str(), atCursor ? kMcPanelColorHighlight :
                              (view.selected.count(item.key) ? kMcPanelColorSelected :
                               (item.directory ? kMcPanelColorSection : kMcPanelColorNormal)));
    }
}
void LeaveOrUp(ArchiveView& view) {
    if (!view.folder.empty()) { size_t end = view.folder.size() - 1, slash = view.folder.find_last_of(L'/', end - 1); view.folder = slash == std::wstring::npos ? L"" : view.folder.substr(0, slash + 1); view.cursor = 0; return; }
    if (!view.dirty) {
        // ZIP-007's own plain confirmation, unchanged -- nothing pending to save.
        std::wstring question = L"Leave " + view.title + L"?";
        const wchar_t* lines[] = {question.c_str()}; McDialogOption options[] = {{L'Y', L"Yes"}, {L'N', L"No"}};
        if (g_host->ShowChoice(L"Leave archive", lines, 1, options, 2) == 0) { g_host->ReleasePanelContent(view.side); view = ArchiveView{}; }
        return;
    }
    // IS-0014: dirty -- the only point any of this archive's deferred edits
    // actually get saved (or discarded) is right here.
    const wchar_t* lines[] = {L"This archive has unsaved changes."};
    McDialogOption options[] = {{L'Y', L"Save and leave"}, {L'N', L"Discard and leave"}, {L'C', L"Cancel"}};
    int choice = g_host->ShowChoice(L"Leave archive", lines, 1, options, 3);
    if (choice == 0) {
        if (!SaveArchiveIfDirty(view)) {
            const wchar_t* err[] = {L"Could not save the archive -- staying open so nothing is lost."};
            g_host->ShowMessage(L"Leave archive", err, 1);
            return;  // stay open, still dirty -- let the user retry or discard next time
        }
        g_host->ReleasePanelContent(view.side);
        view = ArchiveView{};
    } else if (choice == 1) {
        g_host->ReleasePanelContent(view.side);
        view = ArchiveView{};  // discard -- the on-disk file was never touched, nothing to undo
    }
    // choice == 2 or Esc (-1): Cancel -- stay, change nothing.
}
void ExtractOrMove(ArchiveView& view, bool move) {
    auto items = CurrentItems(view); auto prefixes = SelectedPrefixes(view, items); if (prefixes.empty()) return;
    McPanelSide other = view.side == kMcPanelLeft ? kMcPanelRight : kMcPanelLeft; wchar_t dest[1024] = {}; g_host->GetPanelPath(other, dest, 1024);
    const auto total = std::count_if(view.entries.begin(), view.entries.end(), [&](const ArchiveEntry& e) { return MatchesAny(e.name, prefixes); });
    ArchiveProgress progress(move ? L"Moving from ZIP archive" : L"Copying from ZIP archive", total);
    uint64_t processed = 0;
    int written = 0; std::set<std::wstring> movedNames;
    enum class Blanket { None, OverwriteAll, SkipAll } blanket = Blanket::None;
    for (const auto& e : view.entries) if (MatchesAny(e.name, prefixes)) {
        // IS-0010: poll Esc every item, the same shape as FileOps.h's own
        // CancelPoll, so cancelling mid-extraction actually stops the loop
        // instead of the keypress sitting unconsumed until it finishes.
        if (g_host->PollCancel && g_host->PollCancel()) break;
        progress.Update(processed++, total, e.name);
        std::wstring rel = e.name.substr(view.folder.size()); std::replace(rel.begin(), rel.end(), L'/', L'\\'); std::wstring out = JoinPath(dest, rel);
        // IS-0012: a directory-marker entry (name ends in '/', zero bytes --
        // e.g. one created via F7) has no real content to write; just
        // ensure the directory itself exists on the destination side.
        if (!e.name.empty() && e.name.back() == L'/') {
            while (!out.empty() && out.back() == L'\\') out.pop_back();
            EnsureDirectoryExists(out);
            if (move) movedNames.insert(e.name);
            continue;
        }
        if (GetFileAttributesW(out.c_str()) != INVALID_FILE_ATTRIBUTES) {
            if (blanket == Blanket::SkipAll) continue;
            if (blanket != Blanket::OverwriteAll) {
                const wchar_t* lines[] = {L"Destination already exists:", out.c_str()};
                McDialogOption options[] = {{L'O', L"Overwrite"}, {L'A', L"Overwrite all"}, {L'S', L"Skip"},
                                             {L'K', L"Skip all"}, {L'C', L"Cancel"}};
                int choice = g_host->ShowChoice(L"Extract from archive", lines, 2, options, 5);
                if (choice == 4 || choice < 0) break;
                else if (choice == 1) blanket = Blanket::OverwriteAll;
                else if (choice == 2) continue;
                else if (choice == 3) { blanket = Blanket::SkipAll; continue; }
                // choice == 0: overwrite this one only, fall through to write below.
            }
        }
        size_t slash = out.find_last_of(L'\\'); if (slash != std::wstring::npos) EnsureDirectoryExists(out.substr(0, slash));
        if (WriteFileBytes(out, e.bytes)) { constexpr uint32_t settable = FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE; if (e.attributes & settable) SetFileAttributesW(out.c_str(), e.attributes & settable); ++written; movedNames.insert(e.name); }
        else { const wchar_t* lines[] = {L"Could not write destination:", out.c_str()}; g_host->ShowMessage(L"Extract from archive", lines, 2); }
    }
    progress.Update(processed, total, L"Refreshing destination...");
    g_host->RefreshPanel(other);
    if (move && !movedNames.empty()) {
        // IS-0014: deferred -- the moved files are already safely on the
        // real filesystem; whether the archive's own shrunk copy is written
        // now or when the archive is left no longer matters for safety.
        auto replacement = view.entries;
        replacement.erase(std::remove_if(replacement.begin(), replacement.end(), [&](const ArchiveEntry& e) { return movedNames.count(e.name) != 0; }), replacement.end());
        ApplyArchiveChanges(view, std::move(replacement));
        view.selected.clear();
    } else if (!move && written > 0) {
        // Match the host's own Copy convention: a completed copy clears the
        // source selection, even though (unlike Move) the archive itself is
        // unchanged.
        view.selected.clear();
    }
}
void ViewArchiveFile(ArchiveView& view) {
    auto items = CurrentItems(view);
    if (items.empty() || items[view.cursor].directory) return;
    const auto& key = items[view.cursor].key;
    auto entry = std::find_if(view.entries.begin(), view.entries.end(), [&](const ArchiveEntry& e) { return e.name == key; });
    if (entry == view.entries.end() || !g_host->ShowFileViewer) return;
    wchar_t dir[MAX_PATH], path[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, dir) || !GetTempFileNameW(dir, L"mcz", 0, path)) return;
    if (WriteFileBytes(path, entry->bytes)) g_host->ShowFileViewer(path);
    DeleteFileW(path);
}
void ChangeArchiveAttributes(ArchiveView& view) {
    auto prefixes = SelectedPrefixes(view, CurrentItems(view));
    if (prefixes.empty()) return;
    const wchar_t* lines[] = {L"Toggle an attribute on the selected archive files."};
    McDialogOption options[] = {{L'R', L"Read-only"}, {L'H', L"Hidden"}, {L'S', L"System"}, {L'A', L"Archive"}, {L'C', L"Cancel"}};
    int choice = g_host->ShowChoice(L"Archive attributes", lines, 1, options, 5);
    if (choice < 0 || choice >= 4) return;
    const uint32_t flags[] = {FILE_ATTRIBUTE_READONLY, FILE_ATTRIBUTE_HIDDEN, FILE_ATTRIBUTE_SYSTEM, FILE_ATTRIBUTE_ARCHIVE};
    auto replacement = view.entries;
    for (auto& e : replacement) if (MatchesAny(e.name, prefixes)) e.attributes ^= flags[choice];
    ApplyArchiveChanges(view, std::move(replacement));  // IS-0014: deferred -- saved when leaving the archive
}
void DeleteFromArchive(ArchiveView& view) {
    auto prefixes = SelectedPrefixes(view, CurrentItems(view)); if (prefixes.empty()) return;
    const wchar_t* lines[] = {L"Delete selected archive entries permanently?"}; McDialogOption options[] = {{L'Y', L"Yes"}, {L'N', L"No"}};
    if (g_host->ShowChoice(L"Delete from archive", lines, 1, options, 2) != 0) return;
    // IS-0014: deferred -- an in-memory erase is instant, no progress dialog
    // needed now that this no longer repacks the archive immediately.
    auto replacement = view.entries;
    auto oldSize = replacement.size(); replacement.erase(std::remove_if(replacement.begin(), replacement.end(), [&](const ArchiveEntry& e) { return MatchesAny(e.name, prefixes); }), replacement.end());
    auto deletedCount = oldSize - replacement.size();
    ApplyArchiveChanges(view, std::move(replacement));
    view.selected.clear(); view.cursor = 0;
    LogInfo(L"Archive browser: deleted " + std::to_wstring(deletedCount) + L" item(s) (not yet saved).");
}

// IS-0012: F7, "Create directory" -- adds a real, persisted empty-directory
// marker entry (name ends in '/', zero bytes; see LoadArchive's own comment
// on why this round-trips correctly as a directory through miniz and any
// other zip tool), not a purely virtual folder that would vanish if the
// user never adds a file to it.
void CreateDirectoryInArchive(ArchiveView& view) {
    wchar_t nameBuffer[260] = {};
    if (!g_host->ShowTextPrompt(L"Create directory", L"", nameBuffer, 260)) return;
    std::wstring name = nameBuffer;
    while (!name.empty() && (name.back() == L'/' || name.back() == L'\\')) name.pop_back();
    if (name.empty() || name.find(L'/') != std::wstring::npos || name.find(L'\\') != std::wstring::npos || name == L"..") {
        const wchar_t* lines[] = {L"Enter a single directory name (no / or \\), not a path."};
        g_host->ShowMessage(L"Create directory", lines, 1);
        return;
    }

    auto items = CurrentItems(view);
    for (const auto& item : items) {
        if (item.key == L"..") continue;
        if (_wcsicmp(item.label.c_str(), name.c_str()) != 0) continue;
        if (!item.directory) {
            const wchar_t* lines[] = {L"A file with that name already exists here."};
            g_host->ShowMessage(L"Create directory", lines, 1);
            return;
        }
        // Already exists as a directory -- a harmless no-op; just move the
        // cursor onto it rather than writing a duplicate marker entry.
        for (size_t i = 0; i < items.size(); ++i) {
            if (items[i].key == item.key) { view.cursor = static_cast<int>(i); break; }
        }
        return;
    }

    std::wstring fullName = view.folder + name + L"/";
    auto replacement = view.entries;
    replacement.push_back(ArchiveEntry{fullName, {}, FILE_ATTRIBUTE_DIRECTORY, 0});
    ApplyArchiveChanges(view, std::move(replacement));  // IS-0014: deferred -- saved when leaving the archive
    LogInfo(L"Archive browser: created directory \"" + fullName + L"\" (not yet saved).");

    // IS-0001's own convention: a newly created item is selected (cursor
    // lands on it) once the listing reflects the change.
    auto refreshed = CurrentItems(view);
    for (size_t i = 0; i < refreshed.size(); ++i) {
        if (refreshed[i].directory && _wcsicmp(refreshed[i].label.c_str(), name.c_str()) == 0) {
            view.cursor = static_cast<int>(i);
            break;
        }
    }
}

// FEA-0008 (ZIP-011): Shift+F5 inside an archive -- copy the cursor entry
// under a new name or relative path, *within* the archive. Purely in memory,
// deferred through ApplyArchiveChanges like every other archive edit
// (ZIP-010). Mirrors the host's own file-panel Shift+F5 (FOP-019,
// ResolveCopyAsTarget): a name lands beside the original, an existing folder
// or a trailing slash means "inside it, keeping the name".
bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}
bool ArchiveFolderExists(const std::vector<ArchiveEntry>& entries, const std::wstring& prefix) {
    return std::any_of(entries.begin(), entries.end(),
                       [&](const ArchiveEntry& e) { return StartsWithNoCase(e.name, prefix); });
}
int FindArchiveEntryNoCase(const std::vector<ArchiveEntry>& entries, const std::wstring& name) {
    for (size_t i = 0; i < entries.size(); ++i)
        if (_wcsicmp(entries[i].name.c_str(), name.c_str()) == 0) return static_cast<int>(i);
    return -1;
}

// `targetKey` empty with no `error` means "nothing to do" (empty input).
struct ArchiveCopyTarget { std::wstring targetKey; std::wstring error; };

// Resolves what the user typed, relative to `folder` (the archive's current
// forward-slash folder prefix), into the full archive name a copy of
// `sourceKey` (a file "a/b.txt" or a folder "a/b/") would get. Comparisons
// are case-insensitive, since the archive's names land on a Windows file
// system once extracted, where "A.txt" and "a.txt" collide.
ArchiveCopyTarget ResolveArchiveCopyTarget(const std::vector<ArchiveEntry>& entries, const std::wstring& folder,
                                           const std::wstring& sourceKey, const std::wstring& typed) {
    std::wstring s = typed;
    size_t start = s.find_first_not_of(L" \t");
    if (start == std::wstring::npos) return {};
    s = s.substr(start, s.find_last_not_of(L" \t") - start + 1);
    if (s.size() >= 2 && s.front() == L'"' && s.back() == L'"') s = s.substr(1, s.size() - 2);
    std::replace(s.begin(), s.end(), L'\\', L'/');
    bool into = false;
    while (!s.empty() && s.back() == L'/') { s.pop_back(); into = true; }
    if (s.empty()) return {};

    const std::wstring kOutside = L"Enter a name or a path inside the archive (no drive, leading slash, . or ..).";
    if (!IsSafeArchiveEntryName(s)) return {L"", kOutside};
    for (size_t pos = 0; pos <= s.size();) {
        size_t slash = s.find(L'/', pos);
        std::wstring segment = s.substr(pos, slash == std::wstring::npos ? std::wstring::npos : slash - pos);
        if (segment.empty() || segment == L".") return {L"", kOutside};
        if (slash == std::wstring::npos) break;
        pos = slash + 1;
    }

    bool sourceIsDir = !sourceKey.empty() && sourceKey.back() == L'/';
    std::wstring trimmedSource = sourceIsDir ? sourceKey.substr(0, sourceKey.size() - 1) : sourceKey;
    size_t lastSlash = trimmedSource.find_last_of(L'/');
    std::wstring leaf = lastSlash == std::wstring::npos ? trimmedSource : trimmedSource.substr(lastSlash + 1);

    std::wstring candidate = folder + s;
    if (!into && ArchiveFolderExists(entries, candidate + L"/") && _wcsicmp((candidate + L"/").c_str(), sourceKey.c_str()) != 0)
        into = true;
    std::wstring target = into ? candidate + L"/" + leaf : candidate;
    if (sourceIsDir) target += L"/";

    if (_wcsicmp(target.c_str(), sourceKey.c_str()) == 0) return {L"", L"The new name is the same as the original."};
    if (sourceIsDir && StartsWithNoCase(target, sourceKey)) return {L"", L"Cannot place a directory inside itself."};

    // A file standing where the target (or one of its parent folders) would go.
    std::wstring bare = sourceIsDir ? target.substr(0, target.size() - 1) : target;
    for (size_t slash = bare.find(L'/'); ; slash = bare.find(L'/', slash + 1)) {
        std::wstring prefix = slash == std::wstring::npos ? bare : bare.substr(0, slash);
        bool isLast = slash == std::wstring::npos;
        if (!(isLast && !sourceIsDir) && FindArchiveEntryNoCase(entries, prefix) >= 0)
            return {L"", L"A file named \"" + prefix + L"\" is in the way."};
        if (isLast) break;
    }
    if (!sourceIsDir && ArchiveFolderExists(entries, target + L"/"))
        return {L"", L"A folder with that name already exists here."};
    return {target, L""};
}

// The (source name, target name) entry pairs copying `sourceKey` to
// `targetKey` produces: one pair for a file, every entry under the prefix
// (its own "dir/" marker included, if any) for a folder.
std::vector<std::pair<std::wstring, std::wstring>> PlanArchiveCopy(const std::vector<ArchiveEntry>& entries,
                                                                   const std::wstring& sourceKey,
                                                                   const std::wstring& targetKey) {
    std::vector<std::pair<std::wstring, std::wstring>> plan;
    bool sourceIsDir = !sourceKey.empty() && sourceKey.back() == L'/';
    for (const auto& e : entries) {
        if (sourceIsDir ? e.name.rfind(sourceKey, 0) == 0 : e.name == sourceKey)
            plan.emplace_back(e.name, targetKey + e.name.substr(sourceKey.size()));
    }
    return plan;
}

void CopyWithinArchive(ArchiveView& view) {
    auto items = CurrentItems(view);
    if (items.empty() || view.cursor < 0 || view.cursor >= (int)items.size()) return;
    const ViewItem item = items[view.cursor];
    if (item.key == L"..") return;

    std::wstring title = L"Copy \"" + item.label + L"\" as:";
    wchar_t buffer[1024] = {};
    if (!g_host->ShowTextPrompt(title.c_str(), item.label.c_str(), buffer, 1024)) return;

    auto target = ResolveArchiveCopyTarget(view.entries, view.folder, item.key, buffer);
    if (!target.error.empty()) {
        const wchar_t* lines[] = {target.error.c_str()};
        g_host->ShowMessage(L"Copy in archive", lines, 1);
        return;
    }
    if (target.targetKey.empty()) return;
    auto plan = PlanArchiveCopy(view.entries, item.key, target.targetKey);
    if (plan.empty()) return;

    const auto conflicts = std::count_if(plan.begin(), plan.end(), [&](const auto& step) {
        return step.second.back() != L'/' && FindArchiveEntryNoCase(view.entries, step.second) >= 0;
    });
    bool overwrite = false;
    if (conflicts > 0) {
        std::wstring line = std::to_wstring(conflicts) + L" item(s) already exist at the target.";
        const wchar_t* lines[] = {line.c_str()};
        McDialogOption options[] = {{L'O', L"Overwrite"}, {L'S', L"Skip existing"}, {L'C', L"Cancel"}};
        int choice = g_host->ShowChoice(L"Copy in archive", lines, 1, options, 3);
        if (choice != 0 && choice != 1) return;
        overwrite = choice == 0;
    }

    auto replacement = view.entries;
    int copied = 0;
    for (const auto& [srcName, dstName] : plan) {
        int srcIndex = -1;
        for (size_t i = 0; i < view.entries.size(); ++i) if (view.entries[i].name == srcName) { srcIndex = (int)i; break; }
        if (srcIndex < 0) continue;
        const ArchiveEntry& src = view.entries[(size_t)srcIndex];
        int existing = FindArchiveEntryNoCase(replacement, dstName);
        if (existing >= 0) {
            if (dstName.back() == L'/' || !overwrite) continue;  // folder marker already there / skip
            replacement[(size_t)existing].bytes = src.bytes;
            replacement[(size_t)existing].attributes = src.attributes;
            replacement[(size_t)existing].compressedSize = 0;
            ++copied;
            continue;
        }
        replacement.push_back(ArchiveEntry{dstName, src.bytes, src.attributes, 0});
        ++copied;
    }
    if (copied == 0) return;
    ApplyArchiveChanges(view, std::move(replacement));  // IS-0014: deferred -- saved when leaving the archive
    LogInfo(L"Archive browser: copied \"" + item.key + L"\" to \"" + target.targetKey + L"\" (not yet saved).");

    // IS-0001 convention: the cursor lands on the new copy when it's in the current folder.
    auto refreshed = CurrentItems(view);
    for (size_t i = 0; i < refreshed.size(); ++i) {
        if (_wcsicmp(refreshed[i].key.c_str(), target.targetKey.c_str()) == 0) {
            view.cursor = static_cast<int>(i);
            break;
        }
    }
}

// Adds `other`'s selection into `view`'s archive. Factored out from
// AddOtherPanelSelection (Shift+Insert, which always computes `other` as
// this archive panel's own complement side) so McPlugin_OnReceiveFiles
// (IS-0011 follow-up: F5 Copy onto an open archive) can reuse the exact
// same logic with a host-supplied source side instead.
void AddFilesFromPanel(ArchiveView& view, McPanelSide other) {
    int count = g_host->GetPanelSelectedNames(other, nullptr, 0, 0);
    if (count <= 0) {
        const wchar_t* lines[] = {L"Nothing selected in the other panel to add."};
        g_host->ShowMessage(L"Add to archive", lines, 1);
        return;
    }
    std::vector<std::wstring> rawBuffers(count, std::wstring(1024, L'\0'));
    std::vector<wchar_t*> ptrs(count);
    for (int i = 0; i < count; ++i) ptrs[i] = rawBuffers[i].data();
    g_host->GetPanelSelectedNames(other, ptrs.data(), count, 1024);
    // IS-0011: GetPanelSelectedNames writes a null-terminated name into each
    // raw buffer but never shrinks the std::wstring's own length -- iterating
    // `rawBuffers` directly (as earlier code here did) leaves every name a
    // 1024-char string padded with embedded NULs past the real text. Every
    // WinAPI call that follows (GetFileAttributesW, CreateFileW,
    // WideCharToMultiByte in Utf8ArchiveName) still "works" because those
    // stop at the first embedded NUL, but plain wstring operations (operator+,
    // std::set's operator<) don't, so the garbage-padded name ends up stored
    // as the actual ArchiveEntry.name -- visibly corrupting the panel row
    // until the archive is closed and reopened, and defeating the duplicate
    // check below. Reconstructing via the wchar_t* constructor (the same
    // fix GetSelectedNamesList() above already applies) trims each name back
    // down to its real length.
    std::vector<std::wstring> names_selected;
    names_selected.reserve(static_cast<size_t>(count));
    for (int i = 0; i < count; ++i) names_selected.emplace_back(ptrs[i]);

    wchar_t dir[1024] = {};
    g_host->GetPanelPath(other, dir, 1024);
    std::vector<PackEntry> add;
    for (const auto& name : names_selected) {
        std::wstring full = JoinPath(dir, name);
        DWORD attrs = GetFileAttributesW(full.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES) continue;
        if (attrs & FILE_ATTRIBUTE_DIRECTORY) {
            CollectRecursive(full, view.folder + name, add);
        } else {
            LARGE_INTEGER s{};
            HANDLE h = CreateFileW(full.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
            if (h != INVALID_HANDLE_VALUE) { GetFileSizeEx(h, &s); CloseHandle(h); }
            add.push_back({full, view.folder + name, attrs, (uint64_t)s.QuadPart});
        }
    }
    if (add.empty()) {
        const wchar_t* lines[] = {L"No accessible files found in the other panel's selection."};
        g_host->ShowMessage(L"Add to archive", lines, 1);
        return;
    }

    auto replacement = view.entries;
    std::set<std::wstring> existingNames;
    for (const auto& e : replacement) existingNames.insert(e.name);
    int added = 0, skipped = 0;
    for (const auto& p : add) {
        if (existingNames.insert(p.archiveName).second) {
            replacement.push_back({p.archiveName, ReadFileBytes(p.sourcePath), p.attributes, 0});
            ++added;
        } else {
            ++skipped;
        }
    }
    // IS-0014: deferred -- saved when leaving the archive, not immediately.
    ApplyArchiveChanges(view, std::move(replacement));
    wchar_t summary[256];
    if (skipped > 0)
        swprintf_s(summary, L"Added %d item(s); %d already present in the archive, skipped. Not yet saved.", added, skipped);
    else
        swprintf_s(summary, L"Added %d item(s) to the archive. Not yet saved.", added);
    const wchar_t* lines[] = {summary};
    g_host->ShowMessage(L"Add to archive", lines, 1);
}

// IS-0011: "Archive info" -- uncompressed size / file count / directory
// count / compression ratio, for either an already-open archive (the Phase
// 3 live-browsing panel) or a .zip file merely selected in a normal panel
// (not entered). This project's own writer (WriteZipArchive/
// WriteArchiveEntries) never stores an explicit directory entry -- a
// directory is only ever implied by the files nested under it (the same
// convention ExtractZipArchive's own comment documents) -- so "number of
// dirs" has to be inferred from every unique directory-path prefix across
// every file entry's own name, not read off a dedicated entry type. A
// third-party-created archive *can* carry explicit directory entries
// (stat.m_is_directory); those are folded into the same prefix set rather
// than double-counted separately.
struct ArchiveStats {
    uint64_t uncompressedBytes = 0;
    uint64_t compressedBytes = 0;
    int fileCount = 0;
    int dirCount = 0;
};

void RegisterDirectoryPrefixes(const std::wstring& normalizedPath, std::set<std::wstring>& dirs) {
    size_t pos = 0;
    while (true) {
        size_t slash = normalizedPath.find(L'/', pos);
        if (slash == std::wstring::npos) break;
        dirs.insert(normalizedPath.substr(0, slash + 1));
        pos = slash + 1;
    }
}

ArchiveStats ComputeArchiveStats(const std::vector<ArchiveEntry>& entries) {
    ArchiveStats stats;
    std::set<std::wstring> dirs;
    for (const auto& e : entries) {
        // IS-0012: LoadArchive now loads an explicit empty-directory marker
        // entry (name ends in '/', zero bytes) as a real ArchiveEntry --
        // count it toward dirCount (via RegisterDirectoryPrefixes below,
        // same as any nested file's own prefix), not fileCount.
        bool isDirectoryMarker = !e.name.empty() && e.name.back() == L'/';
        if (!isDirectoryMarker) {
            ++stats.fileCount;
            stats.uncompressedBytes += e.bytes.size();
            stats.compressedBytes += e.compressedSize;
        }
        RegisterDirectoryPrefixes(e.name, dirs);
    }
    stats.dirCount = static_cast<int>(dirs.size());
    return stats;
}

// Reads `zipPath` fresh from disk (not yet opened as a live ArchiveView) and
// accumulates its stats into `stats` (so a caller can sum several selected
// archives together). Returns false (stats left unmodified) if the file
// can't be read or isn't a valid archive.
bool AccumulateArchiveStatsFromFile(const std::wstring& zipPath, ArchiveStats& stats) {
    std::vector<uint8_t> zipBytes = ReadFileBytes(zipPath);
    if (zipBytes.empty()) return false;
    mz_zip_archive zip{};
    if (!mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) return false;
    std::set<std::wstring> dirs;
    for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&zip); i < count; ++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        std::wstring name = WideFromUtf8(stat.m_filename);
        std::replace(name.begin(), name.end(), L'\\', L'/');
        if (stat.m_is_directory) {
            if (!name.empty() && name.back() != L'/') name += L'/';
            RegisterDirectoryPrefixes(name, dirs);
            continue;
        }
        ++stats.fileCount;
        stats.uncompressedBytes += stat.m_uncomp_size;
        stats.compressedBytes += stat.m_comp_size;
        RegisterDirectoryPrefixes(name, dirs);
    }
    mz_zip_reader_end(&zip);
    stats.dirCount += static_cast<int>(dirs.size());
    return true;
}

std::wstring FormatBytesApprox(uint64_t bytes) {
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 4) { value /= 1024.0; ++unit; }
    wchar_t buf[64];
    if (unit == 0) {
        swprintf_s(buf, L"%llu bytes", static_cast<unsigned long long>(bytes));
    } else {
        const wchar_t* units[] = {L"bytes", L"KB", L"MB", L"GB", L"TB"};
        swprintf_s(buf, L"%.1f %s", value, units[unit]);
    }
    return buf;
}

void ShowArchiveStatsMessage(const std::wstring& title, const ArchiveStats& stats) {
    double ratio = stats.uncompressedBytes > 0
                       ? 100.0 * static_cast<double>(stats.compressedBytes) / static_cast<double>(stats.uncompressedBytes)
                       : 0.0;
    wchar_t countsLine[128];
    swprintf_s(countsLine, L"Files: %d    Directories: %d", stats.fileCount, stats.dirCount);
    std::wstring sizeLine =
        L"Uncompressed: " + FormatBytesApprox(stats.uncompressedBytes) + L"    Compressed: " + FormatBytesApprox(stats.compressedBytes);
    wchar_t ratioLine[128];
    swprintf_s(ratioLine, L"Compression ratio: %.0f%% (%.0f%% saved)", ratio, 100.0 - ratio);
    const wchar_t* lines[] = {title.c_str(), countsLine, sizeLine.c_str(), ratioLine};
    g_host->ShowMessage(L"Archive info", lines, 4);
}

void ShowArchiveInfo() {
    if (!g_host) return;
    McPanelSide activeSide = g_host->GetActiveSide();
    const ArchiveView& activeView = ViewFor(activeSide);
    if (activeView.open) {
        ShowArchiveStatsMessage(activeView.title, ComputeArchiveStats(activeView.entries));
        return;
    }

    std::vector<std::wstring> selected = GetSelectedNamesList();
    wchar_t sourceDirBuf[1024] = {};
    g_host->GetActivePanelPath(sourceDirBuf, 1024);
    std::wstring sourceDir = sourceDirBuf;

    std::vector<std::wstring> zipPaths;
    for (const auto& name : selected) {
        if (!IsZipContainerName(name)) continue;
        std::wstring fullPath = JoinPath(sourceDir, name);
        if (GetFileAttributesW(fullPath.c_str()) != INVALID_FILE_ATTRIBUTES) zipPaths.push_back(fullPath);
    }
    if (zipPaths.empty()) {
        const wchar_t* lines[] = {L"Select one or more ZIP archives, or open one, to see its info."};
        g_host->ShowMessage(L"Archive info", lines, 1);
        return;
    }

    ArchiveStats combined;
    const auto archivesRead = std::count_if(zipPaths.begin(), zipPaths.end(), [&](const auto& path) {
        return AccumulateArchiveStatsFromFile(path, combined);
    });
    if (archivesRead == 0) {
        const wchar_t* lines[] = {L"Could not read the selected archive(s) -- see the log for details."};
        g_host->ShowMessage(L"Archive info", lines, 1);
        return;
    }
    std::wstring title = (archivesRead == 1) ? FileNameOf(zipPaths[0]) : (std::to_wstring(archivesRead) + L" archives");
    ShowArchiveStatsMessage(title, combined);
}
void HandleArchiveKey(McPanelSide side, uint16_t key, uint32_t control) {
    ArchiveView& view = ViewFor(side); if (!view.open) return;
    if (key == VK_BACK) { LeaveOrUp(view); return; }
    // IS-0011: checked before the items-empty early return below, since
    // adding the other panel's selection is exactly what you need when the
    // current folder (or a brand-new archive) has nothing in it yet -- every
    // other key in this function legitimately needs an existing item to act
    // on, but this one doesn't.
    if (key == VK_INSERT && (control & SHIFT_PRESSED)) {
        AddFilesFromPanel(view, side == kMcPanelLeft ? kMcPanelRight : kMcPanelLeft);
        return;
    }
    // IS-0012: same reasoning -- reloading or creating the first directory
    // in an empty archive/folder are both valid even with nothing to act on
    // yet, so both are checked ahead of the items-empty early return too.
    if (key == 'R' && (control & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) { ReloadArchiveFromDisk(view); return; }
    if (key == VK_F7) { CreateDirectoryInArchive(view); return; }
    auto items = CurrentItems(view);
    ClampArchiveCursor(view, (int)items.size());
    if (items.empty()) return;
    if (key == VK_F3) { ViewArchiveFile(view); return; }
    // FEA-0008 (ZIP-011): Shift+F5 copies in place under a new name. Must be
    // checked before the plain-F5 branch below, which ignores modifiers and
    // used to extract the selection to the other panel on Shift+F5 too.
    if (key == VK_F5 && (control & SHIFT_PRESSED)) { CopyWithinArchive(view); ClampArchiveCursor(view, (int)CurrentItems(view).size()); return; }
    if (key == 'A' && (control & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) { ChangeArchiveAttributes(view); return; }
    if (key == VK_ADD && (control & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) {
        for (const auto& item : items) if (item.key != L"..") view.selected.insert(item.key);
        return;
    }
    if (key == VK_SUBTRACT && (control & (LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED))) { view.selected.clear(); return; }
    if (key == VK_MULTIPLY) {
        for (const auto& item : items) if (item.key != L"..") { if (!view.selected.erase(item.key)) view.selected.insert(item.key); }
        return;
    }
    int page = view.visibleRows; if (key == VK_UP) --view.cursor; else if (key == VK_DOWN) ++view.cursor; else if (key == VK_PRIOR) view.cursor -= page; else if (key == VK_NEXT) view.cursor += page; else if (key == VK_HOME) view.cursor = 0; else if (key == VK_END) view.cursor = (int)items.size()-1;
    else if (key == VK_INSERT) { if (items[view.cursor].key != L"..") { if (!view.selected.erase(items[view.cursor].key)) view.selected.insert(items[view.cursor].key); if (view.cursor + 1 < (int)items.size()) ++view.cursor; } }
    else if (key == VK_RETURN) { const auto& item=items[view.cursor]; if(item.key==L"..") LeaveOrUp(view); else if(item.directory){view.folder=item.key;view.cursor=0;} }
    else if (key == VK_BACK) LeaveOrUp(view); else if (key == VK_F8 || key == VK_DELETE) DeleteFromArchive(view); else if (key == VK_F5) ExtractOrMove(view,false); else if (key == VK_F6) ExtractOrMove(view,true);
    ClampArchiveCursor(view, (int)CurrentItems(view).size());
}
void HandleArchiveClick(McPanelSide side, int16_t x, int16_t y, bool doubleClick);
void HandleArchiveMouse(McPanelSide side, int16_t x, int16_t y, uint32_t buttons, uint32_t flags, uint32_t control) {
    auto& view = ViewFor(side);
    if (!view.open) return;
    if (flags == MOUSE_WHEELED) {
        int delta = static_cast<short>(HIWORD(buttons)) > 0 ? -3 : 3;
        view.cursor += delta; ClampArchiveCursor(view, (int)CurrentItems(view).size()); return;
    }
    if (!(buttons & RIGHTMOST_BUTTON_PRESSED)) view.mouseSelecting = false;
    auto items = CurrentItems(view);
    int index = view.topIndex + y - 1;
    if (y < 1 || y > view.visibleRows || index < 0 || index >= (int)items.size()) return;
    bool range = (control & (SHIFT_PRESSED | LEFT_CTRL_PRESSED | RIGHT_CTRL_PRESSED)) != 0;
    if ((buttons & (RIGHTMOST_BUTTON_PRESSED | FROM_LEFT_1ST_BUTTON_PRESSED)) && range && flags == 0) {
        for (int i = std::min(view.cursor, index); i <= std::max(view.cursor, index); ++i)
            if (items[i].key != L"..") view.selected.insert(items[i].key);
        view.cursor = index; return;
    }
    if (buttons & RIGHTMOST_BUTTON_PRESSED) {
        if (!view.mouseSelecting && flags != 0) return;
        if (!view.mouseSelecting) {
            view.mouseSelectValue = !view.selected.count(items[index].key);
            view.mouseSelecting = true;
        }
        if (items[index].key != L"..") {
            if (view.mouseSelectValue) view.selected.insert(items[index].key);
            else view.selected.erase(items[index].key);
        }
        view.cursor = index; return;
    }
    if ((buttons & FROM_LEFT_1ST_BUTTON_PRESSED) && (flags == 0 || flags == DOUBLE_CLICK))
        HandleArchiveClick(side, x, y, flags == DOUBLE_CLICK);
}
void HandleArchiveClick(McPanelSide side, int16_t, int16_t y, bool doubleClick) {
    ArchiveView& view = ViewFor(side);
    if (!view.open || y < 1 || y > view.visibleRows) return;
    int index = view.topIndex + y - 1;
    auto items = CurrentItems(view);
    if (index >= 0 && index < (int)items.size()) {
        view.cursor = index;
        if (doubleClick) HandleArchiveKey(side, VK_RETURN, 0);
    }
}

}  // namespace

extern "C" __declspec(dllexport) const McPluginMetadata* __stdcall McPlugin_GetInfo(void) {
    static const McPluginMetadata info = {
        sizeof(McPluginMetadata),
        MC_PLUGIN_API_VERSION_MAJOR,
        MC_PLUGIN_API_VERSION_MINOR,
        MC_PLUGIN_MAGIC,
        L"com.mycommander.zipplugin",
        L"ZIP Plugin",
        L"ZIP archive support: packing, unpacking, and live archive browsing.",
        MCPLUGIN_VERSION_WSTR,  // IS-0011: one source of truth, also feeds ZipPlugin.rc's file-version resource
        L"MyCommander project",
        kMcPluginCapMenu | kMcPluginCapPanel | kMcPluginCapArchiveHandler | kMcPluginCapHelp,
    };
    return &info;
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_Init(const McPluginHost* host) {
    g_host = host;
    // 'P'/'U' match the original built-in "Pack Files"/"Unpack Package"
    // stubs' own mnemonics, now free since MenuBar.h's stubs were removed
    // (FEA-0007).
    bool packOk = g_host->AddMenuItem(L'F', L"Pack file", L'P', kPackCommandId, kMcMenuPlacementAppend, 0);
    g_host->Log(kMcLogInfo, packOk ? L"\"Pack file\" menu item registered."
                                   : L"\"Pack file\" menu item registration failed.");
    bool unpackOk = g_host->AddMenuItem(L'F', L"Unpack Package", L'U', kUnpackCommandId, kMcMenuPlacementAppend, 0);
    g_host->Log(kMcLogInfo, unpackOk ? L"\"Unpack Package\" menu item registered."
                                     : L"\"Unpack Package\" menu item registration failed.");
    // IS-0011: shows file/dir counts, uncompressed/compressed size, and
    // compression ratio -- for a .zip file merely selected in a normal
    // panel, or for the archive currently open in the active panel.
    bool infoOk = g_host->AddMenuItem(L'F', L"Archive info", L'I', kArchiveInfoCommandId, kMcMenuPlacementAppend, 0);
    g_host->Log(kMcLogInfo, infoOk ? L"\"Archive info\" menu item registered."
                                   : L"\"Archive info\" menu item registration failed.");
    return true;
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_Shutdown(void) {
    // IS-0014: deferred edits are only ever written when leaving an archive
    // (LeaveOrUp) -- if the whole app quits with one still open and dirty,
    // auto-save it here as a safety net rather than silently losing the
    // in-memory changes. Best-effort, no prompt: a dialog during teardown is
    // riskier than it's worth, and g_host is still valid at this point.
    for (const auto& view : g_views) {
        if (view.open && view.dirty) {
            if (WriteArchiveEntries(view.path, view.entries))
                LogInfo(L"Archive browser: auto-saved \"" + view.path + L"\" on shutdown.");
            else
                LogError(L"Archive browser: could not auto-save \"" + view.path + L"\" on shutdown -- unsaved changes may be lost.");
        }
    }
    g_host = nullptr;
}
extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetHelpMarkdown(void) {
    return L"# ZIP panel\n- Enter on a .zip, .jar, .war, .ear, .apk, .xpi, .vsix, .nupkg or .whl file: browse it\n"
           L"- F3: view the current file\n- F5/F6: extract/copy or move selected files to the other panel\n"
           L"- Shift+F5: copy the current item under a new name (or relative path) inside the archive\n"
           L"- F7: create a directory\n- F8 / Delete: delete selected archive entries\n"
           L"- Ctrl+A: toggle archive file attributes\n- Ctrl+R: reload the archive from disk\n"
           L"- Insert: toggle selection\n- Shift+Insert: add the other panel's selection into the archive\n"
           L"- Right-click/right-drag: select files\n"
           L"- Ctrl-click or Shift-click: select a range (Ctrl works in legacy console hosts)\n"
           L"- Backspace: parent folder or confirm leaving the archive\n"
           L"- F5 pressed from the *other* panel (with this archive open) also copies its selection "
           L"into this archive, same as Shift+Insert\n"
           L"- Changes are batched in memory and only written to disk when you leave the archive "
           L"(prompting to save/discard if anything is pending) -- a `*` after \"Name\" in the header "
           L"means there are unsaved changes\n";
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnMenuAction(uint32_t commandId) {
    if (commandId == kPackCommandId) PackSelection();
    else if (commandId == kUnpackCommandId) UnpackSelection();
    else if (commandId == kArchiveInfoCommandId) ShowArchiveInfo();
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_CanHandleFile(const wchar_t* fileName) {
    return fileName && IsZipContainerName(fileName);
}

extern "C" __declspec(dllexport) bool __stdcall McPlugin_EnterArchive(McPanelSide side, const wchar_t* filePath) {
    if (!g_host || !filePath) return false;
    ArchiveView candidate; candidate.open = true; candidate.side = side; candidate.path = filePath; candidate.title = FileNameOf(filePath);
    if (!LoadArchive(candidate.path, candidate)) { LogWarning(L"Archive browser: could not open \"" + candidate.path + L"\"."); return false; }
    if (!g_host->RequestPanelContent(side, candidate.title.c_str())) return false;
    ViewFor(side) = std::move(candidate);
    return true;
}

// IS-0011 follow-up: lets the host's own F5 Copy, pressed while the other
// (real) panel is active and this archive is open on `side`, add the
// selection into the archive instead of the host falling back to copying
// onto the archive's real containing directory (reported bug -- "Copy to
// zip not working," since a panel showing plugin content still reports its
// own real filesystem path underneath, which is what the host's ordinary
// copy targets when nothing intercepts it).
extern "C" __declspec(dllexport) bool __stdcall McPlugin_OnReceiveFiles(McPanelSide side, McPanelSide sourceSide) {
    ArchiveView& view = ViewFor(side);
    if (!view.open) return false;
    AddFilesFromPanel(view, sourceSide);
    return true;
}

extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelDraw(McPanelSide side, uint16_t width, uint16_t height, bool active) {
    DrawArchive(side, width, height, active);
}
extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelKey(McPanelSide side, uint16_t key, uint32_t control) {
    HandleArchiveKey(side, key, control);
}
extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelClick(McPanelSide side, int16_t x, int16_t y, bool doubleClick) {
    HandleArchiveClick(side, x, y, doubleClick);
}
extern "C" __declspec(dllexport) void __stdcall McPlugin_OnPanelMouse(McPanelSide side, int16_t x, int16_t y,
                                                                     uint32_t buttons, uint32_t flags, uint32_t control) {
    HandleArchiveMouse(side, x, y, buttons, flags, control);
}
