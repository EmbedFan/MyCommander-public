# Feature Plan — FEA-0003

Source: `AIPrompt/additional_feature_requests.md`

> Add a file property show table
>
> Connect it to menu File->View->Executable Properties
>
> The file properties depend on the file type:
> When the file is runnable:
> Show the file info fields:
> Description, File version, Product Name, Product version, Copyright
>
> Try to show the Digital signo
> a) The file is not signed
> b) The short signature information
>
> General fields:
> File size (Byte/MByte/Gbyte), Last modification, File name

## Summary

Adds a **File Properties** view (Alt+Enter — Explorer's own convention for "show properties",
currently unbound: `main.cpp`'s dual-pane `VK_RETURN` case only checks `ctrlHeld`/an empty command
line, never `altHeld`) for the panel cursor's file entry. Shows three **General** fields always
(file name, human-readable size, last modification time — reusing the exact formatting `DrawPanel`'s
own columns already use), and, only when the file is a PE executable with an embedded
`VERSIONINFO` resource ("runnable" — `.exe`/`.dll`/`.ocx`/`.sys`/`.cpl`/`.scr`/`.drv`), five more
**File info** fields (Description/File version/Product Name/Product version/Copyright — the same
`StringFileInfo` block `Help.cpp`'s own About section already reads, generalized here from "the
running exe's own path" to "any file path") plus a best-effort **Digital signature** line
(Authenticode-verified via `WinVerifyTrust`: "Not signed", or signed with a short signer name).

Wired to the request's own named integration point: View menu's `Executable properties` item
(FEA-0005, currently a `Stub`) becomes a `Real` item — the same `Alt+Enter` key, replayed via
`SimulateKeyPress` exactly like every other Real menu item.

## Current state

- `main.cpp`'s dual-pane `VK_RETURN` case (~line 2642) checks `ctrlHeld` (insert name/path) and
  `!commandLine.empty()` (run/navigate); with neither, it calls `ActivateCursorEntry`. `altHeld`
  is never checked, so `Alt+Enter` today is indistinguishable from plain `Enter` — it already
  activates the cursor entry. This is a genuine (if unlikely-to-be-relied-on, since it's currently
  unremarkable) behavior change for anyone who happens to hold Alt while pressing Enter; noted
  here rather than silently introduced.
- `CursorFilePath(Panel&)` (~line 1327) already returns the cursor entry's resolved path, or
  `nullopt` for `..`/a directory — the exact same "files only" scoping `DoView`/`DoEdit` already
  use, and this feature's own scope (a *file* properties view).
- `Help.cpp`'s `ReadFileVersionMetadata()` (KEY-004's About section) already contains the exact
  `GetFileVersionInfoSizeW`/`GetFileVersionInfoW`/`VerQueryValueW` pattern this feature needs,
  scoped to `GetModuleFileNameW`'s own path (the running exe). This plan generalizes that same
  pattern to an arbitrary file path rather than duplicating it a third time (`main.cpp`'s
  `--version` output is the second, compile-time-only user of `Version.h` directly).
- `FEA-0005`'s `MenuBar.h` `BuildMenus()` currently has View's `Executable properties` as
  `stub(L"Executable properties", L'E')` — the exact item this plan wires up.
- No code in this repository currently links `wintrust.lib`/`crypt32.lib`, or calls any
  Authenticode/certificate API — this is new Win32 API surface, isolated to one new module.

## Design

### 1. New module: `FileInfo.h`/`.cpp`

Same shape as `TextFile.h`/`.cpp` (VEE-004's precedent: real file/Win32 I/O, but `Console`-free,
so it's usable from `MyCommanderTests` directly) — split into a pure part and a Win32-I/O part:

- `bool IsRunnableExtension(const std::wstring& extension)` — **pure**: case-insensitive match
  against the PE extensions that can carry a `VERSIONINFO` resource (`.exe .dll .ocx .sys .cpl
  .scr .drv`). Accepts the extension with or without a leading dot, matching
  `std::filesystem::path::extension()`'s own convention either way callers might pass it.
- `struct FileVersionFields { bool available; std::wstring description, fileVersion, productName,
  productVersion, copyright; }` and `FileVersionFields ReadFileVersionFields(const
  std::filesystem::path& path)` — the exact `GetFileVersionInfoSizeW`/`GetFileVersionInfoW`/
  `VerQueryValueW` sequence `Help.cpp`'s `ReadFileVersionMetadata` already has, generalized to take
  a path instead of resolving the running exe's own. `available = false` (every field left empty)
  if the file has no `VERSIONINFO` resource at all, or any Win32 call fails — never a crash, the
  same graceful-degradation `Help.cpp`'s own version already established for this exact API.
- `enum class SignatureStatus { NotSigned, Signed, CheckFailed };` and `struct SignatureInfo {
  SignatureStatus status; std::wstring signerName; }` (meaningful only when `status == Signed`)
  and `SignatureInfo CheckDigitalSignature(const std::filesystem::path& path)` — Authenticode
  verification via `WinVerifyTrust` (`WINTRUST_ACTION_GENERIC_VERIFY_V2`, `WTD_UI_NONE` — no UI of
  its own, `WTD_REVOKE_NONE` — no network revocation check, keeping this a fast, local-only,
  best-effort check rather than a full trust decision) against a `WINTRUST_FILE_INFO` for `path`.
  `WinVerifyTrust` returning `ERROR_SUCCESS` means signed; any other result means not signed (or
  unverifiable, folded into `NotSigned` — this feature reports presence/absence, not a full trust
  verdict, matching the request's own "a) not signed / b) short signature information" framing).
  When signed, `CryptQueryObject`/`CryptMsgGetParam`/`CertGetNameStringW` extract the signing
  certificate's subject display name for `signerName` — if that extraction step itself fails,
  `status` stays `Signed` with an empty `signerName` (main.cpp shows a generic "Signed" line
  instead of a name) rather than downgrading to `NotSigned`, since the file genuinely is signed
  even if this feature couldn't read who by. `CheckFailed` is reserved for `path` itself being
  unreadable (e.g. deleted between the panel listing and the keypress) — distinguished from
  `NotSigned` in `main.cpp`'s own display text so "not signed" isn't misreported for a file this
  couldn't actually check.

### 2. Gathering + display: `main.cpp`

New `DoShowFileProperties(Console&, Panel& left, Panel& right, bool leftActive)`:

1. `CursorFilePath(target)` — same `..`/directory exclusion `DoView` already has; a no-op if
   `nullopt`.
2. Builds the **General** lines from the *panel's own already-loaded* `Entry` (name, `sizeBytes`
   via a shared human-readable formatter — see below, `lastWriteTimeUtc` via the existing
   `FormatModifiedTime`) rather than re-querying the filesystem — the panel already has this data,
   and it's what the user is actually looking at.
3. If `IsRunnableExtension(entry path's extension)`: calls `ReadFileVersionFields`/
   `CheckDigitalSignature` and appends the **File info** lines only when `available`, plus one
   **Digital signature** line ("Not signed." / "Signed" + signer name if known / "Could not check
   the file's signature." for `CheckFailed`).
4. `ShowMessage(console, kStrFilePropertiesTitle, lines, redraw)` — the exact same centered,
   auto-sized, any-key-or-click-dismissed box every other informational dialog in this app already
   uses (IS-0003's own click-to-dismiss included, for free). No new `Dialog.cpp` code needed.

`FormatHumanBytes` (already in `main.cpp`, `FOP-009`'s copy-progress formatter — "3.2 MB") is
reused directly for the size line, matching the request's own "Byte/MByte/Gbyte" wording; no
second implementation.

### 3. Wiring

- `main.cpp`'s dual-pane `VK_RETURN` case gains an `if (altHeld) { DoShowFileProperties(...); break;
  }` guard, checked first (before the existing `ctrlHeld`/command-line branches) — `Alt+Enter`
  takes priority the same way every other modifier combination on this key already does.
- `MenuBar.h`'s `BuildMenus()`: View's `stub(L"Executable properties", L'E')` becomes
  `real(L"Executable properties", L'E', VK_RETURN, /*ctrl=*/false, /*shift=*/false, /*alt=*/true)`
  — the exact same `Alt+Enter` `HintAction`, replayed via the already-existing `SimulateKeyPress`
  mechanism (no new dispatch path).

## Files touched

- `Source/MyCommander/FileInfo.h`/`.cpp` — new (§1).
- `Source/MyCommander/main.cpp` — `DoShowFileProperties`, the `VK_RETURN` case's new `altHeld`
  guard (§2/§3).
- `Source/MyCommander/MenuBar.h` — View's `Executable properties` item, `Stub` → `Real` (§3).
- `Source/MyCommander/Strings.h` — `kStrFilePropertiesTitle` and the handful of fixed label
  strings ("File name", "Size", "Modified", "Description", "File version", "Product Name",
  "Product version", "Copyright", "Digital signature", "Not signed.", etc.).
- `Source/MyCommander/MyCommander.vcxproj` — new files added to the existing `ClCompile`/
  `ClInclude` item groups (matching every prior new-module precedent this session).
- `Source/MyCommanderTests/FileInfoTests.cpp` — new (see **Test plan**); `MyCommanderTests.vcxproj`
  gains it plus `FileInfo.cpp` compiled in (matching `TextFile.cpp`'s own existing precedent there).
- `Source/MyCommanderTests/MenuBarTests.cpp` — updates the existing "Executable properties is a
  Stub" assumption, if any test currently encodes it (checked during implementation).
- `Source/MyCommander/Help.cpp` — F1 reference gains an `Alt+Enter` row (KEY-002/003's mirroring
  discipline with README).
- `README.md`/`USERGUIDE.md` — a `File properties` mention (keybindings table + a short
  description), matching every prior feature's documentation treatment this session.
- `AIPrompt/my-commander.md` — a new small requirements block (proposed prefix `PROP-`, since
  nothing existing fits a file-properties view specifically — parallel to `INST-`/`MENU-`'s own
  precedent of introducing a new prefix when needed); exact IDs decided at implementation time,
  matching those two plans' own approach to the same question.

## Test plan

`IsRunnableExtension` is pure — straightforward table-driven `TEST_CASE`s (every listed extension
matches case-insensitively with and without a leading dot; a non-PE extension like `.txt`/`.ini`
doesn't; an empty extension doesn't).

`ReadFileVersionFields`/`CheckDigitalSignature` do real file I/O and call real Win32 APIs, matching
`TextFile.h`'s own established precedent for that shape of function — tested against real files
via `TempDir` (TST-004) plus one already-available real PE file this repo's own test run always
has on disk: the test harness's own `MyCommanderTests.exe` (`GetModuleFileNameW`), which carries a
real `VERSIONINFO` resource (`MyCommanderTests.rc`) with a known `ProductName` ("MyCommander") to
assert against — the same "point it at our own already-known binary" trick `Help.cpp`'s original
implementation effectively relies on, just reused here as a test fixture instead of the shipped
behavior. Additional cases: a plain non-PE text file (`available == false`, no crash) and a
nonexistent path (`available == false`/`CheckFailed`, no crash).

`WinVerifyTrust`'s actual *positive* signed-and-trusted path can't be exercised without a real,
currently-valid Authenticode-signed binary checked into the repo (fragile — real certificates
expire) — matching this session's own established precedent for OS-trust-subsystem code that
can't be fully driven by an automated test (`Elevation.h`'s `RunElevated`, `MyCommanderSetup`'s
`IShellLinkW`): the *negative* path (an ordinary unsigned file → `NotSigned`) is fully covered by
`TempDir`-created files, and the positive path is verified manually once implemented, e.g. against
`MyCommander.exe` itself before it's been Authenticode-signed (expected: `NotSigned`, since this
project doesn't currently sign its own builds) and, if a signed system binary is available on the
test machine (e.g. `%SystemRoot%\System32\notepad.exe` on a modern Windows install), confirming
`Signed` with a plausible Microsoft signer name.

`DoShowFileProperties`/the `VK_RETURN` `altHeld` guard/the `MenuBar.h` wiring are `main.cpp`-local
dispatch glue — no dedicated test, consistent with this codebase's fully established limit for
that category of code (every prior `IS-000N`/`FEA-000N` plan this session has made the same call).

## Risk / scope notes

- **`Alt+Enter` was not previously a distinct key combination** (it behaved identically to plain
  `Enter`) — this is a real, if very unlikely to be noticed, behavior change. Called out explicitly
  rather than silently introduced; no other key changes meaning.
- **Digital signature checking is deliberately shallow**: presence/absence and a display name only
  — no certificate chain validation UI, no revocation checking (`WTD_REVOKE_NONE`, explicitly, for
  speed and to avoid a network call from what's meant to be a quick properties lookup), no "this
  signature is invalid/expired" distinction beyond folding those into `NotSigned`. Matches the
  request's own "try to show" framing (best-effort informational, not a security verdict) — a
  fuller trust-verification UI is out of scope unless separately asked for.
- **"Runnable" is defined by file extension, not by actually inspecting PE headers.** A renamed
  file (e.g. `notepad.exe` saved as `notepad.dat`) won't be detected as runnable even though it
  has a real `VERSIONINFO` resource; conversely this plan doesn't design for that edge case, since
  the request's own framing ("when the file is runnable") reads as extension-driven, matching how
  a user would think about it (and how `FormatType`/other panel logic already treats extensions as
  the source of truth for a file's "kind").
- **No `Implemented`/Disposition change to any existing requirement row** — this is new
  functionality behind a new proposed `PROP-` prefix, not a refinement of something already
  tracked.

## Status

**Implemented.** No design choice above needed a confirming question, matching the assessment
that started this section; two real bugs were found and fixed during live verification (neither
was anticipated by this plan — both are documented here rather than silently patched):

- **`ReadFileVersionFields`'s hardcoded `"040904B0"` language block was wrong for any
  non-English-built file.** Caught live against `notepad.exe` on this machine (Hungarian display
  language): `available` correctly came back `true` (the resource exists), but every string field
  came back empty, since the file's actual `StringFileInfo` block is `"040E04B0"` (Hungarian), not
  `"040904B0"`. Fixed by reading `\VarFileInfo\Translation` first (the standard, documented
  technique for this exact API) and using the file's *own* language/codepage pair, falling back to
  `"040904B0"` only if that translation block itself is missing. Re-verified: `FileDescription`/
  `ProductName`/`FileVersion`/`LegalCopyright` for `notepad.exe` now match
  `(Get-Item ...).VersionInfo`'s own values exactly (in Hungarian, as expected).
- **`CheckDigitalSignature` reported `NotSigned` for every genuinely-signed Windows system
  binary** (`notepad.exe`, `cmd.exe`, ...) — confirmed against `Get-AuthenticodeSignature`'s own
  `Status: Valid` for the same files. Root cause: most Windows system binaries are **catalog-
  signed**, not embedded-signed — `WinVerifyTrust`'s plain `WTD_CHOICE_FILE` check (§1's original
  design) only ever sees an embedded signature and reports `TRUST_E_NOSIGNATURE` for a catalog-
  signed file even though the file is genuinely trusted, a well-documented Authenticode API
  limitation, not a mistake in the original `WinVerifyTrust` call itself. Fixed by adding a
  catalog-lookup fallback (new `FindCoveringCatalog`, `<mscat.h>`'s `CryptCATAdminAcquireContext`/
  `CryptCATAdminCalcHashFromFileHandle`/`CryptCATAdminEnumCatalogFromHash`/
  `CryptCATCatalogInfoFromContext`) tried only when the embedded check fails — if the file's hash
  is covered by any installed system catalog, it's genuinely signed via that catalog, and the
  catalog file itself (also Authenticode-signed) is reused to extract a signer name the same way
  as an embedded signature. A second bug surfaced while building this fix: `CryptCATAdminAcquireContext`'s
  `pgSubsystem` parameter, passed as the commonly-copied `DRIVER_ACTION_VERIFY` GUID, returns
  `ERROR_ACCESS_DENIED` for a non-elevated caller (confirmed live) — passing `nullptr` instead
  (a generic, non-driver-specific context) succeeds for an ordinary user, matching SEC-001/002's
  "never require elevation for ordinary operation" the same way every other feature in this app
  already does; this app never runs elevated, so the fix isn't optional. Also worked around
  `DRIVER_ACTION_VERIFY`'s own symbol not resolving cleanly from `<mscat.h>`/`<softpub.h>` in this
  toolchain by defining its well-known GUID value locally instead — moot now that `nullptr` is
  what's actually used, but the underlying header-fragility note is kept for anyone touching this
  code later. Re-verified: `notepad.exe`/`cmd.exe` both now report `Signed`, signer `"Microsoft
  Windows"` — matching `Get-AuthenticodeSignature`'s certificate subject exactly.
- Every fix above was caught by writing a small, disposable, standalone verification program
  against real system files (`notepad.exe`, `cmd.exe`) and cross-checking the output against
  PowerShell's own `Get-AuthenticodeSignature`/`(Get-Item ...).VersionInfo` — neither bug was
  something the `TempDir`-based automated tests (necessarily working with ordinary, non-catalog-
  signed, English-only test files) could have caught on their own. The disposable verification
  program itself was deleted afterward, not committed.
- `Executable properties` (View menu, FEA-0005) is now `real(..., VK_RETURN, alt=true)`, per the
  request's own explicit connection point; no `MenuBarTests.cpp` assertion depended on it staying
  a `Stub`, so no test needed updating there.
- New requirement rows use the proposed `PROP-` prefix (`PROP-001` through `PROP-004`), added to
  `AIPrompt/my-commander.md`, all `Implemented`.
- Full test suite: 385 cases, 11,686 checks, 0 failed (up from 375/11,660 before this feature — the
  10 new `FileInfoTests.cpp` cases). Both Debug and Release builds clean across all three projects.
