# Fix Plan — IS-0021 (give the application, its installer and its shortcuts the Windows icon)

Source: `AIPrompt/issue_fix_requests.md`:

> Please add the Windows icon files to the app, to be able to use when the software is browsing, running, and so on.
> Add it to the install app too.
> Relative path of the icon files: `Resources\mycommander_extracted\ico`
> `app_16.ico`, `app_24.ico`, `app_32.ico`, `app_48.ico`, `app_64.ico`, `app_96.ico`, `app_128.ico`, `app_256.ico`

This is an enhancement, not a defect. Today neither executable has an icon: `MyCommander.rc` and `MyCommanderSetup.rc` carry
only version information, so Explorer, the taskbar, Alt+Tab, Task Manager and the desktop shortcut all show Windows'
generic executable icon (FEA-0006's plan and the installer's own comment already flag "no app icon exists yet").

## What the icon files actually are (checked, not assumed)

I parsed all eight files. This decides the design:

| Finding | Consequence |
|---|---|
| Every `app_N.ico` is a **multi-size** icon with the same 7 images: 16, 24, 32, 48, 64, 128, 256 px, all 32-bit PNG-compressed | The eight files overlap heavily (65–125 KB each). Embedding all eight as separate icons would be wrong. |
| The `N` in the file name is the **size the artwork was drawn for**; the other six sizes in that file are the same drawing resampled (`Resources/slice_mycommander_icons.py` crops each variant from a rendered sheet, then resizes it) | The sharp image for size N is the size-N image inside `app_N.ico`. A single file, such as `app_16.ico`, would give a blurry 256 px image. |
| There is **no 96 px image** in any file (96 is not in the ICO size list the script writes), but `png/app_96.png` holds the 96 px artwork | Explorer's *Large icons* view is 96 px, so it matters for "browsing". The 96 px image has to be made from that PNG. |
| The art is a two-pane window with folder and file rows, transparent corners (measured at 16, 32, 48 and 256 px), padded to a square rather than stretched | Usable as it is. The 16 px image has a few semi-transparent edge pixels; the manual test checks it. |
| Windows picks the application icon from the **first** icon group in the executable | One icon group with all sizes, not eight groups. |

## Design

### D1 — One merged, multi-size icon (`Resources/mycommander.ico`)

- Built by a new **`Tools/MakeAppIcon.py`**, which takes, for each size, the image that was drawn for it:

| Size | Source |
|---|---|
| 16, 24, 32, 48, 64 | the image of that size inside `app_16.ico` … `app_64.ico` (PNG bytes copied unchanged) |
| 96 | `png/app_96.png`, scaled to a square 96 × 96 image with the transparent padding kept (the only step that needs Pillow) |
| 128, 256 | the image of that size inside `app_128.ico` and `app_256.ico` |

- Output: 8 images, about 140 KB. The result is **committed**, so a build never needs Python or Pillow; the script is for
  when the artwork changes. `--check` prints the entries of any `.ico` (size, bit depth, PNG or not).

### D2 — The application (`MyCommander.exe`)

- `MyCommander.rc` gains `IDI_APPICON ICON "..\\..\\Resources\\mycommander.ico"` with `#define IDI_APPICON 1` (the project
  has no `resource.h`; the define sits in the `.rc`).
- This is the whole fix for "browsing" (Explorer's views, shortcuts, the Start menu, Task Manager's Details tab) and for
  "running" under the classic console host (title bar, taskbar, Alt+Tab take the executable's icon).
- No runtime `WM_SETICON` call: the console host needs none, and under Windows Terminal it would have no visible effect
  (see Risks).

### D3 — The installer (`MyCommanderSetup.exe`)

- `MyCommanderSetup.rc` gets the same icon (`IDI_APPICON` added to its `resource.h`), so the installer and the copy of it
  kept as the uninstaller show the application icon in Explorer.
- **Desktop shortcut:** `CreateShortcutFile` already leaves the icon to the target, so the shortcut now shows the installed
  `MyCommander.exe`'s icon with no code change. The stale comment above `CreateDesktopShortcut` is corrected, and the manual
  test confirms the executable is fully written before the shortcut is created, so the shell cannot cache a generic icon.
- **Apps & Features:** `SetupLogic`'s `BuildUninstallInfo` gains `displayIcon` (`<install path>\MyCommander.exe,0`), and
  `main.cpp` writes it as the `DisplayIcon` value of the Uninstall key (INST-005).
- No other file is added to the install folder in this plan (open question 2).

### D4 — Not in scope

- Plugin DLLs and the test executable get no icon. The other icons in `mycommander_extracted/ico` (folder, file types,
  toolbar actions) are not used by the app yet, and nothing here uses them.

## Files touched

| File | Change |
|---|---|
| `Tools/MakeAppIcon.py` (new) | Builds `Resources/mycommander.ico`; `--check` |
| `Resources/mycommander.ico` (new, generated, committed) | The merged icon |
| `Source/MyCommander/MyCommander.rc` | `IDI_APPICON` icon resource |
| `Source/MyCommanderSetup/MyCommanderSetup.rc`, `resource.h` | The same icon |
| `Source/MyCommanderSetup/SetupLogic.{h,cpp}`, `main.cpp` | `displayIcon` in `UninstallInfo`, written as `DisplayIcon`; the corrected comment |
| `Source/MyCommanderTests/IconAssetTests.cpp` (new), `SetupLogicTests.cpp`, `MyCommanderTests.vcxproj` | Tests below |
| `AIPrompt/my-commander.md`, `README.md`, `USERGUIDE.md`, `CLAUDE.md`, `AIPrompt/ManualTests.md`, `AIPrompt/features/FEA-0006-plan.md`, changelog | See Documentation updates |

## Tests

- **`IconAssetTests.cpp`, the icon file:** `Resources/mycommander.ico` (located from `__FILE__`, skipped with a note if the
  folder is absent) has a valid header; exactly the sizes 16, 24, 32, 48, 64, 96, 128 and 256, once each; every image is a
  PNG whose own width and height equal the directory entry's size; 32 bits per pixel; total size under 300 KB.
- **`IconAssetTests.cpp`, the executable:** the `MyCommander.exe` built next to the test executable (skipped with a note if
  it is not there) is loaded as a data file; it has an `RT_GROUP_ICON` resource with ID 1 listing the same eight sizes, and
  every referenced `RT_ICON` exists. This catches a broken or missing `ICON` line in the `.rc`.
- **`SetupLogicTests.cpp`:** `BuildUninstallInfo` returns `displayIcon` equal to the install path plus
  `\MyCommander.exe,0`, for a path with spaces and a Unicode path.
- **Not automated:** how the icon looks, the taskbar and the installer, as for the existing installer code (INST-005's own
  precedent). Covered by a new manual test.
- **Manual (new `MT` entry):** Explorer in Small (16), Medium (48), Large (96) and Extra large (256) icon views and Details,
  for `MyCommander.exe` and `MyCommanderSetup.exe`; taskbar, title bar and Alt+Tab when run in the classic console host;
  Task Manager (Details); the desktop shortcut after installing; the Apps & Features entry; the 16 px image's edges on a
  light and a dark background; Windows Terminal (the tab keeps the terminal's own icon, see Risks). After replacing the
  executable, refresh Explorer's icon cache (restart Explorer) before judging.

## Documentation updates

- `AIPrompt/my-commander.md`: new row **UI-018** (the application carries a multi-size Windows icon) and **INST-008** (the
  installer, the desktop shortcut and the Apps & Features entry show it), both IN-MVP / Implemented when done; a version
  bump (patch) and a history paragraph.
- `README.md`: a short note in the build section on `Resources/`, the committed `mycommander.ico` and `Tools/MakeAppIcon.py`
  (when to rerun it, that it needs Pillow only for the 96 px image); the install section mentions the icon.
- `USERGUIDE.md`: one sentence that the program icon is used by Explorer, the taskbar and the shortcut, and that a
  Windows Terminal tab keeps the terminal's own icon.
- `CLAUDE.md`: the source layout gets `Resources/` and `Tools/MakeAppIcon.py`; the installer paragraph notes the icon,
  `displayIcon` and that the desktop shortcut inherits the executable's icon; the Testing list adds `IconAssetTests.cpp`.
- `AIPrompt/features/FEA-0006-plan.md`: its "icon note" (no icon yet) points to IS-0021 as the resolution.
- `AIPrompt/ManualTests.md`: the new entry above.
- `AIPrompt/fixes/IS-0021-fixplan.md`: status when done.
- `3_Reports/ChangeLogs/changelog_YYYY-MM-DD.md`: an entry at implementation time.
- `Source/MyCommander/Version.h`: bumped by the build (the patch number is a manual decision, as usual).

## Phasing

1. `Tools/MakeAppIcon.py`, the generated `Resources/mycommander.ico` and the asset test.
2. The `.rc` icon for `MyCommander.exe` and the executable-resource test.
3. The installer: its icon, `displayIcon`, the comment, and the tests.
4. Documentation, spec, manual test, changelog.

## Risks

- **Windows Terminal ignores process icons.** Its tab and taskbar icon come from the terminal's profile, not from the
  program inside it, so "running" shows the new icon only in the classic console host. This is a limit of the terminal, not
  of the app. The real fix is a Windows Terminal profile with an `icon`, which needs a standalone `.ico` in the install
  folder (open question 2).
- **Stale icon cache.** Explorer caches icons per executable, so the old generic icon can linger after the first build.
  The manual test says how to refresh it; it is not a defect.
- **PNG-compressed small sizes.** All images are PNG inside the icon, which Windows 10 and 11 handle. If a view ever shows
  a wrong or blank icon, the fallback is to store the 16 to 48 px entries as classic bitmaps; the generator gets a flag for
  that only if needed.
- **Art quality at 16 px.** The images are crops of a rendered sheet, so the smallest sizes depend on that crop. The manual
  test judges them; replacing weak ones is an artwork task and does not change the design here.
- **Size.** The executable and the installer each grow by roughly 140 KB.
- **Untracked folder.** `Resources/` is not in git yet; the plan commits `mycommander.ico` and the files the generator
  reads (open question 3).

## Decisions (2026-10-16)

The four open questions were answered:

1. **One merged icon**, as recommended.
2. **Both offered extras are in scope:** the installer installs a standalone `mycommander.ico` beside the executable, and it
   offers a per-user **Windows Terminal profile fragment** with the icon (default yes, like the shortcut prompt). Consequences
   for the design: `Resources/mycommander.ico` is also embedded in the setup as `RCDATA` (`IDR_PAYLOAD_ICON`); `SetupLogic`
   gains `TerminalFragmentFolder`/`TerminalFragmentPath`, `BuildTerminalFragmentJson` and `JsonEscape` (pure, tested); the
   fragment lives outside the install folder (`%LOCALAPPDATA%\Microsoft\Windows Terminal\Fragments\MyCommander`), so
   `RunUninstall` removes it explicitly; new spec row INST-009.
3. **Commit all existing files in `Resources/`** (including the contact sheet, the prompt, the slicing script and the other
   icons), at commit time.
4. **Patch** version bump (2.13.0 to 2.13.1).

## Status

Planned 2026-10-16. **Implemented 2026-10-16** (spec 1.50.0, version 2.13.1) with the four decisions above.

Delivered: `Tools/MakeAppIcon.py` and the generated `Resources/mycommander.ico` (8 sizes, 134,531 bytes); `IDI_APPICON` in
`MyCommander.rc` and `MyCommanderSetup.rc`; the setup also embeds the icon (`IDR_PAYLOAD_ICON`) and installs it as
`mycommander.ico`; `DisplayIcon` in the Uninstall key; the Windows Terminal profile fragment (prompted, default yes) and its
removal on uninstall; `SetupLogic`'s `displayIcon`, `TerminalFragmentFolder/Path`, `BuildTerminalFragmentJson`, `JsonEscape`;
seven new tests (icon file, executable icon group, `displayIcon`, fragment path, JSON escaping, two JSON round trips);
spec rows UI-018, INST-008 and INST-009; README, USERGUIDE, CLAUDE.md, the FEA-0006 note and manual test MT-067.

Checked after the build: all four executables (`MyCommander.exe` and `MyCommanderSetup.exe`, Debug and Release) contain
icon group 1 with the sizes 16, 24, 32, 48, 64, 96, 128 and 256, and both setups embed the 134,531-byte `.ico` payload.
Full solution build succeeds; `MyCommanderTests.exe`: 1,042 cases, 0 failed, in Debug and Release.

Differences from the plan:

- **The setup's icon payload is referenced straight from `Resources\`**, not staged through `payload\`, because it is a
  committed asset rather than a build output.
- **The Windows Terminal profile is written only if the icon was installed** (the profile points at it); a failed icon
  extraction is a warning, like the plugins.
- **Commit scope (decision 3):** everything under `Resources/` is committed with this change, at commit time.

**Not verified in this session:** how the icon looks (Explorer sizes, taskbar, Alt+Tab, shortcut, Apps & Features) and the
interactive installer flow, including that a real Windows Terminal picks up the profile fragment. All of it is manual test
MT-067; the fragment follows Microsoft's documented per-user fragment location and format but has not been loaded by a
terminal here.
