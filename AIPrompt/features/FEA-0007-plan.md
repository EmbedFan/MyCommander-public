# Feature Plan — FEA-0007 (ZIP functionality)

Source: `AIPrompt/additional_feature_requests.md`

> A ZIP plugin is required. The menu "Pack file" shall be removed from the application startup.
> This shall be put onto the File menu "Pack file". It shall have a dialog with selectable
> options for: compression rate; with subdirectories (checked by default); Enable output >
> general max size for zips; for huge output multi file compression enabled. All files in the zip
> shall keep their own attributes. When press enter on a zip file: step into the zip file and
> shown as a directory; support standard panel functions (select, delete, move, copy to, copy
> from); support cursor moving in level, into dir, out from dir; when user enters `..` that exits
> from zip, please ask a confirmation. Review all of this requirements and add ones that are
> generally useful and used in other software.

## Summary

This is the largest single feature requested of this project so far — larger than FEA-0002 (MC
Plugin) itself, which it depends on directly. It asks for three genuinely different things bundled
into one request:

1. **A ZIP *creation* tool** — a packing dialog with real options (compression level,
   recursion, a size cap, multi-volume splitting), reachable from the File menu, replacing the
   `Pack Files` stub that has existed since FEA-0005.
2. **Attribute-preserving archives** — every packed entry keeps its Win32 file attributes.
3. **Live, read/write ZIP *browsing*** — treating a `.zip` file as if it were a real directory:
   `Enter` steps into it, the panel shows its contents, selection/delete/copy-in/copy-out all
   work, nested directories inside the archive are navigable, and leaving the archive (`..` at its
   own root) asks for confirmation first.

Per the request's own closing instruction, this plan also folds in the one clearly-implied,
"generally useful" counterpart the request itself doesn't spell out: **unpacking** (extracting
from a `.zip`) — the File menu already has an `Unpack Package` stub sitting right next to `Pack
Files`, asking for exactly the same treatment, and no archive tool ships one half without the
other. A short list of further, explicitly-`LATER` extras (password protection, integrity
testing, nested-archive browsing) is proposed at the end rather than silently expanded into scope.

**The central architectural question this plan has to answer first is: does "browse a `.zip` like
a directory with standard panel functions" mean *literally reusing* `Panel`/`FileOps`'s own
code against a non-filesystem backend, or can it be built *as a plugin*, on top of what FEA-0002
already shipped?** The answer matters enormously for cost and risk — see **Design, §0** below for
the full reasoning, but the short version: **as a plugin**, using FEA-0002 Phase 3's existing
`kMcPluginCapPanel` (panel content) machinery plus one small, new, equally generic capability this
plan proposes (`kMcPluginCapArchiveHandler`). This keeps `Panel.h`/`Panel.cpp`/`FileOps.h`/`.cpp` —
the most heavily-depended-on, already-tested core of the app — completely untouched, at the cost of
the ZIP plugin re-implementing its own entry-list rendering rather than reusing `Panel`'s. The
alternative (a true storage-backend abstraction inside `Panel` itself, which is what `EXT-002`
already envisions — "remote and virtual file systems... through a common provider interface") is
the architecturally "proper" long-term answer, but is a far larger, riskier, core-engine refactor
this plan deliberately does not propose attempting as part of FEA-0007. `EXT-003` already
anticipates exactly this trade-off ("a public plug-in API may later support... archive handlers"),
so building this as a plugin capability is squarely in-scope for what the spec already expects of
the plugin architecture, not a workaround.

## Current state (what exists today, and why none of it is a ZIP seam yet)

- `MenuBar.h`'s File menu has two relevant stubs, inert since FEA-0005: `stub(L"Pack Files", L'P')`
  and `stub(L"Unpack Package", L'U')` — selecting either just shows "Not yet implemented."
- `Panel.h`/`Panel.cpp` enumerate exclusively via `std::filesystem::directory_iterator` and raw
  Win32 calls (`GetAttributesOrInvalid`, `GetVolumeInformationW`, ...) — there is no seam anywhere
  in `Refresh`/`BeginRefresh`/`EnterSelected`/`NavigateTo`/`GoToParent` for a non-real location.
  `Panel::EnterSelected()` specifically requires `fs::is_directory()` to succeed before navigating;
  a file (including a `.zip`) never reaches it at all — `main.cpp`'s `ActivateCursorEntry` (the
  actual `Enter`-key handler, ~line 1773) branches to View/Edit/Execute for any cursor entry
  `CursorFilePath` identifies as a file, and only falls through to `Panel::EnterSelected()` for a
  directory/`..`. **This is the exact, single point where a new hook is needed** for "press Enter
  on a zip file" to mean anything other than View/Edit/Execute.
- `FileOps.h`/`.cpp` — every operation (`CopyItems`, `MoveItems`, `DeleteItems`,
  `ToggleAttributes`, ...) takes a `std::filesystem::path` directory plus item names and calls
  `CopyFileExW`/`MoveFileWithProgressW`/`SHFileOperationW`/`DeleteFileW` directly. Nothing here
  can be redirected at a non-filesystem destination without a genuine abstraction layer (see
  Design §0).
- FEA-0002's MC Plugin SDK (`Source/MyCommanderPluginSdk/`), as of Phase 3, already provides
  everything a ZIP plugin needs *except* the "claim this file as a virtual directory" hook and a
  richer-than-single-field options dialog:
  - `kMcPluginCapMenu`/`AddMenuItem` — a plugin can already put its own "Pack file" item on the
    File menu.
  - `kMcPluginCapPanel`/`RequestPanelContent`/`PanelDrawText`/`OnPanelDraw`/`OnPanelKey`/
    `OnPanelClick` (Phase 3, §8) — a plugin can already take over a panel's own drawing and input
    entirely. This is the mechanism ZIP browsing will use (Design §2).
  - `ShowChoice`/`ShowMessage`/`ShowTextPrompt` — enough for the `..`-exits-the-archive
    confirmation, but **not** enough for a multi-field packing-options dialog (checkbox +
    numeric + choice, all in one modal) — see Design §1.
  - `GetActivePanelPath`/`GetSelectedNames` — read-only, and scoped to whichever panel is
    *currently active*, not addressable by side — insufficient for "extract into the *other*
    panel's directory" when the archive view itself is the active panel (Design §3).
  - `Log`/`GetTheme`/theme-change notification — already sufficient as-is.

## Design

### §0. Why a plugin, not a `Panel`/`FileOps` refactor

`EXT-002` ("remote and virtual file systems... through a common provider interface") describes the
architecturally complete answer: give `Panel` a pluggable enumeration/IO backend, so
`Refresh`/`EnterSelected`/`GoToParent` and every `FileOps` entry point work identically whether the
current location is a real directory or something else (an archive, eventually an FTP/SFTP root,
per `EXT-004`/`EXT-005`). That is the *right* long-term design — but it means touching the single
most depended-upon, most heavily-tested class in the codebase (`Panel`) and its file-operation
counterpart (`FileOps`), both currently 100% filesystem-coupled by design, for a feature that only
one plugin needs today. That is a disproportionate amount of core-engine risk to take on as part of
delivering ZIP support specifically.

The alternative this plan proposes instead: the ZIP plugin uses FEA-0002 Phase 3's existing
`IPanelContent`/`kMcPluginCapPanel` mechanism to take over a panel's drawing and input *entirely*
while an archive is open, drawing its own entry list (name/size/compressed-size/attributes columns,
matching the app's own visual conventions per §8's own requirement) and handling its own
navigation keys. "Standard panel functions" (select, delete, move, copy to, copy from) are
satisfied by the plugin *behaving the same way from the user's perspective*, not by literally
calling `Panel::ToggleCursorSelection`/`FileOps::DeleteItems` against archive entries — the plugin
has its own entry list, its own selection marks, and its own archive-aware delete/extract/add
logic (reusing real `FileOps`/Win32 calls only for the real-filesystem side of an extract or add,
which is ordinary file I/O the plugin can already do in-process, no new host API required for that
part). This costs the ZIP plugin some real implementation work re-deriving a small slice of what
`Panel` already does (column layout, cursor highlight, selection marker) — but it's additive,
bounded, reversible (deleting the plugin removes the feature entirely), and risks nothing in the
core engine. **Recommendation: build FEA-0007 this way; treat a real `EXT-002` provider
abstraction as a separate, future, much larger undertaking if ever pursued.**

### §1. New SDK prerequisite: a generic multi-field options dialog

The packing dialog needs four fields in one modal (compression level, a subdirectories checkbox, a
numeric max-size, a multi-volume checkbox) — `ShowChoice`/`ShowTextPrompt` each handle exactly one
concern. Rather than a one-off, ZIP-specific dialog bolted onto the host, add one small, genuinely
reusable primitive any future plugin can use:

```c
typedef enum McFormFieldKind {
    kMcFormFieldCheckbox,
    kMcFormFieldNumber,
    kMcFormFieldChoice,   // cycles through McFormField::choices on Left/Right, like a combo box
    kMcFormFieldText,
} McFormFieldKind;

typedef struct McFormField {
    McFormFieldKind kind;
    const wchar_t* label;
    bool boolValue;               // kMcFormFieldCheckbox: in/out
    int64_t numberValue;          // kMcFormFieldNumber: in/out
    int64_t numberMin, numberMax; // kMcFormFieldNumber: clamps
    int choiceIndex;              // kMcFormFieldChoice: in/out, index into choices
    const wchar_t* const* choices;
    int choiceCount;
    wchar_t* textBuffer;          // kMcFormFieldText: in/out, truncate-safe like ShowTextPrompt
    int textBufferSize;
} McFormField;

// Returns false if the user pressed Esc (fields left however the user had them at that point,
// caller should not trust them); true if Enter/an explicit OK was chosen, with every field's
// value updated in place.
typedef bool (__stdcall* McHost_ShowFormFn)(const wchar_t* title, McFormField* fields, int fieldCount);
```

Host-side: a new `Dialog.h`/`Dialog.cpp` function, `ShowFormDialog`, built the same way
`ShowChoiceDialog` already is (keyboard-only: `Up`/`Down` moves between fields, `Left`/`Right`
toggles a checkbox or cycles a choice field, typing edits a number/text field, `Enter` confirms,
`Esc` cancels) — this is new host UI code, not a plugin-side reimplementation, so it automatically
follows the app's own visual conventions (§8/§9's own requirement) the same way `ShowChoice`
already does. `McHost_ShowFormFn` is appended to `McPluginHost` (another additive,
minor-version-bump field, same discipline as every prior addition).

### §2. New SDK prerequisite: claiming a file as a virtual directory

```c
typedef enum McPluginCapability {
    ... // existing bits unchanged
    kMcPluginCapArchiveHandler = 1 << 5,  // can claim a file as a virtual, navigable directory
} McPluginCapability;

// Required if kMcPluginCapArchiveHandler is set. Fast, name-only check (extension sniffing is
// enough for ZIP; a plugin wanting content-sniffing can open and inspect the file itself here) --
// called for every file the cursor could land on, so must be cheap. Not called for a directory
// entry (those are never candidates) or while another plugin's content already owns this side
// (RequestPanelContent would simply fail if this plugin tried to claim it anyway).
typedef bool (__stdcall* McPlugin_CanHandleFileFn)(const wchar_t* fileName);

// Called once, when the host decides this plugin should take over: the user pressed Enter on a
// file this plugin's own CanHandleFile returned true for, and Config::enterFileAction's View/
// Edit/Execute choice is *not* consulted in that case -- entering a claimed file always means
// "browse it," never view/edit/execute, the same way Enter on an ordinary directory means
// "navigate," never either of those. The plugin is expected to call host->RequestPanelContent(side, ...)
// itself from inside this call (exactly as it would from a menu command or Init) and return
// whether it actually took over the panel; false here means "declined after all" (e.g.
// RequestPanelContent's own conflict check failed) and the host falls back to today's normal
// View/Edit/Execute/EnterSelected handling for that entry instead -- never a dead keypress.
typedef bool (__stdcall* McPlugin_EnterArchiveFn)(McPanelSide side, const wchar_t* filePath);
```

Host-side hook: `main.cpp`'s `ActivateCursorEntry` (~line 1773) gains one new check, before its
existing `CursorFilePath(target)` branch:

```cpp
void ActivateCursorEntry(Console& console, Panel& left, Panel& right, bool leftActive,
                         const Config& config, Logger& logger, PluginManager& pluginManager,
                         std::vector<PendingEdit>& pendingEdits) {
    Panel& target = leftActive ? left : right;
    if (auto filePath = CursorFilePath(target)) {
        if (pluginManager.TryEnterAsArchive(leftActive ? kMcPanelLeft : kMcPanelRight, *filePath)) return;
        switch (config.enterFileAction) { /* unchanged View/Edit/Execute */ }
    } else {
        target.EnterSelected();
    }
}
```

`PluginManager::TryEnterAsArchive(side, filePath)` asks every loaded `kMcPluginCapArchiveHandler`
plugin's `CanHandleFile(filename)` in load order, stopping at the first `true`, then calls that
plugin's `EnterArchive(side, filePath)` (SEH-wrapped, like every other call into plugin code) and
returns its result. Exactly one plugin can claim a given file — the first match wins, matching
`RequestPanelContent`'s own "first owner wins" rule for a side. No behavior change at all when no
loaded plugin declares this capability (the check is a cheap no-op loop over an empty/non-matching
set), so an app with no archive plugin installed behaves exactly as today.

### §3. New SDK prerequisite: side-scoped panel queries and a refresh request

`GetActivePanelPath`/`GetSelectedNames` only ever describe *the currently active* panel — useless
for "extract into the *other* panel's directory" once the archive view itself is the active one.
Add side-scoped siblings (the existing active-panel versions stay exactly as they are — additive,
not a replacement):

```c
typedef void (__stdcall* McHost_GetPanelPathFn)(McPanelSide side, wchar_t* outBuffer, int outBufferSize);
typedef int (__stdcall* McHost_GetPanelSelectedNamesFn)(McPanelSide side, wchar_t** outNames, int outCount,
                                                         int maxNameLength);
// After the plugin writes real files into a panel's own directory (e.g. extracting into it), the
// host's own Panel for that side has stale entries_ until something re-lists it -- normally only
// ever triggered by the host's own navigation code. This lets a plugin ask for that refresh
// explicitly rather than the user needing to manually leave and re-enter the directory.
typedef void (__stdcall* McHost_RefreshPanelFn)(McPanelSide side);
```

All three are thin wrappers reusing exactly what `Panel`/`FileOps` already expose
(`Panel::Path()`/`SelectionOrCursor()`/`Refresh()`) — no new `Panel` logic, just addressing it by
side instead of implicitly "whichever is active."

### §4. ZIP file format and the compression library (needs your confirmation before implementation)

Nothing in this codebase reads or writes the ZIP container format or does DEFLATE compression
today, and implementing DEFLATE from scratch is reinventing a large, well-solved, easy-to-get-
subtly-wrong algorithm — the same class of judgment call this project already made for
Authenticode verification (`WinVerifyTrust`, not a hand-rolled signature check) and the config
format (DEC-007 rejected a hand-rolled TOML/JSON parser in favor of something smaller, not the
other way around — the inverse lesson applies here: don't hand-roll what already has a tiny,
proven, freely-licensed answer).

**Recommendation: vendor [miniz](https://github.com/richgel999/miniz)** — a single `.c`/`.h` pair,
**MIT-licensed** (confirmed against the current repository; earlier, older copies of miniz
circulated as public domain, but the actively-maintained fork this plan points at is MIT),
implementing DEFLATE compress/decompress and full ZIP container read/write (including per-entry
attribute storage). It compiles as an ordinary source file inside the ZIP plugin's own standalone
`.vcxproj` (matching `ExamplePlugin`'s own "not part of `MyCommander.sln`" precedent) — no package
manager (NuGet/vcpkg), no build-system change, no dependency DEC-009 didn't already allow (DEC-009
decided the *build system*, not a ban on ever vendoring a small, permissively-licensed source file;
DEC-002's "no third-party TUI library" is specifically about UI frameworks, not a general
algorithmic library with no UI surface at all). Being MIT rather than public domain means an
attribution notice is required once it's actually vendored — a short credit (library name, author,
license text or a pointer to it) in e.g. a new `THIRD_PARTY_LICENSES.md` or a header comment atop
the vendored file itself, mirroring how `LICENSE` already documents this project's own MIT terms
(DEC-008); a one-time, low-cost addition at implementation time, not a reason to reconsider the
recommendation.

This is flagged as an open decision (**DEC-010** below) rather than silently assumed, the same way
Phase 1's SEH-isolation trade-off was confirmed with you before FEA-0002 implementation began — it
introduces this project's first-ever vendored third-party source, which is a real, if small,
precedent worth your explicit sign-off on before any code is written.

### §5. Phase 1 — Packing ("Pack file")

A new standalone plugin, `Source/ZipPlugin/` (mirroring `Source/ExamplePlugin/`'s own standalone-
`.vcxproj`-with-`PostBuildEvent`-copy shape exactly), declaring `kMcPluginCapMenu |
kMcPluginCapArchiveHandler` (Phase 3 below also needs `kMcPluginCapPanel`, declared from the
start even though Phase 1 doesn't use it yet, to avoid a second capability-bump release).

- **Menu integration**: registers "Pack file" on the File menu via `AddMenuItem` at `Init` —
  this is the *only* "Pack file" item once the plugin is loaded. `MenuBar.h`'s own built-in
  `stub(L"Pack Files", L'P')` is **removed** from `BuildMenus()` entirely (not just left as a
  stub) — exactly as the request's own first sentence asks ("shall be removed from the application
  startup... put onto the File menu"). With no ZIP plugin installed, the File menu simply has no
  "Pack file" item at all (one fewer stub, not a dead one) — acceptable, since the request frames
  packing as something the *plugin* provides, not a built-in the app always offers.
- **Options dialog** (`McHost_ShowFormFn`, §1): title "Pack files", fields:
  - Compression (`kMcFormFieldChoice`): `Store` / `Fast` / `Normal` / `Best`, default `Normal` —
    mapped internally to miniz's own `MZ_NO_COMPRESSION`/a low/default/`MZ_BEST_COMPRESSION`
    level. A friendly four-way choice, not a raw 0-9 dial — matches how consumer archivers
    (7-Zip, WinRAR, Windows' own "Compressed Folder") already present this, satisfying "add ones
    generally useful and used in other software."
  - "Include subdirectories" (`kMcFormFieldCheckbox`), **default checked**, exactly as asked.
    Unchecked packs only the items directly in the selection (no recursion at all) — if two
    selected top-level folders would flatten two identically-named files into the same archive
    path, the later one is skipped with a logged warning (`kMcLogWarning`) rather than an
    interactive per-conflict prompt; a deliberate scope simplification for this dialog's own
    one-shot nature (unlike `FileOps`'s own interactive `ConflictHandler`, this is a plugin-owned,
    non-interactive batch operation by design).
  - "Max archive size (MB), 0 = unlimited" (`kMcFormFieldNumber`), default `0`.
  - "Split into multiple archives if needed" (`kMcFormFieldCheckbox`), default unchecked.
- **Packing**: `GetSelectedNames`/`GetActivePanelPath` (existing, unchanged) give the source
  selection; output defaults to `<cursor-folder-name>.zip` in the *other* panel's directory
  (`GetPanelPath`, §3) — mirrors `F5`/`F6`'s own "destination is the other panel" convention
  (`ShowTextPrompt` lets the user rename/relocate it first, same shape as `DoCopyOrMove`'s own
  destination prompt). Every entry's Win32 attributes are stored in the ZIP's standard "external
  file attributes" field (the high word, DOS/Windows-attribute convention every mainstream ZIP
  tool — Explorer, 7-Zip, WinRAR, Info-ZIP — already reads/writes) — this is what satisfies "all
  files in the zip shall keep their own attributes" without inventing a non-standard extension.
- **Max size / multi-archive behavior**: if the configured cap would be exceeded and "split into
  multiple archives" is checked, output becomes `name.1.zip`, `name.2.zip`, ... — each one a
  **complete, independently valid ZIP file** containing a greedily-packed subset of the input
  (bin-packed by size, not by any particular order), not a single archive spanning multiple
  volumes via continuation records. This is a deliberate simplification over "true" PKZIP-spec
  multi-volume spanning (`.zip`/`.z01`/`.z02`/... with the central directory only in the last
  volume) — true spanning needs cross-volume central-directory bookkeeping most general-purpose
  ZIP readers other than WinRAR/7-Zip's own GUI tools don't even fully agree on, for a benefit
  (one logical archive instead of several numbered ones) this plan judges isn't worth that
  complexity for a first version. If the cap is exceeded and splitting is **not** checked, a
  confirmation dialog (`ShowChoice`, "the result would be ~X MB, over your Y MB limit — continue
  anyway / cancel") is shown rather than silently ignoring the configured limit or silently
  refusing outright — consistent with this app's existing "ask, don't silently proceed or
  silently block" convention (`FOP-010`/`SEC-003`'s own shape).

### §6. Phase 2 — Unpacking ("Unpack Package")

Symmetric treatment, same plugin, same `stub(L"Unpack Package", L'U')` removed from `MenuBar.h` and
replaced by the plugin's own "Unpack Package" menu item (`kMcPluginCapMenu`, registered alongside
"Pack file" at `Init`). Reads the cursor's own `.zip` file (`CursorFilePath`-identified, via
`GetSelectedNames`), extracts into the *other* panel's directory by default (`ShowTextPrompt` to
confirm/change the destination, same shape as Phase 1's own output-location prompt), restoring
each entry's stored Win32 attributes from the "external file attributes" field it was packed with.
Name-collision handling at the destination: same non-interactive "skip and log a warning" choice
as Phase 1's own flattening case, for the same reason (a batch, plugin-owned operation, not
`FileOps`'s own interactive per-conflict flow). After extracting, calls `RefreshPanel` (§3) on the
destination side so the freshly-extracted files appear without the user needing to leave and
re-enter that directory.

This phase exists because the request's own closing instruction asks to add what's "generally
useful and used in other software" — no mainstream archive tool packs without also unpacking, and
the File menu already has a matching, equally-inert `Unpack Package` stub sitting right next to
`Pack Files` for exactly this.

### §7. Phase 3 — Live archive browsing (the deepest ask)

Uses `kMcPluginCapArchiveHandler` (§2) + `kMcPluginCapPanel` (FEA-0002 Phase 3, already built)
together. `CanHandleFile` returns true for a `.zip` extension (case-insensitive); `EnterArchive`
opens the archive (reads its central directory into an in-memory entry list, grouped by the
`/`-delimited paths already standard inside ZIP archives — a "directory" inside the archive is
synthesized from common path prefixes, the same convention every ZIP-browsing tool already uses,
since ZIP itself has no real directory entries, only flat paths with `/` separators) and calls
`RequestPanelContent(side, archiveFileName)` to take over that panel.

- **Drawing** (`OnPanelDraw`): the plugin renders its own in-archive listing — name, size,
  compressed size, and attributes columns, matching `UI-004`'s own built-in column conventions as
  closely as the deliberately small `McPanelColor` palette (Normal/Highlight/Section/Warning,
  FEA-0002 §8) allows; the cursor row uses `kMcPanelColorHighlight`, a directory-like (path-prefix)
  entry uses `kMcPanelColorSection` the way a real directory row would read as visually distinct.
  `..` is always the first row unless already at the archive's own root view *and* the archive
  itself has no further "up" — see the exit case below.
- **Navigation** (`OnPanelKey`): `Up`/`Down`/`Page Up`/`Page Down`/`Home`/`End` move within the
  current in-archive level's own entry list (plugin-owned cursor state, one per open archive).
  `Enter` on a prefix/"directory" entry descends a level; `Enter` on a real entry does nothing
  (no in-archive viewer in this plan — `F3`/`F4` equivalents are explicitly out of scope, see Risk
  notes). `Insert` toggles the cursor entry's own selection mark (plugin-owned, mirroring
  `Panel::ToggleCursorSelection`'s exact behavior and advance-after-toggle convention).
- **Exiting** (`Backspace`, or `Enter`/`..` at the *archive's own root level*): if already inside a
  nested in-archive directory, goes up one level within the archive (no confirmation — this isn't
  leaving the archive, just navigating within it, same as `Backspace` in a real panel). Only at
  the archive's own top level does `..`/`Backspace` mean "leave the archive back to the real
  filesystem" — **exactly this transition asks for confirmation** (`ShowChoice`, "Leave
  `archive.zip`?"), per the request's own explicit ask; confirming calls `ReleasePanelContent(side)`
  (FEA-0002, already built) and the panel reverts to its normal, real-filesystem display — the
  real `Panel`'s own path/history were never touched while the archive view was up, so this is an
  instant, lossless revert, not a re-navigation.
- **Select/Delete/Copy to/Copy from** ("standard panel functions," read per §0's own framing —
  behaviorally equivalent, not literally `Panel`'s own code):
  - *Select*: `Insert` as above; a dedicated "select all"/"clear"/"invert" key set mirroring
    `Ctrl+Numpad +`/`-`/`Numpad *` is in scope too, cheap to add once the plugin owns its own
    selection state at all.
  - *Delete* (`F8` is already claimed by the host's own built-in Delete — per §2's keyboard
    interception rule from FEA-0002 Phase 3, every key except `Tab`/`F10` already reaches this
    plugin's `OnPanelKey` while its content is installed, so `F8` is available to it): removes
    the selected entries from the in-memory entry list and **rewrites the whole archive** (via
    the same packing engine as Phase 1, re-adding every entry *except* the deleted ones) —
    confirmed first (`ShowChoice`, matching the host's own delete-confirmation convention,
    `Config::confirmPermanentDelete`'s spirit even though this isn't Recycle-Bin-eligible at all,
    there being no "recycle bin for one entry inside a zip"). Whole-archive rewrite rather than
    true in-place local-header patching is a deliberate simplification — correct and simple,
    acceptably slow only for a very large archive, which is the same trade-off most lightweight
    ZIP-editing tools already make.
  - *Copy to / Move to* (`F5`/`F6`, reusing the host's own existing key bindings the same way via
    `OnPanelKey`): extracts the selection (or cursor entry) into the *other* panel's directory
    (`GetPanelPath`, §3) — "Move to" additionally removes the extracted entries from the archive
    afterward (the same rewrite path as Delete).
  - *Copy from* ("paste into this archive," no existing host key for "the other panel's selection
    into here" — new to this plugin, bound to `Shift+Insert` by analogy with the Windows clipboard
    convention, registered via `RegisterShortcut` the same as any plugin shortcut, §12): adds the
    *other* panel's currently selected real files (`GetPanelSelectedNames`, §3) into the open
    archive at the current in-archive directory level, using Phase 1's own packing/attribute logic.
- **Nested archive content** (a `.zip` *inside* another open `.zip`): **out of scope** for this
  phase — `CanHandleFile`/`EnterArchive` are only ever reached from a *real* panel's
  `ActivateCursorEntry`, never from inside another plugin's own `OnPanelKey`; opening an archive
  found while already browsing one would need its own, separate design. Flagged under Phase 4/
  LATER below, not silently attempted.

## Spec entries (applied for Phase 1/2; ZIP-005–008 exist but await Phase 3)

This section originally proposed these rows before any implementation existed. **As of Phase 2,
all eight rows and `DEC-010` have been applied to `AIPrompt/my-commander.md` itself** — this is no
longer a standalone proposal, it's a record of what's there now, kept here for the plan's own
narrative continuity. The requirements table below mirrors the real spec's current state; if the
two ever disagree, `AIPrompt/my-commander.md` is the source of truth, not this copy.

New section, **`## 27. ZIP Archive Requirements`** (sections `27`-`32` were renumbered to `28`-`33`,
the same mechanical renumbering every prior new section insertion in this spec has already done):

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| ZIP-001 | A ZIP plugin shall provide a "Pack file" command on the File menu, replacing the application's own built-in stub. | P2 | IN-MVP | **Implemented** |
| ZIP-002 | The pack command shall offer a dialog with compression level, include-subdirectories (default on), a maximum archive size, and multi-archive splitting options. | P2 | IN-MVP | **Implemented** |
| ZIP-003 | Every packed entry shall retain its own Win32 file attributes, readable back on unpack. | P2 | IN-MVP | **Implemented** |
| ZIP-004 | A ZIP plugin shall provide an "Unpack Package" command on the File menu, replacing the application's own built-in stub. | P2 | IN-MVP | **Implemented** |
| ZIP-005 | Pressing Enter on a `.zip` file shall step into it as a navigable, panel-rendered virtual directory. | P2 | IN-MVP | Not Started — Phase 3 (§7); needs §2's new `kMcPluginCapArchiveHandler` capability/host hook, not yet built |
| ZIP-006 | While browsing an archive, the panel shall support selection, deletion, extraction to the other panel, and adding the other panel's selection into the archive. | P2 | IN-MVP | Not Started — Phase 3 (§7), same prerequisite as `ZIP-005` |
| ZIP-007 | Navigating out of an archive's own root level (back to the real file system) shall ask for confirmation first. | P3 | IN-MVP | Not Started — Phase 3 (§7), same prerequisite as `ZIP-005` |
| ZIP-008 | Nested archive browsing, password/encrypted archives, and an archive integrity check are useful extensions not required for the initial release. | P3 | LATER | Not Started — intentionally deferred, not blocked on anything |

Resolved open decision:

| ID | Question | Candidates | Recommendation | Status |
|---|---|---|---|---|
| DEC-010 | ZIP/DEFLATE implementation | Hand-rolled DEFLATE; vendor a small, permissively-licensed library (miniz) | Vendor **miniz** (single `.c`/`.h`, MIT-licensed, compiled directly into the standalone ZIP plugin project — no package manager, no build-system change) | **DECIDED**: vendored miniz 3.1.2 (MIT) into `Source/ZipPlugin/third_party/miniz/` (2026-10-01) |

`EXT-003` ("a public plug-in API may later support... archive handlers") has been reworded to
reference this section, the same way it already references FEA-0002's own §26.

**Implementation readiness for what's left:** Phase 3 (live browsing, `ZIP-005`-`007`) is **not**
ready yet — §2's `kMcPluginCapArchiveHandler` capability, its two new plugin exports, and the
`ActivateCursorEntry` host hook in `main.cpp` all still need to be designed into working code
first; nothing Phase 1/2 built reduces that remaining scope.

## Files touched

**Already done (Phase 1/2):**

- `Source/MyCommanderPluginSdk/McPluginTypes.h` — `McFormFieldKind`/`McFormField`.
- `Source/MyCommanderPluginSdk/McPluginHost.h` — `McHost_ShowFormFn`, `McHost_GetActiveSideFn`,
  `McHost_GetPanelPathFn`, `McHost_GetPanelSelectedNamesFn`, `McHost_RefreshPanelFn`, appended to
  `McPluginHost`.
- `Source/MyCommanderPluginSdk/McPluginAbi.h` — `MC_PLUGIN_API_VERSION_MINOR` bump (2 → 3; no
  further bump needed for Phase 2, which added no new SDK surface at all).
- `Source/MyCommanderPluginSdk/DEVELOPER_GUIDE.md` — new "Options dialog"/"Addressing a specific
  panel" sections; a §8 pointer to `ZipPlugin.cpp` as a second worked example, now also covering
  Unpack's own untrusted-input-validation example.
- `Source/MyCommander/Dialog.h`/`Dialog.cpp`/`DialogInput.cpp` — `FormField`/`ShowFormDialog`/
  `ApplyFormFieldKey`; `Strings.h` gained `kStrFormDialogHint`.
- `Source/MyCommander/PluginManager.h`/`.cpp` — `HostShowForm`/`HostGetActiveSide`/
  `HostGetPanelPath`/`HostGetPanelSelectedNames`/`HostRefreshPanel` and their thunks.
- `Source/MyCommander/MenuBar.h` — both the `Pack Files` and `Unpack Package` stubs removed.
- `Source/ZipPlugin/` (new, standalone `.vcxproj` + `PostBuildEvent` copy, mirroring
  `Source/ExamplePlugin/`'s own shape) — the plugin itself (`kMcPluginCapMenu` only — Phase 3's
  `kMcPluginCapPanel` not yet needed), registering both `"Pack file"` and `"Unpack Package"`; plus
  vendored `third_party/miniz/{miniz.c,miniz.h,LICENSE,VENDORED.md}`.
- `AIPrompt/my-commander.md`/`CLAUDE.md`/`README.md` — see the Spec entries section above and the
  2026-10-01 changelog entries for the full list.

**Still pending (Phase 3, not yet touched):**

- `Source/MyCommanderPluginSdk/McPluginAbi.h` — `kMcPluginCapArchiveHandler`;
  `McPlugin_CanHandleFileFn`/`McPlugin_EnterArchiveFn` typedefs; another
  `MC_PLUGIN_API_VERSION_MINOR` bump when this lands.
- `Source/MyCommander/PluginManager.h`/`.cpp` — `TryEnterAsArchive`; load-time lookup of the two
  new plugin exports when `kMcPluginCapArchiveHandler` is declared.
- `Source/MyCommander/main.cpp` — `ActivateCursorEntry`'s new pre-check (§2); its signature gains
  `PluginManager&` (already threaded through most nearby call sites from FEA-0002).
- `Source/ZipPlugin/ZipPlugin.cpp` — the archive-browsing logic itself; `kMcPluginCapPanel` added
  to its declared capabilities.

## Test plan

- `PluginManager::TryEnterAsArchive`'s own dispatch logic (which plugin's `CanHandleFile` wins,
  "no match falls through to existing behavior unchanged") is the one genuinely new piece of
  *host*-side pure-ish logic — if it can be expressed without a live `Console`/`Panel` (a plain
  "given a list of (pluginId, CanHandleFile-result) pairs, which one wins" function), it belongs
  in `PluginManagerLogic.cpp` with `PluginManagerTests.cpp` coverage, matching every prior
  FEA-0002 phase's own TST-005 split.
- `ShowFormDialog`'s field navigation/editing is `Dialog.cpp`-local UI glue, same category as
  `ShowChoiceDialog`'s own `MOUSE_EVENT`/key handling — no automated coverage expected, consistent
  with this file's established limit; manual verification once implemented (Tab/arrow navigation
  between fields, checkbox toggle, number clamping, Esc cancels without applying).
  `BuildOptionRegions`-style pure helpers, if any fall out of the implementation, follow
  `DialogInputTests.cpp`'s existing precedent.
- The ZIP plugin's own packing/unpacking/attribute-preservation logic is real, substantial new
  code with no existing test harness to extend — recommend the plugin itself gain a small, private
  test entry point (a `--self-test` mode run manually, or a short-lived standalone test `.cpp`
  compiled only when building the plugin directly, mirroring how `MyCommanderTests` is a sibling
  project rather than linked into `MyCommander.exe`) rather than shipping with zero automated
  coverage for logic this consequential (an attribute-preservation or size-splitting bug would be
  silent data loss, not a cosmetic glitch) — the exact shape is an implementation-time decision,
  flagged here as a requirement on the *process*, not a specific mechanism.
- Manual, interactive verification (same standing limitation as every FEA-0002/IS-000N fix
  involving a live run) for: pack with each compression level, recursive vs. flat packing, a
  deliberately small max-size triggering the multi-archive split, attribute round-trip
  (read-only/hidden/system survive pack→unpack), stepping into a `.zip`, nested in-archive
  navigation, delete-from-archive, extract-to/add-from the other panel, and the exit-confirmation
  prompt specifically declining (archive view must remain open) and accepting (must revert to the
  real directory with no residual state).

## Risk / scope notes

- **This is explicitly larger and riskier than FEA-0002.** `DEC-010` (library choice) and the §0
  architectural approach (plugin-based, not a `Panel`/`FileOps` refactor) were confirmed before
  Phase 1 began, the same way FEA-0002 Phase 1's own scope/isolation trade-offs were. Phase 1
  (Pack) is now implemented and verified. The same phase-by-phase discipline still applies to what
  remains: implement and verify Phase 2 (Unpack, §6) and Phase 3 (Browse, §7) separately, each with
  its own full Debug+Release build and test-suite run, rather than attempting both at once — Phase
  3 in particular introduces a genuinely new host hook (`ActivateCursorEntry`'s pre-check, §2) that
  deserves its own isolated verification pass before being combined with anything else.
- **No in-archive file viewing/editing** (`F3`/`F4` equivalents) is proposed in any phase here —
  the request asks for panel functions (select/delete/copy), not a viewer; adding one would mean
  extracting to a temp file first and is a reasonable, separate future ask, not assumed here.
- **No encryption/password support** in any phase — ZipCrypto (the "legacy" ZIP password scheme)
  is cryptographically weak enough that shipping it as this feature's only option would be a
  disservice, and WinZip-style AES encryption is real additional complexity (a crypto backend
  decision of its own); explicitly `LATER` (`ZIP-008`).
- **Whole-archive rewrite on every in-archive delete/add** (§7) is a real, accepted performance
  trade-off for a very large archive — flagged plainly rather than silently accepted, since "pack
  a single new file into a 2 GB archive re-reads and re-writes the whole 2 GB" is a real,
  noticeable cost a user could hit.
- **Multi-archive splitting (§5) is independent numbered `.zip` files, not true PKZIP-spec
  volume-spanning** — restated here (also in §5 itself) since it's the one place this plan departs
  furthest from what "multi file compression" might initially sound like; the reasoning (format
  compatibility, implementation cost) is the same engineering judgment this project has made
  elsewhere (e.g. the Markdown renderer's own "deliberately minimal, not full CommonMark" choice).
- **`EXT-002`'s own "common provider interface" is not implemented by this plan** — `§0` discusses
  why, explicitly, so a future reader doesn't mistake the plugin-based approach here for having
  quietly completed `EXT-002`; that row's own `Disposition`/`Implemented` fields are untouched by
  this plan.

## Status

**Phase 1 (Pack, §5): implemented** (2026-10-01). `DEC-010` was decided (miniz 3.1.2, MIT-licensed)
and the `§0` plugin-based architectural approach confirmed before implementation began.

- §1 (generic options dialog): `McHost_ShowFormFn`/`McFormField` landed exactly as designed —
  checkbox/number/choice/text fields, a new `Dialog.h`/`Dialog.cpp` `ShowFormDialog` plus pure
  `ApplyFormFieldKey` (`DialogInput.cpp`, TST-005 precedent, 10 test cases).
- §2 (claiming a file as a virtual directory) and the `kMcPluginCapPanel`-based live browsing it
  feeds into §7: **not implemented this phase** — Phase 1 only ever needed `kMcPluginCapMenu`;
  declaring `kMcPluginCapArchiveHandler`/`kMcPluginCapPanel` now with no matching exports would be
  a load-time warning for nothing gained, so (departing from this plan's own "declare both from
  the start" suggestion in §5) each capability now lands only with the phase that actually
  implements it — the same precedent FEA-0002's own three phases already established.
- §3 (side-scoped panel queries): `GetActiveSide`/`GetPanelPath`/`GetPanelSelectedNames`/
  `RefreshPanel` all landed exactly as designed (`GetPanelSelectedNames` is implemented and
  available, even though Phase 1's own packing flow doesn't call it yet — only Phase 3's "copy
  from the other panel into the archive" will).
- §4 (`DEC-010`): vendored miniz 3.1.2 into `Source/ZipPlugin/third_party/miniz/` — **MIT-licensed**,
  not public domain as this plan's own earlier draft said before being corrected against the live
  repository.
- §5 (Pack): landed with three deliberate refinements beyond the original text, each documented
  in place and in the changelog rather than discovered as a surprise later: (1) exceeding the size
  cap with splitting off is an *after-the-fact* `ShowMessage` notice, not a *before-the-fact*
  "continue anyway?" confirmation — accurately predicting compressed size ahead of actually
  compressing isn't practical; (2) "without subdirectories" flattens a selected folder's own
  direct files into the archive root (one level, no further recursion), which is what actually
  makes two same-named files from different folders collide, as this plan's own §5 text already
  anticipated; (3) attribute preservation (`ZIP-003`) required a post-write binary patch of the
  ZIP central directory's external-attributes field, since miniz's public writer API never exposes
  it — verified correct via a standalone round-trip through miniz's own reader before being wired
  into the real plugin, and again via a throwaway harness exercising `ZipPlugin.cpp`'s own
  production functions against real temp files.
- `ZIP-001`/`002`/`003` promoted to `IN-MVP`/Implemented in the spec (§27, new section); `DEC-010`
  resolved; `EXT-003` reworded to reference it.
- Full test suite: 425 cases, 11,796 checks, 0 failed (10 new). Full interactive verification
  (actually opening the dialog and packing a selection through the running UI) remains
  outstanding — the same standing no-interactive-console-capture limitation noted throughout this
  project — mitigated by the standalone packing-logic harness described above, which exercises
  real production code against real files rather than only confirming it compiles.

**Phase 2 (Unpack, §6): implemented** (2026-10-01). Confirmed before starting that no new SDK
surface was needed — every host function it uses (`GetSelectedNames`, `GetActivePanelPath`,
`GetActiveSide`/`GetPanelPath`/`RefreshPanel`, `ShowTextPrompt`) already existed from Phase 1, and
that held exactly.

- §6: landed as designed — `MenuBar.h`'s `"Unpack Package"` stub removed, the plugin's own item
  registered in its place (mnemonic `U`, reusing the stub's own former letter, same as Pack's `P`).
  Extracts into the other panel's directory, restoring each entry's stored attributes read back
  from the same external-attributes field Phase 1 writes. One subfolder per archive (named after
  the archive) when more than one `.zip` is selected at once, no extra nesting for a single
  selection — a concrete design choice the original §6 text left implicit, resolved here the same
  way mainstream archive tools already batch-extract.
- One deliberate hardening addition beyond the original §6 text, not a deviation from it so much
  as filling a gap the text didn't address at all: a **zip-slip guard** (`IsSafeArchiveEntryName`)
  rejecting any entry name containing a `..` segment, a leading slash, or a drive letter, checked
  before any directory is created or any byte is written. A corrupted or malicious archive could
  otherwise name an entry to escape the chosen destination directory — worth adding proactively
  for a feature that writes files to disk from untrusted input, even though the request itself
  never raised it.
- Destination-collision handling reuses Phase 1's own non-interactive "skip and log" convention
  (a destination file that already exists is left untouched, not overwritten) — the same
  batch-operation reasoning, now applied symmetrically to both directions.
- `ZIP-004` promoted to `IN-MVP`/Implemented in the spec.
- Full test suite: 425 cases, 11,794 checks, 0 failed (unchanged case count from Phase 1 — no new
  pure logic to extract this phase; 2 fewer checks net from `MenuBarTests.cpp`'s own updated
  expectations). Verified with a standalone harness exercising `ZipPlugin.cpp`'s own real
  `ExtractZipArchive`/`IsSafeArchiveEntryName` against a real archive built with the already-
  verified Phase 1 writer: full pack→unpack round trip, nested-directory auto-creation, both
  attributes restored exactly, content byte-exact, every zip-slip shape rejected, and re-extracting
  into the same destination correctly skips rather than overwrites — 13/13 checks passed. Full
  interactive verification (actually unpacking a selection through the running UI) remains
  outstanding, the same standing limitation as Phase 1.

**Phase 3 (live browsing, §7): not implemented** — `ZIP-005` through `007` remain `Not Started` in
the spec. §2's `kMcPluginCapArchiveHandler` capability, its two new plugin exports, and the
`ActivateCursorEntry` host hook in `main.cpp` all still need to be designed into working code
first; nothing from Phase 1/2 reduces that remaining scope.
