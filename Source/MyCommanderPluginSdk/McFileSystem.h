#pragma once

// API 1.12 (EXT-002, D3): file-system providers. A plugin that can present
// something as files and folders (an archive, a server, a cloud store) fills
// one McFsProvider table and hands it to McPluginHost::OpenProvider. The host
// then shows it in its OWN panel -- sorting, the quick filter, Brief view,
// selection, copy/move/delete/rename/mkdir between any two panels, conflict
// prompts, progress, Esc and pause all come from the host. The plugin only
// answers "list this folder", "read/write this file", and so on.
//
// Plain C, like every SDK header: POD structs and __stdcall function pointers.
// Strings crossing the boundary are UTF-16 and owned by whoever passes them;
// the receiver copies what it keeps. Every function gets the table's
// `context` back as its first argument.
//
// Errors: a function that fails writes a short human-readable message into
// `error` (at most errorSize - 1 characters plus a terminator; it may leave it
// empty) and returns the matching McFsStatus. Never put a password or other
// secret into an error message.
//
// Long waits (network): poll McPluginHost::PollCancel and return
// kMcFsCancelled when it says so. The host itself checks Esc between chunks.

#include <stdint.h>
#include <wchar.h>
#ifndef __cplusplus
#include <stdbool.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

typedef enum McFsStatus {
    kMcFsOk = 0,
    kMcFsFailed = 1,
    kMcFsCancelled = 2,
    kMcFsConnectionLost = 3,  // the host offers Retry, which calls Reconnect first
    kMcFsNotSupported = 4,
    kMcFsNotFound = 5,        // nothing exists at that path (Stat uses this to mean "free")
} McFsStatus;

// What the provider can do; the host greys out / refuses the rest.
typedef enum McFsCapability {
    kMcFsCapRead = 1,
    kMcFsCapWrite = 2,
    kMcFsCapMkdir = 4,
    kMcFsCapDelete = 8,
    kMcFsCapRename = 16,
    kMcFsCapAttributes = 32,  // SetAttributes (Ctrl+A toggles read-only/hidden/system/archive)
} McFsCapability;

// One directory entry. Strings are only borrowed for the duration of the
// sink call.
typedef struct McFsEntry {
    uint32_t structSize;
    const wchar_t* name;          // a single name, no separators
    bool isDirectory;
    bool isLink;                  // a symbolic link; with isDirectory, a link to a folder. The host opens it on
                                  // Enter but never descends into it when copying or deleting (SEC-006).
    uint64_t size;
    uint64_t modifiedUtc;         // FILETIME ticks (100 ns since 1601-01-01 UTC), 0 = unknown
    uint32_t attributes;          // Win32 FILE_ATTRIBUTE_* if meaningful (HIDDEN hides it by default), else 0
    const wchar_t* permissions;   // e.g. L"rwxr-xr-x", or NULL
} McFsEntry;

typedef void(__stdcall* McFsEntrySink)(void* sinkContext, const McFsEntry* entry);

// Byte progress of a whole-file transfer (DownloadToFile/UploadFromFile).
// Call it now and then; when it returns false the user cancelled: stop and
// return kMcFsCancelled (after cleaning up any partial file).
typedef bool(__stdcall* McFsProgressFn)(void* progressContext, uint64_t done, uint64_t total);

// Paths are absolute provider paths built from `rootPath` and `separator`:
// with rootPath L"/" and separator L'/', a folder is L"/docs" and a file in it
// L"/docs/a.txt". The host joins and splits paths itself with these two.
typedef struct McFsProvider {
    uint32_t structSize;
    void* context;

    uint32_t capabilities;        // McFsCapability bits
    const wchar_t* displayRoot;   // shown before the path in the panel header, e.g. L"sftp://bob@host"
    const wchar_t* kind;          // short type name for the volume line, e.g. L"sftp", L"zip"
    const wchar_t* rootPath;      // e.g. L"/"
    wchar_t separator;            // e.g. L'/'
    bool caseInsensitive;         // whether two paths differing only in case are the same
    bool confirmOnLeave;          // ask "Leave <displayRoot>?" even with nothing to save (ZIP-007)

    // Calls `sink` once per entry of `dir` (no "." / ".." entries).
    McFsStatus(__stdcall* List)(void* context, const wchar_t* dir, McFsEntrySink sink, void* sinkContext,
                                wchar_t* error, int32_t errorSize);
    // Calls `sink` exactly once with `path`'s own entry, or returns kMcFsNotFound.
    McFsStatus(__stdcall* Stat)(void* context, const wchar_t* path, McFsEntrySink sink, void* sinkContext,
                                wchar_t* error, int32_t errorSize);

    // Reading: Open -> Read until *got == 0 -> CloseRead (always, also after an error).
    McFsStatus(__stdcall* OpenRead)(void* context, const wchar_t* path, void** stream, wchar_t* error,
                                    int32_t errorSize);
    McFsStatus(__stdcall* Read)(void* context, void* stream, void* buffer, uint32_t capacity, uint32_t* got,
                                wchar_t* error, int32_t errorSize);
    void(__stdcall* CloseRead)(void* context, void* stream);

    // Writing: Open -> Write... -> FinishWrite (keep) or AbortWrite (discard). Exactly one of the two ends the
    // stream. Until FinishWrite succeeds, an existing file at `path` must stay untouched, and after AbortWrite
    // nothing partial may remain (FTP-003/SFTP-003).
    McFsStatus(__stdcall* OpenWrite)(void* context, const wchar_t* path, uint64_t sizeHint, uint64_t modifiedUtc,
                                     void** stream, wchar_t* error, int32_t errorSize);
    McFsStatus(__stdcall* Write)(void* context, void* stream, const void* data, uint32_t size, wchar_t* error,
                                 int32_t errorSize);
    McFsStatus(__stdcall* FinishWrite)(void* context, void* stream, wchar_t* error, int32_t errorSize);
    void(__stdcall* AbortWrite)(void* context, void* stream);

    McFsStatus(__stdcall* MakeDirectory)(void* context, const wchar_t* path, wchar_t* error, int32_t errorSize);
    // One file, link, or EMPTY folder; the host deletes trees children first.
    // `isDirectory` is true only for a real folder; a link -- even to a
    // folder -- comes with false and must be removed as itself, never followed.
    McFsStatus(__stdcall* Delete)(void* context, const wchar_t* path, bool isDirectory, wchar_t* error,
                                  int32_t errorSize);
    // Must refuse (kMcFsFailed) when `to` already exists.
    McFsStatus(__stdcall* Rename)(void* context, const wchar_t* from, const wchar_t* to, wchar_t* error,
                                  int32_t errorSize);
    // Needed for kMcFsCapAttributes, else NULL: stores Win32 FILE_ATTRIBUTE_*
    // bits (only READONLY/HIDDEN/SYSTEM/ARCHIVE are ever changed by the host).
    McFsStatus(__stdcall* SetAttributes)(void* context, const wchar_t* path, uint32_t attributes, wchar_t* error,
                                         int32_t errorSize);

    // Optional (may be NULL). Pending changes are saved by Commit; when the
    // panel leaves a provider with HasPendingChanges() true, the host asks
    // "Save changes?" and calls Commit on Yes (ZIP: write the archive).
    bool(__stdcall* HasPendingChanges)(void* context);
    McFsStatus(__stdcall* Commit)(void* context, wchar_t* error, int32_t errorSize);
    // Optional. Re-opens a dropped session (after kMcFsConnectionLost).
    McFsStatus(__stdcall* Reconnect)(void* context, wchar_t* error, int32_t errorSize);
    // Optional. The user asked to re-read the panel (Ctrl+R): drop caches,
    // re-read the source (ZIP: the archive file, asking first if that would
    // discard unsaved changes). The host lists the folder again afterwards.
    McFsStatus(__stdcall* Reload)(void* context, wchar_t* error, int32_t errorSize);

    // Optional pair (both or neither): whole-file transfers, for protocols
    // that move a file in one call rather than in chunks (FTP, SFTP). When
    // present the host uses them whenever the other side is the local disk,
    // with its own progress box, Esc and pause; between two providers it
    // stages through a local temporary file.
    // DownloadToFile writes `path` into the local file `localPath` (created or
    // truncated; the host renames it into place afterwards).
    McFsStatus(__stdcall* DownloadToFile)(void* context, const wchar_t* path, const wchar_t* localPath,
                                          uint64_t sizeHint, McFsProgressFn progress, void* progressContext,
                                          wchar_t* error, int32_t errorSize);
    // UploadFromFile replaces `path` with the local file `localPath` ONLY on
    // success: after a failure or a cancel an existing file at `path` is
    // unchanged and no partial upload remains (upload to a temporary name,
    // then rename it into place).
    McFsStatus(__stdcall* UploadFromFile)(void* context, const wchar_t* localPath, const wchar_t* path,
                                          uint64_t modifiedUtc, McFsProgressFn progress, void* progressContext,
                                          wchar_t* error, int32_t errorSize);

    // Required. The host is done with this provider (the panel left it, the
    // plugin asked to close it, or the host is shutting down): free `context`.
    // Called exactly once, and nothing else is called afterwards.
    void(__stdcall* Close)(void* context);
} McFsProvider;

#ifdef __cplusplus
}  // extern "C"
#endif
