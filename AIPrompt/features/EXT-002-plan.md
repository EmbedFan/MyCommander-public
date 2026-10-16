# Implementation Plan — EXT-002 (a common file-system provider interface)

Source: `AIPrompt/my-commander.md` §21:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| EXT-002 | Remote and virtual file systems should be representable through a common provider interface. | P2 | LATER | Not Started |

Related:

- EXT-003: the plugin API, Implemented. Its "file-system providers" are today plugin panel content plus
  `OnReceiveFiles`.
- EXT-004/005: SFTP and FTP.
- ARC-001…003: ZIP browsing.
- EXT-006: cloud providers, LATER. It would be the first new user of this interface.

## Implementation check (2.0.0.368)

There are three virtual file systems today, and **each one implements its own panel**:

| Provider | Where | What it re-implements |
|---|---|---|
| ZIP archives | `ZipPlugin.cpp` (1,791 lines): `ArchiveView` with its own entries, cursor, selection, scrolling, drawing, keys and mouse | Listing, columns, selection keys, F5/F6/F7/F8/F2, conflicts, progress |
| FTP and SFTP | `PluginCommon/RemotePanel.cpp` (1,025 lines) over `remote::Client` | The same again, with its own column layout and selection model |
| Directory tree | `DirectoryTreeContent` in `main.cpp` | Not a file system; it's a view. It stays as panel content |

**The consequences:**

- None of the host's panel features work inside these providers:
  - sorting (Ctrl+F3…F6), the quick filter (Ctrl+F), Brief view (planned), hidden-file toggle;
  - compare/sync (SRC-006/007), find files (SRC-001…005), diff (SRC-008);
  - copy as, group rename, file actions, background jobs, the hotlist.
- Copying between two providers (ZIP ↔ SFTP, FTP ↔ SFTP) is impossible.
- Every bug is fixed up to three times.

**What already looks like a provider interface:** `remote::Client` (`PluginCommon/RemoteClient.h`) — List, Download,
Upload, MakeRemoteDirectory, RemoveRemoteDirectory, DeleteRemoteFile, RenameRemote, HomeDirectory, with
`RemoteEntry {name, isDirectory, isLink, size, modifiedUtc, permissions}` and `Status {Ok, Failed, Cancelled,
ConnectionLost}`. The host's `Panel::Entry` carries almost the same fields.

## Design

### D1 — The host interface (`Source/MyCommander/FileProvider.h`)

```cpp
struct ProviderEntry { std::wstring name; bool isDirectory, isLink; uint64_t size, modifiedUtc; uint32_t attributes;
                       std::wstring permissions; };
enum class ProviderStatus { Ok, Failed, Cancelled, ConnectionLost, NotSupported };
enum ProviderCaps : uint32_t { CapRead = 1, CapWrite = 2, CapMkdir = 4, CapDelete = 8, CapRename = 16,
                               CapAttributes = 32, CapServerSideCopy = 64 };
class IFileProvider {
public:
    virtual uint32_t Capabilities() const = 0;
    virtual std::wstring DisplayRoot() const = 0;   // "sftp://bob@host", "C:\a.zip" -- shown in the panel header
    virtual std::wstring Join(const std::wstring& dir, const std::wstring& name) const = 0;  // '/' or '\'
    virtual std::wstring Parent(const std::wstring& path) const = 0;
    virtual bool IsRoot(const std::wstring& path) const = 0;
    virtual ProviderStatus List(const std::wstring& dir, std::vector<ProviderEntry>&, const CancelPoll&, std::wstring& error) = 0;
    virtual ProviderStatus OpenRead(const std::wstring& path, std::unique_ptr<IReadStream>&, std::wstring& error) = 0;
    virtual ProviderStatus OpenWrite(const std::wstring& path, uint64_t sizeHint, std::unique_ptr<IWriteStream>&, std::wstring& error) = 0;
    virtual ProviderStatus MakeDirectory(...) = 0; virtual ProviderStatus Delete(..., bool isDirectory) = 0;
    virtual ProviderStatus Rename(const std::wstring& from, const std::wstring& to, ...) = 0;
    virtual ProviderStatus Commit(const CancelPoll&, std::wstring& error) { return ProviderStatus::Ok; }  // ZIP save-on-leave
    virtual ProviderStatus Reconnect(...) { return ProviderStatus::NotSupported; }
};
```

- **Streams** (`IReadStream::Read(buf, n)`, `IWriteStream::Write` / `Finish` / `Abort`) let the host copy between
  any two providers in 64 KB chunks, with byte progress, cancel and pause (FOP-015).
- **`Finish` / `Abort`** carry the "never leave a partial or destroyed file" rule (FTP-003/SFTP-003): local targets
  write `<name>.mc-part` and rename; remote ones delete the partial upload.
- **The local file system** is a provider too (`LocalProvider`). Local ↔ local keeps the existing `FileOps` engine
  (CopyFileExW fast path, attributes, elevation, Recycle Bin): the generic engine is only used when either side is
  not local.

### D2 — Provider-backed panels (host-drawn)

- **`Panel` gains a provider mode:** `Panel::OpenProvider(std::shared_ptr<IFileProvider>, startPath)` and
  `CloseProvider()`. Its `path_` becomes a provider path string; the local `std::filesystem` path is kept underneath,
  as the plugin-content model does today.
- **Shared code path:** listing, sorting, the filter, selection (Insert, masks, Shift+arrows, the mouse), cursor
  restore, history, the column layout, Brief view and the status line all run through the existing code. `Entry` is
  filled from `ProviderEntry`.
- **Operations** (F5/F6/F7/F8/F2, Shift+F5) dispatch on the panels' providers:

  | Source → target | Engine |
  |---|---|
  | Local → local | `FileOps` (unchanged) |
  | Anything else | `ProviderCopy`: generic, with conflict prompts (`FileOps` `ConflictHandler`), errors, progress, cancel |
  | Same provider with `CapServerSideCopy` / rename | Provider fast path |

- **Leaving:** `..` at the provider root, or Esc, leaves the provider (with ZIP's save confirmation, through
  `Commit`). Disconnecting remote providers works as today.
- **Session:** a provider location is **never** restored automatically. NAV-008 only stores local paths, and
  reconnecting to a server unasked would be a surprise and a security concern. The panel restores the local folder
  underneath.

### D3 — Plugin ABI: `kMcPluginCapFileSystem` (API 1.12, additive)

- **A C function table** (`McFsProvider`, filled by the plugin), mirroring D1:
  - list, with an entry-callback per entry: no allocation across the ABI;
  - open/read/close and open/write/finish/abort with caller-owned buffers;
  - mkdir, delete, rename, commit, reconnect;
  - capability bits;
  - `Join` / `Parent` / `IsRoot`.
- **Opening a provider:** the plugin calls `McHost_OpenProvider(side, table, rootDisplay, startPath)`. It does this
  instead of `RequestPanelContent`, e.g. from Enter on a `.zip` (archive handler) or from its Network menu command.
  The host's `PluginProviderAdapter` wraps the table as `IFileProvider`, with every call SEH-wrapped (PLUG-009).
- **Compatibility:** the existing panel-content API stays for views that aren't file systems (custom plugin UIs,
  the host tree). Old plugins keep working unchanged.

### D4 — Migrations (each one deletes a private panel)

1. **FTP and SFTP:** `RemoteClient` maps almost 1:1 to `McFsProvider`. `PluginCommon/RemotePanel.cpp`'s drawing,
   keys, selection and transfer code is removed. The connection manager, login and host-key prompts and Disconnect
   stay in the plugins.
   - **Gained:** sorting, the filter, Brief view, compare/sync between a server and a local folder, FTP ↔ SFTP copy,
     background jobs to and from servers, find-by-name on servers (recursive listing, cancelable).
2. **ZIP:** archive entries as a provider (writes go to the in-memory model; `Commit` = save on leave, IS-0014). The
   private `ArchiveView` panel is removed. Stats, pack/unpack and the archive info command stay as plugin commands.
3. **Host features** enabled per capability:
   - compare/sync (SRC-006/007) needs `CapRead` on both sides (content compare by streaming);
   - diff (SRC-008) and viewers through a temporary download;
   - file actions on provider files via a temporary copy, read-only.

### D5 — What stays out

- Server-side copy between two different servers (stream through the client instead).
- Mounting a provider as a drive letter.
- EXT-006 cloud providers themselves (they become straightforward once D3 exists).

## Files touched

| File | Change |
|---|---|
| `Source/MyCommander/FileProvider.h`, `LocalProvider.{h,cpp}`, `ProviderCopy.{h,cpp}` (new) + tests | Interface, local provider, generic copy engine |
| `Source/MyCommander/Panel.{h,cpp}` | Provider mode |
| `Source/MyCommander/main.cpp` | Operation dispatch by provider; leave/commit; the status line |
| `Source/MyCommander/PluginManager.{h,cpp}`, `PluginProviderAdapter.{h,cpp}` (new) | ABI adapter, SEH |
| `Source/MyCommanderPluginSdk/*`, `DEVELOPER_GUIDE.md` | API 1.12 `kMcPluginCapFileSystem`, `McFsProvider`, `OpenProvider` |
| `Source/FtpPlugin`, `Source/SftpPlugin`, `Source/PluginCommon` | Become providers; `RemotePanel` UI removed |
| `Source/ZipPlugin` | Becomes a provider; `ArchiveView` panel removed |
| Plugin harnesses | Rewritten against a fake host provider panel |
| Docs, spec (EXT-002 → IN-MVP / Implemented; new PLUG-016), CLAUDE.md, changelog, `Version.h` (MINOR) | Docs |

## Tests

- **`ProviderCopy`** against two in-memory providers (a `FakeProvider` like `FakeRemoteClient`):
  - files, trees, conflicts (overwrite/skip/rename/all), cancel mid-file (no partial target), errors and retry;
  - a dropped connection followed by reconnect;
  - pause (FOP-015).
- **`LocalProvider`:** list, read/write streams and `Finish`/`Abort` on a `TempDir`, with Unicode and long paths.
- **Panel provider mode:** sort, filter, selection, cursor restore and history over a fake provider.
- **`PluginProviderAdapter`:** a fake C table, including SEH on a crashing callback.
- **Regression:** the existing FTP/SFTP/ZIP harness checks rewritten against the provider model. Same coverage:
  browse, transfers, conflicts, links never followed, no secret in logs, zip-slip guard.
- **Manual:**
  - ZIP ↔ SFTP copy;
  - compare a server folder with a local folder;
  - sort and filter on an FTP listing;
  - leaving a modified ZIP asks to save.

## Phasing (each phase shippable)

1. D1 interface, `LocalProvider`, `ProviderCopy` with tests. No user-visible change.
2. D2 panel provider mode, host-internal: an in-memory test provider is behind a debug switch.
3. D3 SDK 1.12 + adapter.
4. D4 migrate FTP/SFTP, then remove `RemotePanel`'s UI.
5. D4 migrate ZIP.
6. Enable compare/sync, diff, find and jobs on providers.

## Risks

- **This is the largest refactor since the plugin system.** The migrations change user-visible behaviour in ZIP,
  FTP and SFTP, mostly by adding features. Each migration phase keeps the old plugin build until its harness parity
  is reached.
- **`Panel` and `main.cpp` assume `std::filesystem::path` widely.** Provider mode must be explicit at every call site
  that touches the disk (`CursorFilePath`, file actions, F4 edit, properties) and refuse cleanly where a local path is
  required.
- **ZIP's write model** (in-memory, save on leave) differs from streaming writes. `IWriteStream` buffers into the
  model, and `Commit` writes.

## Open questions

1. **Scope:** the full plan (all phases, about 6 releases of work)? Or stop after phase 4 (FTP/SFTP), leaving ZIP on
   its own panel?
2. **Host features first:** which should providers get first after migration? Recommended: sort/filter/Brief
   (automatic), then compare/sync, then jobs.
3. **Old API:** keep the panel-content API for non-file-system views (recommended)? Or deprecate it?

## Status

Planned 2026-10-12. **Implemented 2026-10-14, all six phases** (version 2.2.0, spec 1.33.0, plugin API 1.12).

Open questions, answered with the plan's own recommendations: the full scope (ZIP included); the host features
providers get are sort/filter/Brief (automatic), then compare/sync, compare files and find by name; the panel-content
API stays for views that aren't file systems.

What was built, by phase:

1. `FileProvider.h` (`IFileProvider`), `LocalProvider`, `MemoryProvider` (the test double, with fault injection) and
   `ProviderCopy` (copy, move, copy-as, delete, toggle attributes) with `LocalProviderTests.cpp` and
   `ProviderCopyTests.cpp`.
2. `Panel` provider mode (`OpenProvider`/`CloseProvider`/`NavigateProvider`, the local folder kept underneath) and
   `main.cpp`'s dispatch: F5/F6/drag-and-drop/Shift+F5/F7/Shift+F7/F8/F2/Ctrl+A on providers; F3/Enter/Shift+F3/
   Shift+Enter on a fetched, read-only temporary copy (removed at exit); local-only features refuse with one message;
   the debug switch is `MC_DEBUG_MEMORY_PROVIDER=1` plus typing `mem://` in the command line. `PanelProviderTests.cpp`.
3. Plugin API 1.12: `McFileSystem.h` (`McFsProvider`), `McPluginHost::OpenProvider`/`CloseProvider`/
   `GetPanelProvider`/`GetPanelProviderPath`/`IsPanelInProvider`, `kMcPluginCapFileSystem`;
   `PluginProviderAdapter` (SEH-wrapped, `PluginProviderAdapterTests.cpp` with a deliberately crashing table);
   `PluginManager`'s Save / Don't save / Cancel and "Leave ...?" prompts; F10 asks before quitting with unsaved
   provider changes.
4. FTP and SFTP: `PluginCommon/RemotePanel.cpp` is now the connection manager plus a provider (listing cache, uploads
   to a temporary name renamed into place, reconnect offers); its panel UI is gone (1,048 -> 750 lines). Harnesses
   rewritten against the provider, through the host's own adapter and copy engine (FTP panel 33, SFTP panel 37
   checks). They found a real `FtpClient` bug, fixed: after `ABOR` the client read only one of the server's two
   replies, so every later reply was one behind (`DrainAfterAbort`).
5. ZIP: the archive is a provider over the in-memory model (`Commit` = save on leave, `confirmOnLeave` = ZIP-007,
   `Reload` = ZIP-009, `SetAttributes` = Ctrl+A); the `ArchiveView` panel UI is gone (1,990 -> 1,694 lines); entry
   times are now kept on save (`mz_zip_writer_add_mem_ex_v2`, and a time parameter on the AES writer).
   `ZipNavigationTests.cpp` rewritten against the provider, ARC-004 block included.
6. Compare/sync folders (`DirCompare`'s new provider overload: listings and content by streaming), compare files
   (fetched copies), find by name (`SearchProvider`) on providers. `ProviderCompareTests.cpp`.

Deviations from the design, and why:

- **No `Join`/`Parent`/`IsRoot` callbacks in the C table** -- `rootPath` + `separator` (+ `caseInsensitive`) are
  enough for every provider, and the host joins and splits paths itself: fewer calls across the ABI.
- **Whole-file transfers** (`CapFileTransfer`, `DownloadToFile`/`UploadFromFile`) were added because
  `remote::Client` moves whole files: rewriting both protocol clients to stream chunk by chunk, without a real SFTP
  server to test against, was riskier than the gain. The host uses them when one side is local and stages through a
  local temporary file between two providers.
- **Background jobs stay local-only** (FOP-014's Alt+F5 etc. refuse provider panels): jobs run on a worker thread,
  and plugin code is only ever called from the main thread.
- **Find inside a provider is by name only**; a content search would fetch every file.
- **Small additions the migrations needed:** `confirmOnLeave` (ZIP-007, FTP-002/SFTP-002), `Reload` (ZIP-009),
  `SetAttributes`/`kMcFsCapAttributes` (Ctrl+A), `IsPanelInProvider` (Pack/Unpack refuse provider panels instead of
  acting on the hidden local folder).

Not verified here: the interactive UI (no console automation), and FTP/SFTP against real servers (the SFTP write
checks still wait for a writable server -- `SFTP-WRITE-TESTS-plan.md`). Manual test MT-052 covers the in-app checks.
