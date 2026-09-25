# Feature Plan — FEA-0006

Source: `AIPrompt/additional_feature_requests.md`

> Add an application installer.
>
> The installer shall built up from native c++
> First just create the plan.
> The installer check the prerequisits for running, than install required elements and this
> software.
> Create a desktop launcher icon for this app.

## Summary

MyCommander currently has no installer at all — "installing" it today means manually building
`MyCommander.sln` and copying `Build\x64\Release\MyCommander.exe` somewhere, with no shortcut, no
prerequisite check, and no clean way to uninstall. This plan adds a second, standalone native C++
executable — `MyCommanderSetup.exe` — built as a third project in the existing `MyCommander.sln`,
alongside `MyCommander` and `MyCommanderTests` (DEC-009: one native VS2022 solution, no separate
build system, no third-party installer framework such as WiX/Inno/NSIS — the request explicitly
says "built up from native C++"). It checks that the target machine can actually run MyCommander,
copies the application to a per-user install folder, creates a desktop shortcut, and registers
itself with Windows so the app shows up in "Apps & Features" and can be removed cleanly.

**This is a plan-only document, per the request's own "First just create the plan" line** — no
code changes are included; implementation is a separate, later step.

## Current state

- No installer, no setup project, no `.ico` application icon resource (`MyCommander.rc` has only
  the `VERSIONINFO` block — no `ICON` resource — so a shortcut to `MyCommander.exe` today shows
  Windows' generic executable icon).
- `MyCommander.vcxproj` sets no `<RuntimeLibrary>`, so both configurations build against MSVC's
  default — **dynamically-linked** (`/MD` Release, `/MDd` Debug), meaning the built `.exe` depends
  on the Visual C++ 2022 x64 Redistributable being present on the machine it runs on. On a clean
  Windows install without Visual Studio or that redistributable, `MyCommander.exe` fails to start
  with a missing-DLL error (`VCRUNTIME140.dll`/`MSVCP140.dll` not found) — this is the concrete,
  checkable "prerequisite" the issue's own wording names.
- The running app already keeps all of its own state under `%APPDATA%\MyCommander\` (`Config.h`'s
  `mycommander.ini`, `Session.h`'s `session.ini`, `CommandHistory.h`'s `history.txt`, `Log.h`'s
  `mycommander.log`) — none of that is installer-owned or touched by this plan; it's created lazily
  by the app itself on first run, same as today.
- `Version.h`'s `MC_VERSION_STR` is already the single compile-time source of truth for the
  version number, consumed by `MyCommander.rc`, `main.cpp --version`, and `Help.cpp`'s About
  section (KEY-004). The setup project reuses the same macro rather than adding a fourth copy.

## Design

### 1. A recommendation that removes most of the "prerequisite" problem: static CRT linking

Before designing prerequisite-*checking*, it's worth eliminating the prerequisite itself where
possible. Switching `MyCommander.vcxproj` (and `MyCommanderTests.vcxproj`, for consistency) from
the MSVC default dynamic runtime to **static** (`<RuntimeLibrary>MultiThreaded</RuntimeLibrary>` /
`MultiThreadedDebug` for Debug) removes the Visual C++ Redistributable dependency entirely — the
`.exe` becomes self-contained with no runtime-DLL prerequisite to check or install at all. This is
a one-line change per configuration, costs a few hundred KB of binary size (a console app, not a
concern here), and is fully consistent with this project's existing "no extra dependency" posture
(DEC-002/DEC-009). **Recommended**, and assumed by the rest of this plan — flagged here as the one
design choice most worth confirming before implementation, since it touches `MyCommander.vcxproj`
itself, not just the new setup project. If rejected, §3 below falls back to detecting and, if
missing, running a bundled `vc_redist.x64.exe` instead (Microsoft's own redistributable installer,
not something this project would build) — a strictly more complex path than simply not needing it.

### 2. New project: `Source/MyCommanderSetup/`

A fourth `.vcxproj` in `MyCommander.sln` (Application, v143 toolset, C++20, `x64` — matching
`MyCommander.vcxproj`'s own settings, `<RuntimeLibrary>MultiThreaded</RuntimeLibrary>` per §1),
building `MyCommanderSetup.exe`. Console subsystem, keyboard-only prompts — the same "raw Win32,
no GUI toolkit" character as the main app (DEC-002), rather than a separate Win32 windowed wizard,
which would be a second, unrelated UI technology stack for a one-time, few-screens task. A project
reference / build-order dependency on `MyCommander.vcxproj` (native MSBuild feature, not a new
build system) ensures `MyCommander.exe` is already built before `MyCommanderSetup` embeds it — see
§4.

Reuses `Source/MyCommander/PathUtil.h` directly (header-only, no `Console`/`Dialog` dependency,
already the established pattern `MyCommanderTests` follows for compiling `MyCommander`'s pure
modules into a second project) for long-path-safe path handling and Win32 error formatting. Does
**not** depend on `Console.h`/`Dialog.h`/`Config.h`/etc. — the installer's own UI is a handful of
plain `wprintf`/`ReadConsole` prompts, not a full-screen buffered UI; pulling in the double-buffer
renderer for that would be solving a problem the installer doesn't have.

### 3. Prerequisite checks

Run first, before touching the filesystem or registry:

- **Windows version** — `RtlGetVersion` (via `ntdll.dll`, the standard way to get the *true*
  OS version without the compatibility-shim lie `GetVersionEx` tells on newer Windows) confirms
  Windows 10 (build ≥ 10240) or later, matching the README's stated `Requirements: Windows 10/11`.
  Below that: report it and stop, no install attempted.
- **CPU architecture** — confirm a 64-bit OS (`IsWow64Process2` or simply that the installer
  itself, built `x64`, is running at all — a 32-bit-only Windows can't run it, so this is really
  "did the installer launch," not a separate runtime check).
- **VC++ Redistributable** — only relevant if §1's static-linking recommendation is *not* taken.
  If needed: check `HKLM\SOFTWARE\Microsoft\VisualStudio\14.0\VC\Runtimes\X64`'s `Installed`
  (DWORD, expect `1`) and `Version` values; if missing or below the toolset's minimum, prompt to
  install and run a bundled `vc_redist.x64.exe` (silently, `/install /quiet /norestart`) before
  continuing. With static linking (§1, recommended), this whole bullet — detection, bundling, and
  the extra few MB `vc_redist.x64.exe` would otherwise add to the installer — doesn't exist.
- **Disk space** — `GetDiskFreeSpaceExW` on the target install drive against a small, generous
  fixed budget (the app + docs are a few MB total; no need for a byte-accurate calculation here).
- **Existing installation** — check for the uninstall registry key from §6 below; if present,
  report the currently-installed version (reusing `Version.h`'s `MC_VERSION_STR` for the new one)
  and offer Upgrade (overwrite in place) / Cancel, rather than silently doubling up shortcuts or
  registry entries on a second run.

Any failed check prints a clear reason and exits with a non-zero code (so a future silent/scripted
install mode, if ever added, can detect failure) rather than partially installing.

### 4. Payload: embedded, not a side-by-side folder

`MyCommanderSetup.exe` embeds `MyCommander.exe`, `USERGUIDE.md`, and `LICENSE` as `RCDATA`
resources (a new `MyCommanderSetup.rc`) rather than shipping as an exe plus a loose folder of
files someone could separate by accident — a single-file installer is the more robust, more
"normal" experience.

**The embedded `MyCommander.exe` is always a fresh `Release` build, rebuilt immediately before the
installer packages it — never whatever happened to already sit in `Build\x64\Release\`, and never
a `Debug` build, regardless of which configuration `MyCommanderSetup` itself is currently being
built in.** Relying on `MyCommander.sln`'s ordinary project-build-order dependency (§2) isn't
enough on its own for this: an incremental solution build only rebuilds `MyCommander.vcxproj` if
MSBuild thinks something changed, and if `MyCommanderSetup` is built as `Debug`, a plain project
reference would build `MyCommander` as `Debug` too unless the solution's Configuration Manager
mapping is set up to force `Release` — an easy thing to get subtly wrong and never notice until
someone installs a Debug (unoptimized, `_DEBUG`-asserting) build by accident. Instead,
`MyCommanderSetup.vcxproj` gets its own `PreBuildEvent` — the same mechanism
`MyCommander.vcxproj`'s own `PreBuildEvent` already uses for `BumpBuildNumber.ps1`, so this is a
second use of an already-established pattern, not a new build-system concept — running a new
`Tools\RebuildReleaseForInstaller.ps1`. That script does two things, in order, every single time
`MyCommanderSetup` builds (Debug or Release, either one):

1. Invokes `msbuild "$(SolutionDir)MyCommander.sln" /t:MyCommander:Rebuild
   /p:Configuration=Release /p:Platform=x64` — the solution-level `Project:Rebuild` target syntax,
   run against the `.sln` specifically (per `CLAUDE.md`'s own documented `$(SolutionDir)`-resolves-
   wrong quirk when a `.vcxproj` is built directly outside the solution — the same reason every
   other build instruction in this project already goes through `MyCommander.sln`). `Rebuild`
   (clean, then build), not the incremental `Build` target — deliberately unconditional, matching
   the request's literal "shall be rebuilt" wording, so a stale or partially-up-to-date `Release`
   folder can never be silently reused.
2. Fails loudly (non-zero exit, which fails `MyCommanderSetup`'s own build in turn) if that
   rebuild fails — a broken `MyCommander` build must never end up quietly packaged into a
   "successfully built" installer.

Only after that does the existing copy step run: `Build\x64\Release\MyCommander.exe` plus
`USERGUIDE.md`/`LICENSE` from the repo root are copied into `MyCommanderSetup`'s own
resource-input folder before it compiles, which is why `MyCommanderSetup` must also still build
*after* `MyCommander` in the solution's own dependency graph (§2) — the rebuild step above is what
guarantees *freshness and the right configuration*; the project dependency is what guarantees
*ordering* within one solution build. At runtime, `FindResource`/`LoadResource`/`LockResource`
extract each embedded file straight to the chosen install folder.

The trade-off, named explicitly rather than left as a surprise: every build of `MyCommanderSetup`
now also does a full `Release` rebuild of `MyCommander` from scratch, which is measurably slower
than an incremental build. That's the deliberate cost of the request's freshness guarantee, not an
oversight — `MyCommanderSetup` is built far less often than `MyCommander` itself during ordinary
development (it's a packaging step, not something touched on every edit-compile-test cycle), so
this cost is paid rarely in practice.

### 5. Install flow

1. Print a short welcome banner with the version being installed (`MC_VERSION_STR`).
2. Run prerequisite checks (§3); stop with a clear message if any fail.
3. Propose the default install folder, **`%LOCALAPPDATA%\Programs\MyCommander`** — a per-user
   location that needs no administrator rights to write to, consistent with the main app's own
   SEC-002 ("never require elevation for ordinary operation") extended sensibly to installing it
   in the first place. Accept it with Enter, or let the user type a different path. (An
   all-users/`%ProgramFiles%` install mode, which *would* need elevation, is explicitly out of
   scope for this plan — flagged in **Risk / scope notes** as a possible later addition, not
   something this plan designs, since the request doesn't ask for it and it changes the
   elevation/UAC story considerably.)
4. Extract the embedded payload (§4) into that folder, plus a copy of `MyCommanderSetup.exe`
   itself (reused as the uninstaller — see §7).
5. Ask "Create a desktop shortcut? [Y/n]" (default yes, since the request explicitly asks for
   one) — see §6.
6. Write the uninstall registry entry (§7).
7. Print where it was installed and how to run it (the desktop shortcut, or the full path).

No progress dialog/animation is needed — every step here is near-instant (a handful of small file
copies and a couple of registry writes), unlike `FileOps`'s bulk copy/move engine, which is why
this doesn't reuse `FileOps.h`'s progress-reporting machinery.

### 6. Desktop shortcut

Created via `IShellLinkW`/`IPersistFile` (`shell32.lib`/`ole32.lib` — the exact same two Win32 COM
libraries `main.cpp` already links for drag-and-drop and `ShellExecuteExW` elevation, `#pragma
comment(lib, ...)` in the setup project the same way `FileOps.cpp`/`main.cpp` already do it, no
new dependency), targeting the installed `MyCommander.exe`, with its working directory set to the
install folder. Written to `%USERPROFILE%\Desktop\MyCommander.lnk` (`SHGetKnownFolderPath` with
`FOLDERID_Desktop`, not a hardcoded path — respects a redirected/OneDrive-relocated Desktop
folder).

**Icon note:** the shortcut will show `MyCommander.exe`'s own icon, which today is Windows'
generic default (§ **Current state** — no `.ico`/`ICON` resource exists yet). A real application
icon is a separate, small prerequisite this plan flags but doesn't design: someone needs to
supply or draw a `MyCommander.ico`, added to `MyCommander.rc` as `IDI_APPICON ICON
"MyCommander.ico"`, before the shortcut looks like anything other than a blank/generic exe icon.
Not blocking — the installer and shortcut both work correctly either way — just worth doing before
or alongside implementation so the "launcher icon" the request asks for actually looks like one.

### 7. Uninstall registration

Writes to `HKCU\Software\Microsoft\Windows\CurrentVersion\Uninstall\MyCommander` (per-user `HKCU`,
matching the per-user install location — no elevation needed to write it, unlike the
`HKLM`-rooted equivalent an all-users install would require): `DisplayName`, `DisplayVersion`
(`MC_VERSION_STR`), `Publisher` ("Attila Gallai", matching `MyCommander.rc`'s own `CompanyName`),
`InstallLocation`, `UninstallString` (the copied `MyCommanderSetup.exe --uninstall`, quoted via
`PathUtil.h`-style safe quoting), `EstimatedSize`, and `NoModify`/`NoRepair` set to `1` (this
installer doesn't support modify/repair, only install/uninstall — telling Windows that up front
avoids Windows offering buttons for modes that don't exist).

`MyCommanderSetup.exe --uninstall` (the same executable, a second entry point via `argv`
dispatch — the exact shape `main.cpp` already uses for its own `--version` flag, and `Elevation.h`
uses for its hidden `--elevated-*` flags) reads the install location back out of that same
registry key, deletes the installed folder, the desktop shortcut, and the registry key itself, and
reports success. This is why §4 copies `MyCommanderSetup.exe` itself into the install folder —
Windows' "Apps & Features" `UninstallString` must point at something that still exists after the
original setup download/USB stick/whatever is gone.

### 8. Command-line flags (only what's needed for §3/§7, nothing speculative)

- *(no flag)* — interactive install, the default double-click experience.
- `--uninstall` — the uninstall path (§7); not shown/offered interactively, only reachable via the
  registry's `UninstallString` or a direct command-line invocation.
- `--silent` is **explicitly out of scope for this plan** (flagged, not designed) — a genuinely
  silent/unattended mode needs its own answer for "where do failures go" (exit code alone, or a
  log file) that the request doesn't ask for; easy to add later without redesigning anything above.

## Files touched (once implemented — none touched by this plan document itself)

- `Source/MyCommanderSetup/` — new directory: `main.cpp` (argv dispatch, welcome/prereq/install/
  uninstall flow), `MyCommanderSetup.rc` (embedded `RCDATA` payload + its own small
  `VERSIONINFO`, reusing `Version.h`), `MyCommanderSetup.vcxproj`/`.vcxproj.filters` (including its
  `PreBuildEvent` calling `Tools\RebuildReleaseForInstaller.ps1`, §4).
- `Tools/RebuildReleaseForInstaller.ps1` — new script: force-rebuilds `MyCommander.vcxproj` as
  `Release|x64` through `MyCommander.sln` before every `MyCommanderSetup` build (§4), matching
  `Tools/BumpBuildNumber.ps1`'s existing role as a `PreBuildEvent`-invoked script.
- `MyCommander.sln` — add the new project, wire its build-order dependency on `MyCommander.vcxproj`
  (§2/§4).
- `Source/MyCommander/MyCommander.vcxproj` (and `Source/MyCommanderTests/MyCommanderTests.vcxproj`,
  for consistency) — add `<RuntimeLibrary>MultiThreaded[Debug]</RuntimeLibrary>` per §1, **if** that
  recommendation is accepted.
- `Source/MyCommander/MyCommander.rc` — add an `ICON` resource once a `.ico` asset exists (§6's
  icon note); not required for the installer itself to work.
- `README.md`/`USERGUIDE.md` — an "Installing" section once implemented (today both only describe
  building from source or running the built `.exe` directly).
- `AIPrompt/my-commander.md` — likely a new requirement ID or small block for the installer once
  implemented (there isn't an existing ID this clearly extends, unlike most `IS-000N` fixes, which
  refine an already-`Implemented` row) — worth deciding a new ID prefix/number as part of
  implementation, not this plan.
- `Source/MyCommanderTests/` — a `SetupLogicTests.cpp` for whichever parts of `main.cpp`'s new
  logic get extracted into a pure, `Console`-free form (see **Test plan** below) — matching every
  other module's own `*Tests.cpp` precedent.

## Test plan

Following this codebase's established split (pure logic gets a `TEST_CASE`; OS-integration code
that would have real side effects on the test machine gets manual verification only — the exact
precedent `Elevation.h`'s `RunElevated` already set):

- **Pure and testable, once extracted out of `main.cpp` into their own functions:** building the
  `UninstallString`/registry value set from an install path and version string; computing whether
  a detected Windows build number meets the minimum; formatting the "already installed, current
  version X, installing version Y" upgrade message. These take plain data in, plain data out — no
  registry/filesystem/COM calls themselves — so they're unit-testable the same way
  `BuildElevatedHelperParameters` already is for `Elevation.cpp`.
- **Not automatable, verified manually instead (matching `RunElevated`'s own precedent, which an
  automated test also can't safely exercise):** the actual `RtlGetVersion` check, registry
  reads/writes, `IShellLinkW` shortcut creation, and file extraction — each of these would need to
  genuinely modify a real machine's filesystem/registry/Desktop to test end-to-end, which is not
  something an automated CI-style test run should do. Manual test plan once implemented: install
  on a clean VM (no prior MyCommander install, ideally no Visual Studio either, to genuinely
  exercise §1/§3's prerequisite story), confirm the desktop shortcut launches the app, confirm
  "Apps & Features" lists it correctly, confirm uninstalling removes the folder/shortcut/registry
  key cleanly, then repeat as an *upgrade* over an existing install.

## Risk / scope notes

- **Per-user install only, no admin/elevation, in this plan.** An all-users (`%ProgramFiles%`)
  install mode would need `ShellExecuteExW`'s `runas` verb (the same mechanism `Elevation.h`
  already uses for one-off elevated file operations) and an `HKLM` registry write instead of
  `HKCU` — a real design difference (SEC-001/002/003's whole "elevation is scoped to one already-
  justified action, never the app's own general operation" posture would need to be reasoned about
  for "elevate to install" too), not just a flag flip. Deliberately left out of this plan; the
  per-user path already satisfies "install required elements and this software" and "create a
  desktop launcher icon" without ever needing a UAC prompt at all.
- **No MSI/Windows Installer database, no WiX/Inno/NSIS.** The request says "built up from native
  C++" — a hand-rolled `.exe` installer is the literal reading of that, and matches DEC-002/
  DEC-009's existing "no third-party tooling" posture project-wide. The trade-off: no
  Programs-and-Features-native rollback-on-failure semantics MSI would give for free; this plan's
  install steps are simple/few enough (file copy, shortcut, two registry writes) that a partial
  failure is easy to reason about and clean up by re-running uninstall, but it's worth naming this
  as the trade-off being made, not an oversight.
- **The static-linking recommendation (§1) is the one part of this plan that touches the existing
  `MyCommander`/`MyCommanderTests` projects, not just new setup-project files** — flagged clearly
  as the single change most worth confirming before implementation starts, since everything else
  in this plan is additive (a new project) and reversible on its own.
- **No `.ico` asset exists yet** (§6) — the installer and shortcut work without one, but "launcher
  icon" reads best once a real icon exists; flagged, not solved, by this plan.
- **`--silent` unattended installs, all-users installs, Start Menu shortcuts, and modify/repair
  support are all explicitly out of scope** — none were asked for, and each is a real, separate
  design decision (see §8, §6, and the MSI note above) rather than a small addition, so this plan
  doesn't pretend they're free.

## Status

**Implemented**, per the design above. Both flagged decisions were confirmed before implementation
started: §1's static linking — accepted — and §6's missing icon — accepted as a known gap, proceed
without one for now.

- `MyCommander.vcxproj`/`MyCommanderTests.vcxproj` gained `<RuntimeLibrary>MultiThreaded[Debug]
  </RuntimeLibrary>` (§1). Confirmed via `dumpbin /dependents` that the built `MyCommander.exe` no
  longer imports `VCRUNTIME140.dll`/`MSVCP140.dll` — only `KERNEL32`/`USER32`/`SHELL32`/`ole32`/
  `VERSION`, none of them requiring the VC++ Redistributable.
- `Source/MyCommanderSetup/` was added exactly as designed: `SetupLogic.{h,cpp}` (pure — install-
  path defaulting, Windows-version-support check, uninstall-registry value construction, upgrade
  message — 12 new `TEST_CASE`s in `SetupLogicTests.cpp`) and `main.cpp` (the OS integration:
  `RtlGetVersion` via `GetProcAddress` on `ntdll.dll`, `GetDiskFreeSpaceExW`, `RCDATA` extraction,
  `IShellLinkW`/`IPersistFile` shortcut creation, `HKCU\...\Uninstall` registration). Reuses
  `PathUtil.h` and `Process.h`/`.cpp` (`QuoteCommandLineArgument`) directly from
  `Source/MyCommander/`, as planned — no duplicated quoting logic.
- §4's freshness guarantee (INST-006) is implemented as designed:
  `Tools/RebuildReleaseForInstaller.ps1`, wired as `MyCommanderSetup.vcxproj`'s `PreBuildEvent`,
  force-`Rebuild`s `MyCommander` as `Release|x64` through `MyCommander.sln` (via
  `/t:MyCommander:Rebuild`, using `vswhere.exe` to locate `MSBuild.exe`) and stages the payload
  into `Source/MyCommanderSetup/payload/` before `MyCommanderSetup.rc` ever compiles. That
  staging folder is gitignored except for a `README.txt` placeholder explaining why.
- One implementation detail not spelled out in the original design: **uninstalling a running
  copy of the setup exe.** `MyCommanderSetup.exe` is copied into the install folder specifically
  to serve as the uninstaller (§7), but a running exe's own file is locked on Windows and can't be
  deleted synchronously by itself. Since this is a per-user, non-elevated install (§1's own
  reasoning in **Risk / scope notes**), `MOVEFILE_DELAY_UNTIL_REBOOT` isn't usable (it needs
  `SE_RESTORE_NAME`, effectively administrator rights). `RunUninstall` therefore deletes
  everything else in the install folder immediately (every file except its own running exe),
  removes the shortcut and registry key synchronously, then hands off to a short detached
  `cmd.exe /c ping -n 2 127.0.0.1 >nul & rmdir /s /q <path>` helper (`CREATE_NO_WINDOW |
  DETACHED_PROCESS`) to remove the now-single-file folder once this process exits and releases
  its own lock — the standard, well-known workaround for a non-elevated self-deleting installer.
- `AIPrompt/my-commander.md` gained a new `## 23. Installation Requirements` section (`INST-001`
  through `INST-006`, all `Implemented`) — inserted before the former `## 23. Proposed MVP`
  (renumbered `## 24.` onward through `## 29. Change Procedure`) rather than appended at the end,
  so it sits alongside the other requirement-category sections it belongs with. `CLAUDE.md`'s own
  requirement-ID-namespace list and Source-layout bullet list were both updated to mention
  `INST-`/`Source/MyCommanderSetup/`.
- **Verified end-to-end on this machine, not just built**: ran the real `MyCommanderSetup.exe`
  (piped stdin, accepting every default) — confirmed the three files extracted correctly, the
  desktop shortcut's target/working-directory/description via `WScript.Shell` COM, and the
  `HKCU\...\Uninstall` registry values, all matched what was expected. Ran it a second time to
  confirm the already-installed/upgrade-prompt path triggers correctly. Ran `--uninstall` and
  confirmed the shortcut, install folder (including the self-deleting exe, after the detached
  helper's brief delay), and registry key were all removed cleanly. This is real local state on a
  real machine (`%LOCALAPPDATA%\Programs\MyCommander`, the real Desktop, real `HKCU`) — chosen
  deliberately over skipping verification, since every effect is per-user, fully reversible, and
  was in fact fully reversed by the same uninstall path this plan designed.
- Full solution build (both `Debug|x64` and `Release|x64`, including `MyCommanderSetup` itself in
  each) and the full test suite (341 cases, 11,525 checks, 0 failed — up from 329/11,508, the 12
  new `SetupLogicTests.cpp` cases) both verified clean.
- No deviation from scope: all-users/elevated install, `--silent`, Start Menu shortcuts, modify/
  repair, and MSI/WiX/Inno tooling remain out of scope, exactly as designed.
