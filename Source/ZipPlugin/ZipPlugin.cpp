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
// Phase 3 (live browsing) has been a file-system provider since EXT-002
// (kMcPluginCapFileSystem, API 1.12): the host draws the archive in its own
// panel; see the "Phase 3 / EXT-002" section below.

#include "../MyCommanderPluginSdk/McPluginAbi.h"
#include "../MyCommanderPluginSdk/McPluginHost.h"
#include "../MyCommanderPluginSdk/McPluginTypes.h"
#include "Version.h"
#include "ZipCrypto.h"
#include "../MyCommander/TextWidth.h"

#include "third_party/miniz/miniz.h"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <memory>
#include <numeric>
#include <cstdlib>
#include <cstring>
#include <set>
#include <string>
#include <vector>

namespace {

const McPluginHost* g_host = nullptr;

// FOP-015 (API 1.11): the per-entry cancel check is also the pause point. The
// host's WaitWhilePaused consumes the Esc that cancels it, so that answer is
// remembered here until the operation's own check has seen it.
bool g_pauseCancelled = false;
bool UserCancelled() {
    if (!g_host) return false;
    if (g_host->WaitWhilePaused && !g_host->WaitWhilePaused()) g_pauseCancelled = true;
    if (g_pauseCancelled) return true;
    return g_host->PollCancel && g_host->PollCancel();
}
class ArchiveProgress {
    bool open_ = false;
public:
    ArchiveProgress(const wchar_t* title, uint64_t total, bool enabled = true) {
        if (enabled) g_pauseCancelled = false;  // FOP-015: a new operation starts clean
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

// EXT-002: Pack / Unpack / Archive info read and write local folders; a
// panel showing an archive or a server isn't one (its path is the local
// folder hidden underneath), so they refuse instead of acting out of sight.
bool RefuseProviderPanels(const wchar_t* title) {
    if (!g_host || !g_host->IsPanelInProvider) return false;
    if (!g_host->IsPanelInProvider(kMcPanelLeft) && !g_host->IsPanelInProvider(kMcPanelRight)) return false;
    const wchar_t* lines[] = {L"This works between local folders.",
                              L"Leave the archive or server panel first, or copy the files out with F5."};
    g_host->ShowMessage(title, lines, 2);
    return true;
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
                      ArchiveProgress& progress, uint64_t& processed, uint64_t total, bool& cancelled,
                      const zipcrypto::Secret* password = nullptr) {
    if (password && !password->Empty()) {
        // ARC-004: an encrypted archive is assembled by ZipBuilder (WinZip AES-256)
        // instead of miniz's writer, which cannot encrypt. The password is only
        // ever handed to ZipBuilder; nothing below formats it into a message.
        zipcrypto::ZipBuilder builder(*password, static_cast<int>(levelAndFlags));
        for (const auto& entry : entries) {
            if (UserCancelled()) { cancelled = true; return false; }
            progress.Update(processed++, total, entry.archiveName);
            std::vector<uint8_t> bytes = ReadFileBytes(entry.sourcePath);
            if (!builder.Add(Utf8ArchiveName(entry.archiveName), bytes.data(), bytes.size(), entry.attributes, false)) {
                LogError(L"Pack file: failed adding \"" + entry.archiveName +
                         L"\" to the encrypted archive (too many entries, or an entry or the archive exceeds 4 GB).");
                return false;
            }
        }
        std::vector<uint8_t> encrypted;
        if (!builder.Finish(encrypted)) {
            LogError(L"Pack file: could not finish the encrypted archive \"" + outputPath + L"\".");
            return false;
        }
        if (!WriteFileBytes(outputPath, encrypted)) {
            LogError(L"Pack file: could not write \"" + outputPath + L"\".");
            return false;
        }
        return true;
    }
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
        if (UserCancelled()) { cancelled = true; ok = false; break; }
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
    if (!g_host || RefuseProviderPanels(L"Pack file")) return;

    const wchar_t* compressionChoices[] = {L"Store", L"Fast", L"Normal", L"Best"};
    McFormField fields[7] = {};

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

    // ARC-004: optional AES-256 password protection. The password lives in
    // these two stack buffers (wiped below) and then in `packPassword`.
    wchar_t passwordBuffer[256] = {}, confirmBuffer[256] = {};
    fields[4].structSize = sizeof(McFormField);
    fields[4].kind = kMcFormFieldCheckbox;
    fields[4].label = L"Encrypt with a password (AES-256)";
    fields[4].boolValue = false;
    fields[5].structSize = sizeof(McFormField);
    fields[5].kind = kMcFormFieldPassword;
    fields[5].label = L"Password";
    fields[5].textBuffer = passwordBuffer;
    fields[5].textBufferSize = 256;
    fields[6].structSize = sizeof(McFormField);
    fields[6].kind = kMcFormFieldPassword;
    fields[6].label = L"Confirm password";
    fields[6].textBuffer = confirmBuffer;
    fields[6].textBufferSize = 256;

    zipcrypto::Secret packPassword;
    bool confirmed = false;
    while (!confirmed) {
        if (!g_host->ShowForm(L"Pack files", fields, 7)) {
            SecureZeroMemory(passwordBuffer, sizeof(passwordBuffer));
            SecureZeroMemory(confirmBuffer, sizeof(confirmBuffer));
            LogInfo(L"Pack file: cancelled (options dialog).");
            return;
        }
        if (!fields[4].boolValue) { confirmed = true; break; }
        if (passwordBuffer[0] == L'\0') {
            const wchar_t* lines[] = {L"Enter a password, or untick \"Encrypt with a password\"."};
            g_host->ShowMessage(L"Pack file", lines, 1);
            continue;
        }
        if (wcscmp(passwordBuffer, confirmBuffer) != 0) {
            const wchar_t* lines[] = {L"The two passwords do not match."};
            g_host->ShowMessage(L"Pack file", lines, 1);
            continue;
        }
        if (wcslen(passwordBuffer) < 8) {
            const wchar_t* lines[] = {L"This password is shorter than 8 characters."};
            McDialogOption options[] = {{L'U', L"Use it anyway"}, {L'C', L"Change it"}};
            if (g_host->ShowChoice(L"Weak password", lines, 1, options, 2) != 0) continue;
        }
        packPassword.Set(passwordBuffer);
        confirmed = true;
    }
    SecureZeroMemory(passwordBuffer, sizeof(passwordBuffer));
    SecureZeroMemory(confirmBuffer, sizeof(confirmBuffer));

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
        if (WriteZipArchive(bins[i], binPath, levelAndFlags, packProgress, processed, totalItems, cancelled,
                            packPassword.Empty() ? nullptr : &packPassword)) {
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

// ARC-004: asks for an archive password in a masked field. false = Esc (or an
// empty password). The text exists only in the wiped stack buffer and in `out`.
bool AskArchivePassword(const std::wstring& archiveName, bool wasWrong, zipcrypto::Secret& out) {
    if (!g_host || !g_host->ShowForm) return false;
    wchar_t buffer[256] = {};
    std::wstring label = (wasWrong ? L"Wrong password, try again: " : L"Password for ") + archiveName;
    McFormField field = {};
    field.structSize = sizeof(McFormField);
    field.kind = kMcFormFieldPassword;
    field.label = label.c_str();
    field.textBuffer = buffer;
    field.textBufferSize = 256;
    bool accepted = g_host->ShowForm(L"Encrypted archive", &field, 1);
    if (accepted) out.Set(buffer);
    SecureZeroMemory(buffer, sizeof(buffer));
    return accepted && !out.Empty();
}

enum class EntryRead { Ok, Cancelled, Failed };

// Reads entry `index` of the in-memory archive, decrypting it when it is
// encrypted. `password` is the archive's cached secret: tried first, asked for
// (three tries at most) when empty or wrong. Never logs the password; the
// entry and archive names are all a log line gets.
EntryRead ReadArchiveEntry(const std::vector<uint8_t>& zipBytes, mz_zip_archive& zip, mz_uint index,
                           const mz_zip_archive_file_stat& stat, const std::wstring& entryName,
                           const std::wstring& archiveName, zipcrypto::Secret& password, std::vector<uint8_t>& out) {
    if (!stat.m_is_encrypted) {
        size_t size = 0;
        void* data = mz_zip_reader_extract_to_heap(&zip, index, &size, 0);
        if (!data) return EntryRead::Failed;
        out.assign(static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
        mz_free(data);
        return EntryRead::Ok;
    }
    zipcrypto::EntryRef ref;
    ref.localHeaderOffset = stat.m_local_header_ofs;
    ref.compressedSize = stat.m_comp_size;
    ref.uncompressedSize = stat.m_uncomp_size;
    ref.crc32 = stat.m_crc32;
    ref.flags = stat.m_bit_flag;
    ref.method = stat.m_method;
    bool wasWrong = false;
    for (int attempt = 0; attempt < 3; ++attempt) {
        if (password.Empty() && !AskArchivePassword(archiveName, wasWrong, password)) return EntryRead::Cancelled;
        switch (zipcrypto::ExtractEncryptedEntry(zipBytes.data(), zipBytes.size(), ref, password, out)) {
            case zipcrypto::Status::Ok:
                return EntryRead::Ok;
            case zipcrypto::Status::WrongPassword:
                password.Clear();
                wasWrong = true;
                break;
            case zipcrypto::Status::Corrupt:
                LogWarning(L"Archive: \"" + entryName + L"\" in \"" + archiveName + L"\" is damaged (authentication failed).");
                return EntryRead::Failed;
            case zipcrypto::Status::Unsupported:
                LogWarning(L"Archive: \"" + entryName + L"\" in \"" + archiveName + L"\" uses an unsupported encryption method.");
                return EntryRead::Failed;
        }
    }
    LogWarning(L"Archive: no valid password given for \"" + archiveName + L"\".");
    return EntryRead::Failed;
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
int ExtractZipArchive(const std::wstring& zipPath, const std::wstring& destDir, const ArchiveProgress& progress,
                       uint64_t& processed, bool& cancelled, zipcrypto::Secret& password) {
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
        if (UserCancelled()) { cancelled = true; break; }
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

        std::vector<uint8_t> bytes;
        size_t archiveSlash = zipPath.find_last_of(L"\\/");
        EntryRead read = ReadArchiveEntry(zipBytes, zip, i, stat, entryName,
                                          archiveSlash == std::wstring::npos ? zipPath : zipPath.substr(archiveSlash + 1),
                                          password, bytes);
        if (read == EntryRead::Cancelled) {
            cancelled = true;  // Esc at the password prompt stops the whole unpack
            break;
        }
        if (read != EntryRead::Ok) {
            LogError(L"Unpack Package: failed extracting \"" + entryName + L"\" from \"" + zipPath + L"\".");
            continue;
        }

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
    if (!g_host || RefuseProviderPanels(L"Unpack Package")) return;

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
    zipcrypto::Secret batchPassword;  // ARC-004: tried on every archive of this batch, wiped when it ends
    for (const auto& zipPath : zipPaths) {
        std::wstring thisDest = baseDest;
        if (zipPaths.size() > 1) {
            size_t slash = zipPath.find_last_of(L"\\/");
            std::wstring fileName = (slash == std::wstring::npos) ? zipPath : zipPath.substr(slash + 1);
            thisDest = JoinPath(baseDest, StripExtension(fileName));
            EnsureDirectoryExists(thisDest);
        }
        int extracted = ExtractZipArchive(zipPath, thisDest, unpackProgress, processed, cancelled, batchPassword);
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

// Phase 3 / EXT-002 (D4): an open archive is a file-system provider
// (McFileSystem.h). The HOST shows it in its own panel -- sorting, the
// filter, Brief view, selection, F3/F5/F6/F7/F8/F2/Shift+F5/Ctrl+A,
// conflicts, progress and Esc are the host's -- and this code answers the
// provider calls over a small in-memory model of the archive's flat central
// directory. Every change only edits the model (ZIP-010); the archive file is
// rewritten once, by Commit, when the host's "Save changes?" prompt on leaving
// says so. Provider paths are "/" + the entry name ("/docs/a.txt" is the
// entry "docs/a.txt"); folders are the "docs/" prefixes (and explicit
// "docs/" marker entries, IS-0012).
struct ArchiveEntry {
    std::wstring name;
    std::vector<uint8_t> bytes;
    uint32_t attributes = 0;
    uint64_t compressedSize = 0;
    uint64_t modifiedUtc = 0;  // FILETIME ticks, 0 = unknown
};
struct ArchiveView {
    McPanelSide side = kMcPanelLeft;
    std::wstring path;   // the .zip on disk (also the panel header's root)
    std::wstring title;  // its file name
    std::vector<ArchiveEntry> entries;
    bool dirty = false;  // IS-0014: unsaved changes in `entries`
    // ARC-004: the archive had encrypted entries, so a save must encrypt again
    // (never silently downgrade it to plain), using `password` -- kept in
    // memory only, wiped when the archive is closed.
    bool encrypted = false;
    zipcrypto::Secret password;
};
std::vector<ArchiveView*> g_archives;  // every archive a panel shows; the host's Close frees one

ArchiveView* ArchiveOnSide(McPanelSide side) {
    if (!g_host || !g_host->GetPanelProvider) return nullptr;
    void* context = g_host->GetPanelProvider(side);
    for (ArchiveView* view : g_archives)
        if (view == context) return view;
    return nullptr;
}

std::wstring FileNameOf(const std::wstring& path) {
    size_t slash = path.find_last_of(L"\\/");
    return slash == std::wstring::npos ? path : path.substr(slash + 1);
}

uint64_t FileTimeFromUnix(MZ_TIME_T t) {
    if (t <= 0) return 0;
    return (static_cast<uint64_t>(t) + 11644473600ull) * 10000000ull;
}
MZ_TIME_T UnixFromFileTime(uint64_t ticks) {
    if (ticks < 116444736000000000ull) return 0;
    return static_cast<MZ_TIME_T>(ticks / 10000000ull - 11644473600ull);
}

bool LoadArchive(const std::wstring& path, ArchiveView& view) {
    ArchiveProgress progress(L"Opening ZIP archive", 0);
    progress.Update(0, 0, L"Reading archive from disk...");
    std::vector<uint8_t> zipBytes = ReadFileBytes(path);
    mz_zip_archive zip{};
    if (zipBytes.empty() || !mz_zip_reader_init_mem(&zip, zipBytes.data(), zipBytes.size(), 0)) return false;
    // ARC-004: entries are collected locally and committed only once the whole
    // archive read (including every password prompt) succeeded, so a cancelled
    // or failed reload leaves the open archive exactly as it was.
    std::vector<ArchiveEntry> loaded;
    zipcrypto::Secret password = std::move(view.password);
    const std::wstring archiveName = FileNameOf(path);
    bool anyEncrypted = false, aborted = false;
    for (mz_uint i = 0, count = mz_zip_reader_get_num_files(&zip); i < count; ++i) {
        progress.Update(i, count, L"Reading archive entries...");
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        std::wstring name = WideFromUtf8(stat.m_filename);
        if (!IsSafeArchiveEntryName(name)) { LogWarning(L"Archive browser: unsafe entry skipped: " + name); continue; }
        if (stat.m_is_directory) {
            // IS-0012: an explicit empty-directory marker entry (name ends in
            // '/', zero bytes) -- kept, so one created via F7 survives a
            // save/close/reopen round trip.
            if (name.empty() || name.back() != L'/') name += L'/';
            loaded.push_back(ArchiveEntry{name, {}, stat.m_external_attr, 0, FileTimeFromUnix(stat.m_time)});
            continue;
        }
        progress.Update(i, count, name);
        std::vector<uint8_t> bytes;
        EntryRead read = ReadArchiveEntry(zipBytes, zip, i, stat, name, archiveName, password, bytes);
        if (stat.m_is_encrypted) anyEncrypted = true;
        // An encrypted entry that can't be read must not be silently dropped:
        // the next save would rewrite the archive without it.
        if (read == EntryRead::Cancelled || (read == EntryRead::Failed && stat.m_is_encrypted)) { aborted = true; break; }
        if (read != EntryRead::Ok) { LogWarning(L"Archive browser: could not read " + name); continue; }
        loaded.push_back(ArchiveEntry{name, std::move(bytes), stat.m_external_attr, stat.m_comp_size,
                                      FileTimeFromUnix(stat.m_time)});
    }
    mz_zip_reader_end(&zip);
    view.password = std::move(password);
    if (aborted) return false;
    view.entries = std::move(loaded);
    view.encrypted = anyEncrypted;
    return true;
}

bool WriteArchiveEntries(const std::wstring& path, const std::vector<ArchiveEntry>& entries, ArchiveProgress* existing = nullptr,
                         const zipcrypto::Secret* password = nullptr) {
    ArchiveProgress ownProgress(L"Saving ZIP archive", entries.size(), existing == nullptr);
    auto& progress = existing ? *existing : ownProgress;
    progress.Update(0, entries.size(), L"Rebuilding archive...");
    if (password && !password->Empty()) {
        // ARC-004: an archive that was encrypted stays encrypted (AES-256).
        zipcrypto::ZipBuilder builder(*password, MZ_DEFAULT_LEVEL);
        size_t done = 0;
        for (const auto& e : entries) {
            progress.Update(done++, entries.size(), L"Encrypting: " + e.name);
            bool directoryMarker = !e.name.empty() && e.name.back() == L'/';
            if (!builder.Add(Utf8ArchiveName(e.name), e.bytes.data(), e.bytes.size(), e.attributes, directoryMarker,
                             e.modifiedUtc)) {
                LogError(L"Archive browser: could not add \"" + e.name + L"\" to the encrypted archive (4 GB / 65,535 entry limit).");
                return false;
            }
        }
        std::vector<uint8_t> encrypted;
        progress.Update(entries.size(), entries.size(), L"Finalizing archive...");
        if (!builder.Finish(encrypted)) return false;
        progress.Update(entries.size(), entries.size(), L"Writing archive to disk...");
        return WriteFileBytes(path, encrypted);
    }
    mz_zip_archive zip{};
    if (!mz_zip_writer_init_heap(&zip, 0, 0)) return false;
    std::vector<uint32_t> attrs;
    bool ok = true;
    for (const auto& e : entries) {
        progress.Update(attrs.size(), entries.size(), L"Compressing: " + e.name);
        std::string name = Utf8ArchiveName(e.name);
        const void* data = e.bytes.empty() ? static_cast<const void*>("") : e.bytes.data();
        MZ_TIME_T modified = UnixFromFileTime(e.modifiedUtc);
        if (!mz_zip_writer_add_mem_ex_v2(&zip, name.c_str(), data, e.bytes.size(), nullptr, 0, MZ_DEFAULT_LEVEL, 0, 0,
                                         modified ? &modified : nullptr, nullptr, 0, nullptr, 0)) {
            ok = false;
            break;
        }
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

// The one place the archive file is rewritten (ZIP-010): the host's Commit,
// after the user chose Save on leaving (or quitting).
bool SaveArchiveIfDirty(ArchiveView& view) {
    if (!view.dirty) return true;
    // ARC-004: never write an encrypted archive back as a plain one. If the
    // password was lost (a failed reload cleared it), ask for it again.
    if (view.encrypted && view.password.Empty() && !AskArchivePassword(view.title, false, view.password)) return false;
    if (!WriteArchiveEntries(view.path, view.entries, nullptr, view.encrypted ? &view.password : nullptr)) return false;
    view.dirty = false;
    return true;
}

// --- names ------------------------------------------------------------------------

bool SameNoCase(const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) == 0; }
bool StartsWithNoCase(const std::wstring& s, const std::wstring& prefix) {
    return s.size() >= prefix.size() && _wcsnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0;
}

// "/docs/a.txt" -> "docs/a.txt"; "/" -> "".
std::wstring KeyOf(const wchar_t* path) {
    std::wstring key = path ? path : L"";
    while (!key.empty() && key.front() == L'/') key.erase(0, 1);
    while (!key.empty() && key.back() == L'/') key.pop_back();
    return key;
}
// The prefix every entry inside folder `key` starts with ("" for the root).
std::wstring PrefixOf(const std::wstring& key) { return key.empty() ? key : key + L"/"; }

int FindFile(const ArchiveView& view, const std::wstring& key) {
    for (size_t i = 0; i < view.entries.size(); ++i)
        if (SameNoCase(view.entries[i].name, key)) return static_cast<int>(i);
    return -1;
}
bool FolderExists(const ArchiveView& view, const std::wstring& key) {
    if (key.empty()) return true;
    std::wstring prefix = PrefixOf(key);
    return std::any_of(view.entries.begin(), view.entries.end(),
                       [&](const ArchiveEntry& e) { return StartsWithNoCase(e.name, prefix); });
}
int FindMarker(const ArchiveView& view, const std::wstring& key) { return FindFile(view, PrefixOf(key)); }

// --- the provider -----------------------------------------------------------------

ArchiveView& A(void* context) { return *static_cast<ArchiveView*>(context); }

void PutError(const std::wstring& text, wchar_t* error, int32_t size) {
    if (error && size > 0) wcsncpy_s(error, static_cast<size_t>(size), text.c_str(), _TRUNCATE);
}

void EmitEntry(McFsEntrySink sink, void* sinkContext, const std::wstring& name, bool directory, uint64_t size,
               uint32_t attributes, uint64_t modified) {
    McFsEntry e{};
    e.structSize = sizeof(McFsEntry);
    e.name = name.c_str();
    e.isDirectory = directory;
    e.size = size;
    e.attributes = directory ? (attributes | FILE_ATTRIBUTE_DIRECTORY) : attributes;
    e.modifiedUtc = modified;
    sink(sinkContext, &e);
}

McFsStatus __stdcall ZipList(void* context, const wchar_t* dir, McFsEntrySink sink, void* sinkContext, wchar_t* error,
                             int32_t errorSize) {
    ArchiveView& view = A(context);
    std::wstring key = KeyOf(dir);
    if (!FolderExists(view, key)) {
        PutError(L"No such folder in the archive: " + key, error, errorSize);
        return kMcFsNotFound;
    }
    std::wstring prefix = PrefixOf(key);
    struct Folder { std::wstring name; uint32_t attributes = 0; uint64_t modified = 0; };
    std::vector<Folder> folders;
    for (const auto& e : view.entries) {
        if (!StartsWithNoCase(e.name, prefix)) continue;
        std::wstring rest = e.name.substr(prefix.size());
        if (rest.empty()) continue;  // this folder's own marker
        size_t slash = rest.find(L'/');
        if (slash == std::wstring::npos) {
            EmitEntry(sink, sinkContext, rest, false, e.bytes.size(), e.attributes, e.modifiedUtc);
            continue;
        }
        std::wstring folder = rest.substr(0, slash);
        auto it = std::find_if(folders.begin(), folders.end(), [&](const Folder& f) { return SameNoCase(f.name, folder); });
        if (it == folders.end()) it = folders.insert(folders.end(), Folder{folder});
        if (slash + 1 == rest.size()) {  // the folder's explicit marker: its attributes and time
            it->attributes = e.attributes & ~static_cast<uint32_t>(FILE_ATTRIBUTE_DIRECTORY);
            it->modified = e.modifiedUtc;
        }
    }
    for (const auto& f : folders) EmitEntry(sink, sinkContext, f.name, true, 0, f.attributes, f.modified);
    return kMcFsOk;
}

McFsStatus __stdcall ZipStat(void* context, const wchar_t* path, McFsEntrySink sink, void* sinkContext, wchar_t* error,
                             int32_t errorSize) {
    ArchiveView& view = A(context);
    std::wstring key = KeyOf(path);
    std::wstring leaf = key.substr(key.find_last_of(L'/') == std::wstring::npos ? 0 : key.find_last_of(L'/') + 1);
    if (key.empty()) {
        EmitEntry(sink, sinkContext, L"/", true, 0, 0, 0);
        return kMcFsOk;
    }
    int file = FindFile(view, key);
    if (file >= 0) {
        const ArchiveEntry& e = view.entries[static_cast<size_t>(file)];
        EmitEntry(sink, sinkContext, leaf, false, e.bytes.size(), e.attributes, e.modifiedUtc);
        return kMcFsOk;
    }
    if (FolderExists(view, key)) {
        int marker = FindMarker(view, key);
        uint32_t attrs = marker >= 0 ? view.entries[static_cast<size_t>(marker)].attributes : 0;
        uint64_t modified = marker >= 0 ? view.entries[static_cast<size_t>(marker)].modifiedUtc : 0;
        EmitEntry(sink, sinkContext, leaf, true, 0, attrs & ~static_cast<uint32_t>(FILE_ATTRIBUTE_DIRECTORY), modified);
        return kMcFsOk;
    }
    PutError(key + L": not in the archive", error, errorSize);
    return kMcFsNotFound;
}

// A read sees a snapshot: the archive may change while a copy inside it runs.
struct ZipReadStream {
    std::vector<uint8_t> bytes;
    size_t offset = 0;
};

McFsStatus __stdcall ZipOpenRead(void* context, const wchar_t* path, void** stream, wchar_t* error, int32_t errorSize) {
    ArchiveView& view = A(context);
    int file = FindFile(view, KeyOf(path));
    if (file < 0) {
        PutError(KeyOf(path) + L": not a file in the archive", error, errorSize);
        return kMcFsNotFound;
    }
    *stream = new ZipReadStream{view.entries[static_cast<size_t>(file)].bytes, 0};
    return kMcFsOk;
}

McFsStatus __stdcall ZipRead(void*, void* stream, void* buffer, uint32_t capacity, uint32_t* got, wchar_t*, int32_t) {
    auto* s = static_cast<ZipReadStream*>(stream);
    size_t n = std::min<size_t>(capacity, s->bytes.size() - s->offset);
    if (n) memcpy(buffer, s->bytes.data() + s->offset, n);
    s->offset += n;
    *got = static_cast<uint32_t>(n);
    return kMcFsOk;
}

void __stdcall ZipCloseRead(void*, void* stream) { delete static_cast<ZipReadStream*>(stream); }

struct ZipWriteStream {
    std::wstring key;
    std::vector<uint8_t> bytes;
    uint64_t modified = 0;
};

McFsStatus __stdcall ZipOpenWrite(void* context, const wchar_t* path, uint64_t sizeHint, uint64_t modifiedUtc,
                                  void** stream, wchar_t* error, int32_t errorSize) {
    ArchiveView& view = A(context);
    std::wstring key = KeyOf(path);
    if (key.empty() || !IsSafeArchiveEntryName(key)) {
        PutError(L"Not a valid name inside the archive.", error, errorSize);
        return kMcFsFailed;
    }
    if (FindFile(view, key) < 0 && FolderExists(view, key)) {
        PutError(key + L": a folder with this name exists", error, errorSize);
        return kMcFsFailed;
    }
    auto* w = new ZipWriteStream{key, {}, modifiedUtc};
    if (sizeHint && sizeHint < (1ull << 31)) w->bytes.reserve(static_cast<size_t>(sizeHint));
    *stream = w;
    return kMcFsOk;
}

McFsStatus __stdcall ZipWrite(void*, void* stream, const void* data, uint32_t size, wchar_t*, int32_t) {
    auto* w = static_cast<ZipWriteStream*>(stream);
    const uint8_t* p = static_cast<const uint8_t*>(data);
    w->bytes.insert(w->bytes.end(), p, p + size);
    return kMcFsOk;
}

McFsStatus __stdcall ZipFinishWrite(void* context, void* stream, wchar_t*, int32_t) {
    std::unique_ptr<ZipWriteStream> w(static_cast<ZipWriteStream*>(stream));
    ArchiveView& view = A(context);
    int existing = FindFile(view, w->key);
    uint64_t modified = w->modified ? w->modified : 0;
    if (existing >= 0) {
        ArchiveEntry& e = view.entries[static_cast<size_t>(existing)];
        e.bytes = std::move(w->bytes);
        e.compressedSize = 0;
        e.modifiedUtc = modified;
    } else {
        view.entries.push_back(ArchiveEntry{w->key, std::move(w->bytes), FILE_ATTRIBUTE_ARCHIVE, 0, modified});
    }
    view.dirty = true;  // IS-0014: saved when leaving the archive
    return kMcFsOk;
}

void __stdcall ZipAbortWrite(void*, void* stream) { delete static_cast<ZipWriteStream*>(stream); }

// IS-0012: F7 adds a real, persisted empty-directory marker entry.
McFsStatus __stdcall ZipMakeDirectory(void* context, const wchar_t* path, wchar_t* error, int32_t errorSize) {
    ArchiveView& view = A(context);
    std::wstring key = KeyOf(path);
    if (key.empty() || !IsSafeArchiveEntryName(key)) {
        PutError(L"Not a valid name inside the archive.", error, errorSize);
        return kMcFsFailed;
    }
    if (FindFile(view, key) >= 0) {
        PutError(L"A file with that name already exists here.", error, errorSize);
        return kMcFsFailed;
    }
    if (FolderExists(view, key)) {
        PutError(L"That folder already exists.", error, errorSize);
        return kMcFsFailed;
    }
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    view.entries.push_back(ArchiveEntry{PrefixOf(key), {}, FILE_ATTRIBUTE_DIRECTORY, 0,
                                        (static_cast<uint64_t>(now.dwHighDateTime) << 32) | now.dwLowDateTime});
    view.dirty = true;
    return kMcFsOk;
}

McFsStatus __stdcall ZipDelete(void* context, const wchar_t* path, bool isDirectory, wchar_t* error, int32_t errorSize) {
    ArchiveView& view = A(context);
    std::wstring key = KeyOf(path);
    if (isDirectory) {
        std::wstring prefix = PrefixOf(key);
        bool hasContent = std::any_of(view.entries.begin(), view.entries.end(), [&](const ArchiveEntry& e) {
            return StartsWithNoCase(e.name, prefix) && !SameNoCase(e.name, prefix);
        });
        if (hasContent) {
            PutError(key + L": the folder is not empty", error, errorSize);
            return kMcFsFailed;
        }
        int marker = FindMarker(view, key);
        if (marker >= 0) {
            view.entries.erase(view.entries.begin() + marker);
            view.dirty = true;
        }
        return kMcFsOk;  // an implied folder vanished with its last entry
    }
    int file = FindFile(view, key);
    if (file < 0) {
        PutError(key + L": not in the archive", error, errorSize);
        return kMcFsNotFound;
    }
    view.entries.erase(view.entries.begin() + file);
    view.dirty = true;
    return kMcFsOk;
}

McFsStatus __stdcall ZipRename(void* context, const wchar_t* from, const wchar_t* to, wchar_t* error, int32_t errorSize) {
    ArchiveView& view = A(context);
    std::wstring a = KeyOf(from), b = KeyOf(to);
    if (b.empty() || !IsSafeArchiveEntryName(b)) {
        PutError(L"Not a valid name inside the archive.", error, errorSize);
        return kMcFsFailed;
    }
    bool caseOnly = SameNoCase(a, b);
    if (!caseOnly && (FindFile(view, b) >= 0 || FolderExists(view, b))) {
        PutError(b + L": already exists", error, errorSize);
        return kMcFsFailed;
    }
    int file = FindFile(view, a);
    if (file >= 0) {
        view.entries[static_cast<size_t>(file)].name = b;
    } else if (FolderExists(view, a)) {
        std::wstring oldPrefix = PrefixOf(a), newPrefix = PrefixOf(b);
        if (StartsWithNoCase(newPrefix, oldPrefix) && !caseOnly) {
            PutError(L"Cannot move a folder into itself.", error, errorSize);
            return kMcFsFailed;
        }
        for (auto& e : view.entries)
            if (StartsWithNoCase(e.name, oldPrefix)) e.name = newPrefix + e.name.substr(oldPrefix.size());
    } else {
        PutError(a + L": not in the archive", error, errorSize);
        return kMcFsNotFound;
    }
    view.dirty = true;
    return kMcFsOk;
}

McFsStatus __stdcall ZipSetAttributes(void* context, const wchar_t* path, uint32_t attributes, wchar_t* error,
                                      int32_t errorSize) {
    ArchiveView& view = A(context);
    std::wstring key = KeyOf(path);
    constexpr uint32_t kSettable =
        FILE_ATTRIBUTE_READONLY | FILE_ATTRIBUTE_HIDDEN | FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_ARCHIVE;
    int index = FindFile(view, key);
    if (index < 0) index = FindMarker(view, key);
    if (index < 0) {
        if (FolderExists(view, key)) return kMcFsOk;  // an implied folder has nowhere to keep attributes
        PutError(key + L": not in the archive", error, errorSize);
        return kMcFsNotFound;
    }
    ArchiveEntry& e = view.entries[static_cast<size_t>(index)];
    e.attributes = (e.attributes & ~kSettable) | (attributes & kSettable);
    view.dirty = true;
    return kMcFsOk;
}

bool __stdcall ZipHasPendingChanges(void* context) { return A(context).dirty; }

McFsStatus __stdcall ZipCommit(void* context, wchar_t* error, int32_t errorSize) {
    ArchiveView& view = A(context);
    if (SaveArchiveIfDirty(view)) {
        LogInfo(L"Archive browser: saved \"" + view.path + L"\".");
        return kMcFsOk;
    }
    PutError(L"Could not save the archive -- it stays open so nothing is lost.", error, errorSize);
    return kMcFsFailed;
}

// ZIP-009 (Ctrl+R): re-read the archive from disk, picking up an outside
// change. A failed reload leaves the open model untouched.
McFsStatus __stdcall ZipReload(void* context, wchar_t* error, int32_t errorSize) {
    ArchiveView& view = A(context);
    if (view.dirty) {
        const wchar_t* lines[] = {L"This archive has unsaved changes -- reloading discards them."};
        McDialogOption options[] = {{L'Y', L"Reload anyway"}, {L'N', L"Cancel"}};
        if (g_host->ShowChoice(L"Refresh", lines, 1, options, 2) != 0) return kMcFsCancelled;
    }
    if (!LoadArchive(view.path, view)) {
        PutError(L"Could not reload the archive -- it may have been deleted or is no longer a valid ZIP file.", error,
                 errorSize);
        return kMcFsFailed;
    }
    view.dirty = false;
    LogInfo(L"Archive browser: reloaded \"" + view.path + L"\" from disk.");
    return kMcFsOk;
}

void __stdcall ZipClose(void* context) {
    ArchiveView* view = static_cast<ArchiveView*>(context);
    if (view->dirty) LogInfo(L"Archive browser: left \"" + view->path + L"\" without saving its changes.");
    view->password.Clear();  // ARC-004: nothing secret outlives the open archive
    g_archives.erase(std::remove(g_archives.begin(), g_archives.end(), view), g_archives.end());
    delete view;
}

McFsProvider MakeArchiveTable(ArchiveView& view) {
    McFsProvider t{};
    t.structSize = sizeof(McFsProvider);
    t.context = &view;
    t.capabilities = kMcFsCapRead | kMcFsCapWrite | kMcFsCapMkdir | kMcFsCapDelete | kMcFsCapRename | kMcFsCapAttributes;
    t.displayRoot = view.path.c_str();
    t.kind = L"zip";
    t.rootPath = L"/";
    t.separator = L'/';
    t.caseInsensitive = true;  // the names land on a Windows file system once extracted
    t.confirmOnLeave = true;   // ZIP-007
    t.List = &ZipList;
    t.Stat = &ZipStat;
    t.OpenRead = &ZipOpenRead;
    t.Read = &ZipRead;
    t.CloseRead = &ZipCloseRead;
    t.OpenWrite = &ZipOpenWrite;
    t.Write = &ZipWrite;
    t.FinishWrite = &ZipFinishWrite;
    t.AbortWrite = &ZipAbortWrite;
    t.MakeDirectory = &ZipMakeDirectory;
    t.Delete = &ZipDelete;
    t.Rename = &ZipRename;
    t.SetAttributes = &ZipSetAttributes;
    t.HasPendingChanges = &ZipHasPendingChanges;
    t.Commit = &ZipCommit;
    t.Reload = &ZipReload;
    t.Close = &ZipClose;
    return t;
}

// ZIP-005: Enter on an archive file opens it in that panel.
bool EnterArchive(McPanelSide side, const std::wstring& filePath) {
    auto view = std::make_unique<ArchiveView>();
    view->side = side;
    view->path = filePath;
    view->title = FileNameOf(filePath);
    if (!LoadArchive(view->path, *view)) {
        LogWarning(L"Archive browser: could not open \"" + view->path + L"\".");
        return false;
    }
    ArchiveView* raw = view.release();
    g_archives.push_back(raw);
    McFsProvider table = MakeArchiveTable(*raw);
    if (!g_host->OpenProvider(side, &table, L"/", raw->title.c_str())) {
        g_archives.erase(std::remove(g_archives.begin(), g_archives.end(), raw), g_archives.end());
        raw->password.Clear();
        delete raw;
        return false;
    }
    return true;
}

// IS-0011: "Archive info" -- uncompressed size / file count / directory
// count / compression ratio, for the archive open in the active panel or the
// .zip files selected in an ordinary panel. Directories are counted from the
// unique folder prefixes of every entry (and explicit markers).
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

// Reads `zipPath` fresh from disk and accumulates its stats into `stats` (so
// several selected archives can be summed). False if it can't be read.
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
    if (const ArchiveView* open = ArchiveOnSide(g_host->GetActiveSide())) {
        ShowArchiveStatsMessage(open->title + (open->dirty ? L" (unsaved changes)" : L""), ComputeArchiveStats(open->entries));
        return;
    }
    if (g_host->IsPanelInProvider && g_host->IsPanelInProvider(g_host->GetActiveSide())) {
        const wchar_t* lines[] = {L"Select one or more ZIP archives in a local folder, or open one, to see its info."};
        g_host->ShowMessage(L"Archive info", lines, 1);
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

}  // namespace

extern "C" __declspec(dllexport) const McPluginMetadata* __stdcall McPlugin_GetInfo(void) {
    static const McPluginMetadata info = {
        sizeof(McPluginMetadata),
        MC_PLUGIN_API_VERSION_MAJOR,
        MC_PLUGIN_API_VERSION_MINOR,  // 1.12: file-system providers (EXT-002)
        MC_PLUGIN_MAGIC,
        L"com.mycommander.zipplugin",
        L"ZIP Plugin",
        L"ZIP archive support: packing, unpacking, and live archive browsing.",
        MCPLUGIN_VERSION_WSTR,  // IS-0011: one source of truth, also feeds ZipPlugin.rc's file-version resource
        L"MyCommander project",
        kMcPluginCapMenu | kMcPluginCapFileSystem | kMcPluginCapArchiveHandler | kMcPluginCapHelp,
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
    // The host closes every open archive (asking about unsaved changes when
    // the user quits) before a plugin shuts down; this only covers a host
    // that didn't (a harness). Nothing is saved here unasked.
    for (ArchiveView* view : g_archives) {
        view->password.Clear();  // ARC-004: nothing secret outlives the plugin
        delete view;
    }
    g_archives.clear();
    g_host = nullptr;
}

extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetHelpMarkdown(void) {
    return L"# ZIP archives\n- Enter on a .zip, .jar, .war, .ear, .apk, .xpi, .vsix, .nupkg or .whl file: browse it in the panel\n"
           L"- Inside an archive every panel feature works as on a folder: sorting (Ctrl+F3..F6), the quick filter\n"
           L"  (Ctrl+F), Brief view, selection, F3 view\n"
           L"- F5 / F6: copy / move to the other panel -- a folder, another archive or a server; F5 / F6 in the\n"
           L"  other panel copies / moves into the archive\n"
           L"- Shift+F5: copy the current item under a new name or a relative path inside the archive\n"
           L"- F7: create a folder; Shift+F7: new empty file; F2: rename; F8: delete; Ctrl+A: toggle attributes\n"
           L"- Ctrl+R: reload the archive from disk\n"
           L"- `..` at the archive's top folder leaves it, asking first; File > Archive info: sizes and counts\n"
           L"- Changes are kept in memory and written to the file only when you leave the archive (or quit),\n"
           L"  after a Save / Don't save / Cancel prompt\n"
           L"- Password-protected archives (WinZip AES, ZipCrypto) ask for the password once, in a masked "
           L"field; Pack file can encrypt with AES-256. An archive that was encrypted is saved encrypted\n";
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
    return EnterArchive(side, filePath);
}
