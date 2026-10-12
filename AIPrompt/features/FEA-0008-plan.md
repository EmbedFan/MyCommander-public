# Feature Plan — FEA-0008 (Shift+F5 "copy as" on file and ZIP panels)

Source: `AIPrompt/additional_feature_requests.md`

> When on file-panel/zip-panel SHIFT/F5 is pressed, a name editor for copy function come up, the
> user type the new name or modify old, then a new copy of the original with the new name shall be
> created.

## Summary

The request has two halves:

1. **File panel** — **already done** (commit 7f8681f, `FOP-019`, version 1.10.1). It shipped
   before this plan as the fix for the user-reported Shift+F5 bug.
   - `DoCopyAs` prompts with the cursor entry's name.
   - The input is resolved by `ResolveCopyAsTarget`: relative goes to the same folder, a full path
     is used as given, and an existing folder or a trailing `\` means "inside, keep the name".
   - The copy goes through `CopyItemAs`.
2. **ZIP panel** — **not done**, and the subject of this plan.

### Current behavior on a ZIP panel

The host sends every key except Tab/F1/F10 straight to the panel's plugin
(`main.cpp:2864`, `target.Content()->OnKey`). The host's new Shift+F5 branch therefore never runs
on an archive panel.

Inside the plugin, `HandleArchiveKey` (`ZipPlugin.cpp:1448`) tests only `key == VK_F5` and ignores
the modifier state. So **Shift+F5 on a ZIP panel currently does the same as F5**: it extracts the
selection to the other panel. That is a silent, wrong action rather than a no-op, and it is the same
class of bug the file-panel half just fixed.

### Archive model (relevant facts)

- `ArchiveView::entries` holds the whole archive in memory as `ArchiveEntry`, with fields `name`,
  `bytes`, `attributes` and `compressedSize`.
  - Names use forward slashes, relative to the archive root.
  - A directory is either implied by any entry under `dir/`, or explicit as a zero-byte `dir/`
    marker entry (IS-0012).
- Since IS-0014 (`ZIP-010`), every edit goes through `ApplyArchiveChanges(view, replacement)`.
  - This replaces `entries` and sets `dirty`.
  - The real repack-and-write happens only once, when the user leaves the archive (with a
    save/discard/cancel confirmation).
- A copy inside the archive is therefore just *duplicating entries in memory under a new name* — no
  disk I/O, no progress dialog, and it is saved together with every other pending edit.
- `IsSafeArchiveEntryName` (`ZipPlugin.cpp:253`) already rejects empty names, a leading `/`, any
  `:` (drive letters) and `..` segments. This is the zip-slip guard. It's reused here, so a typed
  name can never escape the archive.

## Part 1 — Stop Shift+F5 extracting

### Design

In `HandleArchiveKey`, route `VK_F5` with `SHIFT_PRESSED` to a new `CopyWithinArchive(view)` before
the existing plain-F5 branch. F5 and F6 without Shift are unchanged. Shift+F6 is untouched (no
"move as" is asked for; it keeps its current meaning).

### Files touched

- `Source/ZipPlugin/ZipPlugin.cpp` (`HandleArchiveKey`).

## Part 2 — `CopyWithinArchive` (the actual feature)

### Behavior, mirroring the file-panel half

- Acts on the **cursor entry** only, like the host's Shift+F5 and F2. It does nothing on `..`.
- Opens `g_host->ShowTextPrompt(L"Copy \"<name>\" as:", <name>, ...)`, pre-filled with the entry's
  own label (the name within the current folder).
  - This prompt is the host's `ShowTextPrompt`, so it automatically gets IS-0016's visible,
    scrolling cursor.
- Resolves the input with a new pure helper (below). The input is relative to the **current archive
  folder** (`view.folder`):
  - **A name** → a sibling copy in the current folder.
  - **A relative path** (`sub/new.txt`, with `\` accepted and normalized to `/`) → that path under
    the current folder. Missing intermediate folders are simply implied, as ZIP folders always are.
  - **An existing archive folder, or input ending in `/` or `\`** → the copy goes inside it, keeping
    the original name. This is the same rule as `ResolveCopyAsTarget`.
  - **Unsafe input** (`..` segment, leading slash, drive letter / `:`) → message "Enter a name or a
    path inside the archive", nothing changes.
    - A full disk path (`C:\x\y.txt`) is therefore refused rather than extracted. Copying out of an
      archive is already what F5 does; see Open questions.
  - **Same as the original** → message "The new name is the same as the original.", nothing
    changes.
- **Copying a file:** append one new `ArchiveEntry` with the same `bytes` and `attributes`, the new
  name, and `compressedSize = 0` (unknown until the next save, as for entries added by
  Shift+Insert).
- **Copying a folder** (`key` ends in `/`):
  - duplicate every entry whose name starts with `src/` (marker included) under `dst/`, keeping the
    same relative remainder;
  - refuse a target inside the source itself (`dir` → `dir/sub/dir2`), with the same "cannot place
    a directory inside itself" message as the host.
- **Conflicts** (one or more target names already exist):
  - Ask once with `ShowChoice`: **Overwrite** / **Skip existing** / **Cancel**. This is a smaller set
    than the host's full conflict dialog: there is no per-item rename, since the user has just typed
    the name.
  - Overwrite replaces the bytes and attributes of the existing entries.
  - A file-versus-folder clash (copying a file onto a name that's a folder, or the reverse) is
    refused with a message, as the host's `CheckDestinationSafety` does.
- **Applying the copy:**
  - `ApplyArchiveChanges(view, replacement)` marks the archive dirty. Nothing is written until the
    user leaves (`ZIP-010`), and the `Name *` header marker shows there are pending changes.
  - The cursor lands on the new copy when it is in the current folder (IS-0001 convention); the
    selection is left alone.
  - A `LogInfo` line notes the copy as "(not yet saved)", matching the other deferred edits.

### Pure helpers (testable, no host calls)

```cpp
// Normalizes `typed` ("\"->"/", trims spaces/quotes) and resolves it against `folder`
// for a copy of `sourceKey` (a file "a/b.txt" or a folder "a/b/"). Empty `error` on success.
struct ArchiveCopyTarget { std::wstring targetKey; std::wstring error; };
ArchiveCopyTarget ResolveArchiveCopyTarget(const std::vector<ArchiveEntry>& entries,
                                           const std::wstring& folder,
                                           const std::wstring& sourceKey,
                                           const std::wstring& typed);

// The (sourceName -> targetName) entry pairs a copy of sourceKey to targetKey produces:
// one pair for a file, every entry under the prefix for a folder.
std::vector<std::pair<std::wstring, std::wstring>> PlanArchiveCopy(
    const std::vector<ArchiveEntry>& entries, const std::wstring& sourceKey,
    const std::wstring& targetKey);
```

`CopyWithinArchive` uses these as follows:

1. prompt;
2. resolve;
3. plan;
4. detect conflicts against `entries`;
5. ask the conflict question if needed;
6. build `replacement`;
7. `ApplyArchiveChanges`.

### Files touched

- `Source/ZipPlugin/ZipPlugin.cpp`: the two helpers, `CopyWithinArchive`, and key routing.
- `Source/ZipPlugin/ZipPlugin.cpp` Help Markdown (`McPlugin_GetHelpMarkdown`, around line 1550):
  add `- Shift+F5: copy the current item under a new name inside the archive`.
- `Source/ZipPlugin/Version.h`: plugin MINOR bump, 0.10 → 0.11 (a new user-visible feature).

No plugin SDK or host change is needed. `ShowTextPrompt`, `ShowChoice` and `ShowMessage` are all
already in `McPluginHost`.

## Part 3 — Tests

`Source/ZipPlugin/ZipNavigationTests.cpp` is the existing standalone harness. It `#include`s
`ZipPlugin.cpp` with a mock `McPluginHost`. Extend it as follows:

- **Pure helper cases:**
  - bare name → sibling key;
  - `sub\x.txt` → `folder/sub/x.txt`;
  - existing folder / trailing slash → inside, keeping the name;
  - `..`, `/abs` and `C:\x` → error;
  - the unchanged name → error;
  - folder into itself → error;
  - `PlanArchiveCopy` for a folder copies every nested entry including its `dir/` marker.
- **End-to-end through `McPlugin_OnPanelKey(side, VK_F5, SHIFT_PRESSED)`:**
  - add a mock `ShowTextPrompt` to the harness host that returns a scripted string;
  - after the call, the new entry exists with identical bytes and attributes;
  - the original is untouched;
  - `dirty` is set (the header shows `Name *`);
  - the other panel's path received no extraction: the `destination` directory stays empty.
    This is the regression guard for Part 1.
- **Conflict:**
  - a scripted `Choose` answer of Overwrite replaces the bytes;
  - Cancel leaves `entries` identical.
- **Round trip:** leave the archive with Save, reopen it, and check the copy persisted.

## Docs, spec, version

- `AIPrompt/my-commander.md`:
  - new row **`ZIP-011`**: "While browsing an archive, `Shift+F5` shall copy the cursor entry under
    a new name or relative path inside the archive (deferred like other archive edits, `ZIP-010`);
    input that would leave the archive is refused." Mark it Implemented once done;
  - add a history note.
  - `FOP-019` already covers the file-panel half.
- `AIPrompt/additional_feature_requests.md`: untouched (the user's own file).
- `README.md`:
  - the ZIP section, if it lists panel keys, gets Shift+F5;
  - the main keybindings row already describes the file-panel half.
- `Help.cpp`: nothing. The ZIP plugin's own Help Markdown carries its keys.
- Host `Version.h`: no change unless the build is shipped together; the plugin version bump is
  enough.
- Changelog entry for the day it's implemented.

## Risk / scope notes

- **Low risk.** All changes are inside the ZIP plugin, purely in memory. They go through the
  existing deferred-save path, so a mistaken copy is undone by "Discard" when leaving the archive.
- **Memory.** Copying a large folder duplicates its bytes in memory until the save. This is the same
  trade-off the in-memory archive model already makes everywhere (`LoadArchive` holds every entry's
  bytes). It's acceptable, and no new limit is introduced.
- **Out of scope:**
  - Shift+F6 "move/rename as" inside the archive (a rename isn't requested; it would be a natural
    follow-up using the same helpers);
  - multi-selection copy-as with a name mask (TC's `*.bak` style);
  - copying from an archive to a disk path via Shift+F5.

## Open questions

1. **Full disk path in the ZIP Shift+F5 prompt.** The request says "copy with the new name". The plan
   refuses a disk path (`C:\...`) with a message, because extracting is F5's job and a
   within-archive copy is the literal ask.
   - The alternative is to extract that one entry to the typed disk path, mirroring the file
     panel's "full path is used as given".
   - This is easy to add with the existing extraction code if you prefer it.
2. **Shift+F5 with several items selected.** The plan copies only the cursor entry, as on the file
   panel, and ignores the selection.

## Status

Implemented 2026-10-05 as planned (ZipPlugin 0.11.0, `ZIP-011`); the file-panel half shipped earlier
in 7f8681f as `FOP-019`. Both open questions were settled by the plan's defaults: a full disk path is
refused, and only the cursor item is copied. One addition: `ZipNavigationTests.cpp`'s three
pre-existing on-disk checks had gone stale since IS-0014's deferred saves (they failed before this
change too) and now call `SaveArchiveIfDirty` before reading the archive back.
