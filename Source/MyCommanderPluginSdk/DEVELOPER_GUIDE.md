# MC Plugin SDK — Developer Guide

This is the plugin-author-facing guide for extending MyCommander with a native, DLL-based MC
Plugin. It covers the complete MC Plugin architecture, all three phases (see
`AIPrompt/features/FEA-0002-plan.md` for the full design and phasing rationale): plugin
discovery/loading, menu integration, dialogs, logging, theme access, keyboard shortcuts,
Help/Markdown content, panel content, and inter-plugin services — plus the generic options-dialog
and side-scoped-panel host functions FEA-0007 (the ZIP plugin, `AIPrompt/features/
FEA-0007-plan.md`) added, equally usable by any plugin, not just that one.

Every struct and function mentioned here is declared in one of three headers, right next to this
file: `McPluginAbi.h` (identity, lifecycle), `McPluginTypes.h` (shared plain-data types), and
`McPluginHost.h` (the function-pointer struct the host hands you). This guide is the readable
narrative form of those headers — if something here and a header ever disagree, the header is
correct (open an issue against the guide, not the other way around).

## 1. Getting started

You need a compiler that can produce a 64-bit Windows DLL and understands C++17 or later (the
host itself is built with MSVC/v143, but the plugin ABI is plain C, so any compatible compiler
works). No SDK library to link against — everything is header-only.

The smallest possible plugin is one export:

```cpp
#include "McPluginAbi.h"

extern "C" __declspec(dllexport) const McPluginMetadata* __stdcall McPlugin_GetInfo(void) {
    static const McPluginMetadata info = {
        sizeof(McPluginMetadata),
        MC_PLUGIN_API_VERSION_MAJOR, MC_PLUGIN_API_VERSION_MINOR,
        MC_PLUGIN_MAGIC,
        L"com.example.myplugin", L"My Plugin", L"Does nothing yet.", L"0.1.0", L"You",
        0,  // no capabilities declared yet
    };
    return &info;
}
```

Build it, drop the DLL into `Plugins\` next to `MyCommander.exe`, and start MyCommander. This
plugin is *valid but inert* — the host identifies it, logs that it loaded, and does nothing
further with it (no `McPlugin_Init` export means the host never tries to activate it). That's the
whole discovery/identification path in miniature; everything past this point is about doing
something once you're active.

Copy `Source/ExamplePlugin/` as your starting point for anything beyond this — it's a complete,
minimal, working plugin (one menu item, one shortcut) with its own standalone `.vcxproj`.

## 2. The ABI contract

Nothing that crosses the boundary between MyCommander and your plugin is a C++ class or
template — every struct is plain, fixed-layout data, and every callback is a plain
`extern "C"`, `__stdcall` function pointer. This isn't a stylistic choice: C++ vtables aren't a
stable ABI across compilers or even across different versions of the same compiler's runtime
library, so a plugin built with a slightly different toolchain than the host risks silent memory
corruption, not a clean failure, if the boundary were C++ classes. Plain C sidesteps that
entirely. Your plugin's own *internals* can be as much C++ as you like — only what you `export`
has to stay C-shaped.

**Versioning.** `MC_PLUGIN_API_VERSION_MAJOR`/`MINOR` (currently 1.0) describe the ABI this guide
documents. Declare the exact version you were built against in your own `McPluginMetadata`:

- The host rejects your plugin outright if your **major** version doesn't match its own exactly —
  a major bump means something broke compatibility (a struct field removed or reordered, a
  callback's signature changed), and there's no safe way to guess around that.
- The host accepts a plugin whose **minor** version is less than or equal to its own — a smaller
  minor version means you were built against an *older* SDK, and every field/function you used
  still means what it meant then, since minor bumps are always purely additive (new fields
  appended to the end of a struct, new optional exports).
- A plugin declaring a **newer** minor version than the host knows about is rejected — the host
  has no way to know what a newer minor version might mean, so it doesn't guess.

`structSize` (on `McPluginMetadata` and `McThemeSnapshot`) exists for the same reason: it lets the
host read only the fields it was compiled to know about, safely, even if your plugin was built
against a newer minor version that appended fields past what this particular host understands.
Always set it to `sizeof(...)` of the struct as *you* compiled it — never hardcode a number.

## 3. Metadata reference

Every field of `McPluginMetadata`, returned by your one required export, `McPlugin_GetInfo`:

| Field | Required? | Notes |
|---|---|---|
| `structSize` | Yes | Always `sizeof(McPluginMetadata)` |
| `apiVersionMajor`/`Minor` | Yes | `MC_PLUGIN_API_VERSION_MAJOR`/`MINOR` as you compiled against them |
| `magic` | Yes | Always `MC_PLUGIN_MAGIC` — the host's very first, cheapest check that this DLL is an MC Plugin at all |
| `id` | Yes | Stable, unique. Reverse-DNS style recommended (`com.example.myplugin`) — this is the key your menu items, log lines, and (once available) service registrations are all filed under |
| `name` | Yes | Display name |
| `description` | Recommended | One line is fine |
| `version` | Recommended | Your own plugin's version, free-form — unrelated to the API version above |
| `author` | Recommended | |
| `capabilities` | Yes (can be `0`) | A bitmask of `McPluginCapability` — see below |

**Capabilities**, and the export each one requires:

- `kMcPluginCapMenu` — you must export `McPlugin_OnMenuAction`. Declaring this bit without the
  export is a load-time warning (logged, your plugin still activates, the menu integration just
  never calls anything).
- `kMcPluginCapShortcuts` — you must export `McPlugin_OnShortcut`, same rule.
- `kMcPluginCapHelp` — you must export `McPlugin_GetHelpMarkdown`, same rule. See "Help content"
  under §5 below.
- `kMcPluginCapPanel` — you must export all three of `McPlugin_OnPanelDraw`/`OnPanelKey`/
  `OnPanelClick`, or `RequestPanelContent` simply fails (`false`) rather than partially working.
  See "Panel content" under §5 below.
- `kMcPluginCapServices` — informational only; `PublishService`/`QueryService` need no capability
  bit to call. See "Inter-plugin services" under §5 below.

Every string pointer in `McPluginMetadata` must stay valid for as long as your DLL is loaded —
a string literal (the normal case) or a buffer you own and never free before `McPlugin_Shutdown`
returns. The host never copies these out immediately; it reads them when it actually needs to
(logging, the menu bar, comparing against `Config::disabledPluginIds`).

`McPluginMetadata::version` also appears next to your plugin's name in the host's own F1 help
screen, if you declare `kMcPluginCapHelp` (see "Help content" under §5 below).

### Giving your plugin DLL file-version info (optional)

The host's own `MyCommander.exe` carries an embedded Windows file-version resource
(`MyCommander.rc`), readable via Alt+Enter (File Properties) exactly like any other PE file —
and `FileInfo.cpp`'s reader is already fully generic, not MyCommander-specific, so it reads the
same kind of resource from a plugin's own `.dll` with no host-side change needed at all. The only
reason this doesn't already work for your plugin is that your `.dll` simply has no such resource
yet.

`McPluginVersion.h` (in this SDK directory) is a small, header-only helper that turns four numeric
macros into both the narrow string a `.rc` file's `FileVersion`/`ProductVersion` values need and
the wide string `McPluginMetadata::version` needs — so both describe the same version, the same
single-source-of-truth shape `Source/MyCommander/Version.h` already gives the host application
itself. To use it:

1. In your own plugin's `Version.h`, define `MCPLUGIN_VERSION_MAJOR`/`MINOR`/`PATCH`/`BUILD`, then
   `#include "../MyCommanderPluginSdk/McPluginVersion.h"`.
2. Copy `PluginVersionTemplate.rc` (this SDK directory) into your project, rename it, fill in the
   five placeholder `VALUE` strings (`CompanyName`/`FileDescription`/`InternalName`/
   `LegalCopyright`/`OriginalFilename`/`ProductName`) to match your own plugin, and add it to your
   `.vcxproj` as a `ResourceCompile` item.
3. Use `MCPLUGIN_VERSION_WSTR` as your `McPluginMetadata::version` in `McPlugin_GetInfo`, so the
   two never drift apart.

See `Source/ZipPlugin/Version.h` and `Source/ZipPlugin/ZipPlugin.rc` for a complete, real,
already-wired-up example of this exact pattern. A plugin with no `.rc` file at all simply shows no
version info on Alt+Enter — `FileInfo.cpp` degrades gracefully on any PE file with no
`VERSIONINFO` resource, the same as before this mechanism existed.

## 4. Lifecycle

Nine states, in order: Discovered → Validated → Loaded → Initialized → Active → Deactivated →
Shutdown → Unloaded. You never set your own state — the host drives this machine and you just
implement the two optional callbacks that sit at its two interesting transitions:

- **`McPlugin_Init(const McPluginHost* host)`** — Initialization → Activation. Called once, after
  identification and version validation already succeeded. `host` is yours to keep for your
  entire remaining lifetime — the host doesn't hand it to you again, and it stays valid until your
  own `McPlugin_Shutdown` returns. Return `false` to abort your own activation (logged by the
  host as an initialization failure); other plugins are entirely unaffected by your failure.
- **`McPlugin_Shutdown(void)`** — Deactivation → Shutdown. By the time this runs, **the host has
  already removed every menu item and shortcut you registered** — you don't need to (and can't)
  prevent that by skipping it here. This callback exists for *your own* resources the host has no
  way to know about (memory, handles, threads you started). If you hold none, you don't need to
  export this at all.

A plugin that only exports `McPlugin_GetInfo` (§1's minimal example) never advances past Loaded —
it's identified and logged, but there's nothing further for the host to do with it.

## 5. Capabilities

### Menu integration

```cpp
bool ok = host->AddMenuItem(L'F', L"My Command", L'M', myCommandId, kMcMenuPlacementAppend, 0);
```

`topMenuMnemonic` picks one of the six built-in top menus (File `F`, Selection `S`, Commands `C`,
Network `N`, View `V`, Settings `E`) — matched case-insensitively, the same way pressing
`Alt+<letter>` opens that menu for a user. An unrecognized mnemonic is a no-op (`false`, nothing
added) — you can only extend an existing top menu, not create a seventh one. `itemMnemonic` needs
to be unique only within that one menu, not across the whole app. `commandId` is entirely yours —
pick any `uint32_t` that's meaningful to you; the host hands it back unchanged to your own
`McPlugin_OnMenuAction` when the item is chosen.

```cpp
host->SetMenuItemEnabled(myCommandId, false);  // still there, just not selectable
host->RemoveMenuItem(myCommandId);             // gone entirely
```

Both are scoped to your own plugin automatically — you cannot address another plugin's menu items,
because you never learn their `pluginId` in the first place.

### Dialogs

```cpp
const wchar_t* lines[] = {L"Something happened."};
host->ShowMessage(L"My Plugin", lines, 1);

McDialogOption options[] = {{L'Y', L"es"}, {L'N', L"o"}};
int choice = host->ShowChoice(L"Proceed?", lines, 1, options, 2);  // -1 if the user pressed Esc

wchar_t buffer[256];
bool submitted = host->ShowTextPrompt(L"Enter a name", L"", buffer, 256);
```

These are the same modal dialogs MyCommander's own built-in features use — same visual
conventions, no separate styling to learn. There's no separate "warning dialog" vs. "error
dialog" function; pick your own title/wording, `ShowMessage`/`ShowChoice` don't distinguish.

### Logging

```cpp
host->Log(kMcLogInfo, L"Something happened.");
```

The host prefixes every line with your plugin's own `id` before writing it, so a log reader can
always tell which plugin produced which line. This only ever writes anything if the *user's own*
`Config::logLevel` setting is at or above the level you logged at — you can't force logging on for
a user who's opted out. Never log anything containing user-typed text, file content, or
credentials — the same discipline MyCommander's own code follows internally.

### Theme

```cpp
McThemeSnapshot theme;
host->GetTheme(&theme);
// theme.normalFg/normalBg, theme.dirFg/dirBg, ... — one (fg, bg) pair per UI element,
// each 0-15 for foreground (bit 3 is the "bright/intense" flag) and 0-7 for background,
// the standard 16-color console palette, not raw Win32 attribute bitmasks.
```

To be notified when the *user* changes their theme (rather than only reading it once), export:

```cpp
extern "C" __declspec(dllexport) void __stdcall McPlugin_OnThemeChanged(const McThemeSnapshot* theme);
```

No capability bit needed for this one — the host looks for it optionally on every plugin. It's
never called before your own `McPlugin_Init` returns, so read the *current* theme via `GetTheme`
during `Init` if you need it right away; `OnThemeChanged` only fires for changes after that point.

### Keyboard shortcuts

```cpp
McShortcutSpec spec{};
spec.virtualKeyCode = 'Y';
spec.ctrl = true; spec.shift = true; spec.alt = false;
spec.commandId = myCommandId;
bool ok = host->RegisterShortcut(&spec);  // false if already taken
```

`ok` is `false` if that exact key+modifier combination already belongs to a built-in MyCommander
shortcut or another already-loaded plugin — nothing is registered in that case. **Always check the
return value and handle `false` gracefully** (log it, skip that one feature, don't treat it as a
fatal `Init` failure) — see the Troubleshooting chapter for how to tell which of the two reasons
caused it. `commandId` works exactly like a menu item's: yours to choose, handed back unchanged to
your own `McPlugin_OnShortcut`. `host->UnregisterShortcut(commandId)` releases one shortcut early
if you ever want to; every shortcut you still hold is released automatically at `Shutdown`
regardless.

**Users can rebind your shortcut** (KEY-005). A line `plugin:<your-plugin-id>/<commandId> = key` in
the user's `keys.conf` makes the host register your command under that key instead of the one you
asked for (or under none, for `= none`), and a reload re-binds it live. Your code doesn't change:
`OnShortcut` still receives the same `commandId`, and `RegisterShortcut` returns `true` when the
user chose `none`. So **document your plugin id and command numbers** for users who want to rebind
them, and keep each command's `commandId` stable between versions.

### Viewers (API 1.10, `kMcPluginCapViewer`)

A viewer plugin shows a file **as text in the host's own viewer**: you produce lines, MyCommander
displays them, and scrolling, search (`/`, `n`), hex mode and resizing work without any code of
yours. Declare `kMcPluginCapViewer` (and API 1.10) and export three functions:

```cpp
// Cheap, name-only: may you view this file? kMcViewNo / kMcViewOffer / kMcViewDefault.
extern "C" __declspec(dllexport) McViewClaim __stdcall McPlugin_CanViewFile(const wchar_t* fileName);
// Your viewer's name in the Shift+F3 chooser (optional; your plugin name otherwise).
extern "C" __declspec(dllexport) const wchar_t* __stdcall McPlugin_GetViewerName(void);
// Render: call writer->AppendLine(writer->context, line) per line; optionally SetTitle.
extern "C" __declspec(dllexport) bool __stdcall McPlugin_RenderView(const wchar_t* filePath,
                                                                    const McViewWriter* writer);
```

- **`kMcViewOffer`** lists your viewer in **Shift+F3** for that file. **`kMcViewDefault`** also makes
  it what **F3** opens — unless the user's `file-actions.conf` names a view action for the file,
  which always wins. Prefer `kMcViewOffer` unless you are *the* viewer for a format; the user can
  make you a default themselves (File → Assign File to App, or `plugin:<your-id>` in
  `file-actions.conf`).
- **Return `false`** from `RenderView` when you can't show the file; the host tells the user and
  opens its built-in viewer instead. Lines are plain text (no colors); tabs are expanded by the
  viewer. The host caps the output (2,000,000 lines / 256 MB) and marks a truncated view, so
  stream big files instead of building everything in memory, and call `PollCancel()` between
  chunks so Esc stops a long render.
- **Inside panel content** (an archive or a server), `ShowFileViewer(path)` also goes through
  viewer plugins claiming `kMcViewDefault` — but never through external programs from
  `file-actions.conf`, because such a file is usually your own temporary copy, deleted right after.
- `Source/ExamplePlugin/ExamplePlugin.cpp` has a complete example: a hex-dump viewer offered for
  every file.

### Help content (F1 shortcut reference)

```cpp
static const wchar_t* kHelpMarkdown =
    L"# My Plugin\n"
    L"\n"
    L"A short description of what this plugin does.\n"
    L"\n"
    L"- `Ctrl+Shift+Y`: does the thing\n"
    L"- Also reachable from the File menu\n";

extern "C" __declspec(dllexport)
const wchar_t* __stdcall McPlugin_GetHelpMarkdown(void) {
    return kHelpMarkdown;
}
```

Set `kMcPluginCapHelp` in your `McPluginMetadata::capabilities` and export
`McPlugin_GetHelpMarkdown`; the host calls it fresh every time the user presses `F1`, and renders
the returned string as a new sub-heading (your own `name` from metadata) inside a "Plugins"
section appended after MyCommander's own built-in shortcut categories. The host also draws a
full-width `====` rule immediately above your own block — not something your Markdown produces,
just the host marking "a new plugin's help text starts here" so multiple plugins' content never
blurs together while scrolling; nothing for you to add or account for. The renderer is
**deliberately minimal** — not a general CommonMark implementation — and recognizes exactly:

- A line starting with one or more `#` followed by a space is a heading (level isn't tracked —
  `#` and `##` render identically).
- A line starting with `-` or `*` followed by a space is a bullet list item.
- Single backticks around inline code (`` `like this` ``) are stripped, not specially colored —
  they still read fine as plain text, but don't expect a different color for that span.
- Anything else is a plain paragraph line, including any Markdown syntax this list doesn't
  mention (tables, links, nested lists, fenced code blocks, emphasis) — it renders as-is, backticks
  included if you didn't use single ones, rather than being interpreted.

The returned pointer must stay valid until your own `McPlugin_Shutdown` returns — a `static const`
string literal (as above) is the simplest way to guarantee that, same convention as
`McPluginMetadata`'s own string fields.

## 6. Talking to the host

Beyond the capability-specific functions above, `McPluginHost` gives you:

```cpp
host->SimulateKey(virtualKeyCode, ctrl, shift, alt);
```

"Execute a supported application command" is, concretely, "press the key that already does it" —
the exact same synthetic-input mechanism MyCommander's own hint-bar clicks and menu items use
internally. Want your plugin's menu item to trigger a Copy? `SimulateKey(VK_F5, false, false,
false)` — no separate command-execution API to learn.

```cpp
wchar_t path[MAX_PATH];
host->GetActivePanelPath(path, MAX_PATH);

wchar_t* names[16];
wchar_t buffers[16][260];
for (int i = 0; i < 16; ++i) names[i] = buffers[i];
int total = host->GetSelectedNames(names, 16, 260);
// total may exceed 16 -- that's how many names *exist*, not how many were written;
// re-call with a bigger array if you need all of them.
```

`GetSelectedNames` follows the same convention every built-in file operation already does: the
current selection, or just the cursor entry if nothing is explicitly selected.

### Options dialog (multiple fields in one modal)

`ShowChoice`/`ShowTextPrompt` each handle exactly one concern. For a real options dialog —
several checkboxes, numbers, or choices at once — use `ShowForm`:

```cpp
const wchar_t* levels[] = {L"Store", L"Fast", L"Normal", L"Best"};
McFormField fields[2] = {};

fields[0].structSize = sizeof(McFormField);
fields[0].kind = kMcFormFieldChoice;
fields[0].label = L"Compression";
fields[0].choices = levels;
fields[0].choiceCount = 4;
fields[0].choiceIndex = 2;  // Normal

fields[1].structSize = sizeof(McFormField);
fields[1].kind = kMcFormFieldCheckbox;
fields[1].label = L"Include subdirectories";
fields[1].boolValue = true;

if (host->ShowForm(L"My options", fields, 2)) {
    // Enter: every field's value is updated in place, in the same array.
    int chosenLevel = fields[0].choiceIndex;
    bool recurse = fields[1].boolValue;
} else {
    // Esc: fields were left however the user had them edited -- don't apply them.
}
```

Five field kinds: `kMcFormFieldCheckbox` (`boolValue`), `kMcFormFieldNumber` (`numberValue`,
clamped to `[numberMin, numberMax]`, digit-only input — no negative numbers), `kMcFormFieldChoice`
(`choiceIndex` into `choices`, cycled with Left/Right), `kMcFormFieldText` (`textBuffer`, the
same truncate-safe in/out convention as `ShowTextPrompt`), and — **API 1.9** —
`kMcFormFieldPassword`: exactly a Text field, but every character is drawn as `*`, so a secret
never reaches the screen. Clear your buffer (`SecureZeroMemory`) once you've used the value, and
never log it. A plugin that uses it must declare API 1.9; an older host then refuses to load it
rather than drawing the password in the clear. `Up`/`Down` moves between fields;
`Enter` confirms the whole dialog, `Esc` cancels it — there's no per-field OK, only the dialog's
own.

### Addressing a specific panel

`GetActivePanelPath`/`GetSelectedNames` above only ever describe *the currently active* panel.
Once your own content occupies one panel (see "Panel content" below), you'll often need to reach
the *other*, real one instead — `GetActiveSide` plus the side-scoped siblings do that:

```cpp
McPanelSide active = host->GetActiveSide();
McPanelSide other = (active == kMcPanelLeft) ? kMcPanelRight : kMcPanelLeft;

wchar_t otherPath[1024];
host->GetPanelPath(other, otherPath, 1024);

wchar_t* names[16];
wchar_t buffers[16][260];
for (int i = 0; i < 16; ++i) names[i] = buffers[i];
int total = host->GetPanelSelectedNames(other, names, 16, 260);

// After writing real files into that panel's own directory yourself:
host->RefreshPanel(other);
```

`RefreshPanel` is what makes files you just wrote show up without the user manually leaving and
re-entering the directory — the host has no other way to know your plugin touched that panel's
own folder.

### Panel content

Declare `kMcPluginCapPanel` and export all three: `McPlugin_OnPanelDraw`, `McPlugin_OnPanelKey`,
`McPlugin_OnPanelClick`. Then, from anywhere you already have `host` (typically `Init`, or a menu
command/shortcut handler):

```cpp
bool ok = host->RequestPanelContent(kMcPanelLeft, L"My Plugin's View");
```

`ok` is `false` if the other side is already claimed by a different plugin, or if you're missing
the capability bit or one of the three exports above. Once it succeeds, that panel's header shows
your `title` instead of its real path, and its whole content area — every redraw, every keypress,
every click — is yours until you call `host->ReleasePanelContent(kMcPanelLeft)` (or your plugin
unloads, which releases it for you automatically, same as menu items and shortcuts):

```cpp
extern "C" __declspec(dllexport)
void __stdcall McPlugin_OnPanelDraw(McPanelSide side, uint16_t width, uint16_t height, bool active) {
    host->PanelDrawText(side, 0, 0, L"Hello from my plugin!", kMcPanelColorNormal);
    host->PanelDrawText(side, 0, 1, L"Press Q to do something.", kMcPanelColorHighlight);
}

extern "C" __declspec(dllexport)
void __stdcall McPlugin_OnPanelKey(McPanelSide side, uint16_t virtualKeyCode, uint32_t controlKeyState) {
    if (virtualKeyCode == 'Q') { /* ... */ }
}

extern "C" __declspec(dllexport)
void __stdcall McPlugin_OnPanelClick(McPanelSide side, int16_t relativeX, int16_t relativeY, bool doubleClick) {
    // relativeX/relativeY are relative to your own content area's top-left corner (0,0) --
    // the exact same coordinate space McPlugin_OnPanelDraw draws in.
}
```

`PanelDrawText` only works *during* your own `McPlugin_OnPanelDraw` call for that same `side` — it
silently does nothing if called at any other time, since there's no live drawing surface to target
outside that one call. Coordinates/text landing outside the content area are clipped, not an
error. `McPanelColor` is a small, named palette (`Normal`/`Highlight`/`Section`/`Warning`), not a
raw console attribute — the host maps each to whichever live theme attribute already serves that
role, so your content stays visually consistent through a theme change (§11) automatically.

**Every key except `Tab`, `F1`, and `F10` goes to your `OnPanelKey`** while your content is installed and
that panel is active — including the panel's own ordinary navigation keys (arrows, Enter,
Backspace, and so on), since your content has entirely replaced the file listing they'd otherwise
move around in. This means panel history (Alt+Left/Alt+Right) also stops reaching the panel while
your content is up — reasonable, since there'd be nothing visible for it to change anyway.

**Your content's own real path stays underneath, unchanged** — `RequestPanelContent` replaces only
what's drawn and how keys are handled, never the panel's own real filesystem path/history. This
matters for one case in particular: if the user presses the host's own **F5 Copy** while the *other*
panel is active and yours is the destination, the host's ordinary copy targets that real path
underneath your content — not whatever your content is actually showing — which looks like "copying
into it silently failed" if your content represents something other than that real directory (an
archive's own entries, for example). Export the optional `McPlugin_OnReceiveFiles(McPanelSide side,
McPanelSide sourceSide)` (API 1.8) to intercept this:

```cpp
extern "C" __declspec(dllexport)
bool __stdcall McPlugin_OnReceiveFiles(McPanelSide side, McPanelSide sourceSide) {
    // side: the panel your content occupies (F5's destination).
    // sourceSide: the other, real panel (F5's source) -- read its
    // selection the same way shown above (GetPanelSelectedNames/GetPanelPath).
    // Return true once you've handled it (even if there was nothing to do),
    // so the host skips its own real-filesystem copy for this keypress.
    // Return false (or don't export this at all) to leave the host's own
    // fallback copy unchanged.
}
```

See `Source/ZipPlugin/ZipPlugin.cpp`'s own `McPlugin_OnReceiveFiles` for a complete example (it
reuses the exact same "add a panel's selection into the archive" logic its own Shift+Insert
shortcut already uses). There is no equivalent interception point for F6 Move today — Move into
panel content is left to the host's own ordinary fallback behavior.

**`Ctrl+R` is this project's own conventional "Refresh" key** (IS-0012) — for a normal panel, the
host re-reads its directory from disk. It carries no special routing of its own: while your
content is installed and active, it reaches your `OnPanelKey` exactly like any other key (per the
"every key except Tab/F1/F10" rule above). If your content is backed by something that can go
stale — data read from a file, e.g. — consider handling `Ctrl+R` yourself to reload it, the same
way `Source/ZipPlugin/ZipPlugin.cpp`'s own archive browser re-reads its `.zip` from disk on this
key. Nothing requires you to; a plugin that doesn't handle it just does nothing on `Ctrl+R`, same
as any other key it doesn't recognize.

### Animated progress (API 1.6)

The host provides a shared busy dialog for synchronous plugin work. It animates every
250 ms independently of your operation, including during one long compression or disk call.
Call these functions on the UI thread, outside `OnPanelDraw`:

```cpp
if (host->BeginProgress(L"Deleting from archive", 0)) {
    host->UpdateProgress(0, 0, L"Preparing replacement archive...");
    for (uint64_t i = 0; i < count; ++i) {
        host->UpdateProgress(i, count, names[i]);
        // Process this item (the spinner continues while this call blocks).
    }
    host->UpdateProgress(count, count, L"Writing archive to disk...");
    host->EndProgress();
}
```

`total == 0` means unknown; otherwise the dialog shows completed/total items.
`BeginProgress` returns false if a plugin dialog is already open or cannot be started.
Use an RAII guard to call `EndProgress` on every exit, including failures. The host also
closes any forgotten dialog when the plugin callback returns. Updates/closing are scoped
to the owning plugin. Host choice/message/form dialogs and the file viewer temporarily
pause animation and restore progress afterward.
The main application can use `mc::AnimatedProgressDialog` from `Dialog.h`, whose scope
automatically closes the dialog; its background callback runs only on the UI thread.

### Cancelling a long operation (API 1.7)

This display by itself does not cancel work — call `host->PollCancel()` once per item in
your loop (the same shape as the host's own Copy/Move cancel polling) to find out whether
the user pressed Esc since your last check:

```cpp
for (uint64_t i = 0; i < count; ++i) {
    if (host->PollCancel()) break;  // stop promptly instead of finishing the whole batch
    host->UpdateProgress(i, count, names[i]);
    // Process this item.
}
```

`PollCancel` drains any input queued since the last call (applying a resize along the way)
and reports whether Esc was among it — it doesn't block waiting for a keypress. Check it
every item, not just when a conflict dialog happens to pop up, since Esc should interrupt
the operation at any point, not only while a nested dialog is already open.

### Inter-plugin services

API 1.5 also offers an optional `McPlugin_OnPanelMouse(side, x, y, buttons, eventFlags,
controlKeyState)` export. Coordinates match `OnPanelDraw`; the remaining fields use Win32
`MOUSE_EVENT_RECORD` values. It receives clicks, right-drag selection, wheel events, and
button releases in the content area. If absent, the host uses `OnPanelClick` for left clicks.
`kMcPanelColorSelected` colors marked entries independently of `kMcPanelColorHighlight`,
which marks the cursor. `host->ShowFileViewer(filePath)` opens the built-in text viewer and
returns when it closes; the plugin retains responsibility for deleting any temporary file.
F1 opens the application's help, including the plugin's exported help, on either panel.

A host-mediated way for two independently loaded plugins to find each other without linking
against each other's DLL:

```cpp
struct MyServiceV1 { int (__stdcall *DoSomething)(int arg); };
static MyServiceV1 kMyService = { &DoSomethingImpl };
bool published = host->PublishService(L"com.example.myservice", &kMyService, /*interfaceVersion=*/1);
```

```cpp
void* raw = host->QueryService(L"com.example.myservice", /*requestedVersion=*/1);
if (raw) {
    auto* svc = static_cast<MyServiceV1*>(raw);
    svc->DoSomething(42);
}
```

The host never looks inside the `void*` — agreeing on what it actually points to (a struct of
function pointers, as above, is the natural shape) is a contract between the two plugins involved,
not something MyCommander validates beyond "does this name exist, and is its published version
`>=` what was requested." `PublishService` fails (`false`) if another still-loaded plugin already
published that same name. **Always null-check a `QueryService` result, every time you call it** —
a service that answered non-null a moment ago can vanish if its owner unloads, and there's no
notification when that happens; nothing forces you to re-check, so make it a habit.

*`McPlugin_GetHelpMarkdown` (Phase 2) is documented in the "Help content" section above rather than
here, since it's a plugin export the host calls, not a `McPluginHost` field the host gives you.*

## 7. Error handling — what isolation actually means

Every call the host makes *into* your plugin's code (`GetInfo`, `Init`, `Shutdown`,
`OnMenuAction`, `OnShortcut`, `OnThemeChanged`) is wrapped in Windows structured exception
handling (SEH). If your code raises a structured exception during one of those calls — an access
violation, a divide-by-zero — the host catches it, logs it against your plugin's own `id`, and
treats that one call as having failed. Other plugins, and MyCommander itself, keep running.

**Read this as a safety net, not a substitute for correctness.** SEH catches *structured*
exceptions at the exact moment they're raised during a call the host made — it does not catch
silent heap corruption from an earlier bug that only crashes later, a stack overflow that's
already unwound past any recovery, or a crash on a thread your plugin spawned itself (the host
only wraps its own direct calls into you, not anything you do concurrently). True fault isolation
for arbitrary third-party code would mean running every plugin in its own separate process — a
fundamentally different architecture from "load a DLL into this process," and not what MC Plugin
DLLs are. Write your plugin the way you'd write any code sharing a process with something you
don't want to bring down: validate the `host` pointer and its fields before use, avoid unbounded
recursion or allocation inside a callback the host is waiting on, and prefer returning an error
(`false` from `Init`, a logged warning) over letting something fail in a way that could corrupt
shared state.

## 8. Worked example

`Source/ExamplePlugin/ExamplePlugin.cpp` is a complete, minimal plugin — read it alongside this
guide, chapter by chapter:

- `McPlugin_GetInfo` → §3 (Metadata reference)
- `McPlugin_Init` → §4 (Lifecycle) for the shape, §5 (Menu integration, Keyboard shortcuts) for
  what it actually registers
- `McPlugin_Shutdown` → §4, and note how little it does — the host already cleaned up the menu
  item and shortcut by the time it runs
- `McPlugin_OnMenuAction`/`McPlugin_OnShortcut` → §5, both funneling into the same `SayHello()`
  since they're the same logical command reached two different ways
- `SayHello()`'s use of `host->ShowMessage`/`host->Log` → §5 (Dialogs, Logging)
- `McPlugin_GetHelpMarkdown` → §5 (Help content) — press `F1` after loading it and look for the
  "Plugins" section, "Example Plugin" sub-heading, near the bottom of the scrollable list

Build it with its own `ExamplePlugin.vcxproj` (a standalone project, not part of
`MyCommander.sln`) — its `PostBuildEvent` copies `ExamplePlugin.dll` straight into
`Build\x64\$(Configuration)\Plugins\` at the repo root, the exact folder `PluginManager::
DiscoverAndLoad` scans next to whichever `MyCommander.exe` that same `$(Configuration)` build
produced, so a Debug build of the plugin becomes visible to a Debug run of the app (and Release
to Release) with no manual copy step. Writing your own plugin from scratch, not starting from
this example project? Copy the same `PostBuildEvent` pattern into your own `.vcxproj`, or just
copy your built DLL into that `Plugins` folder by hand.

For a second, substantially larger worked example — real file I/O, a vendored third-party
library, and the options-dialog/side-scoped-panel functions from this chapter — see
`Source/ZipPlugin/ZipPlugin.cpp` (the "Pack file"/"Unpack Package" ZIP plugin, `AIPrompt/features/
FEA-0007-plan.md`). It's a genuinely useful plugin, not a toy, built exactly the way a real
third-party plugin author would: its own standalone `.vcxproj`, its own vendored dependency
(`third_party/miniz/`), reading `McFormField`s back after `ShowForm`, `GetActiveSide`/
`GetPanelPath`/`RefreshPanel` to default its output next to the other panel and make the result
appear there immediately, and (its "Unpack Package" command) a worked example of validating
untrusted input before writing anything to disk — rejecting an archive entry name that tries to
escape the destination directory.

Two more bundled plugins show panel content on a remote server: `Source/FtpPlugin/` (EXT-005) and
`Source/SftpPlugin/` (EXT-004, vendoring libssh2). They share one remote-panel implementation,
`Source/PluginCommon/` (`RemotePanel`, a `remote::Client` interface each protocol implements, a
connections store and an in-memory `FakeRemoteClient` for tests). Each plugin compiles those files into
its own DLL. That's ordinary source sharing between two plugins built from this repository, **not part of
the SDK**: plugins still talk to each other only through the host (inter-plugin services above), and
nothing in `PluginCommon` crosses a DLL boundary.

## 9. Troubleshooting

| Symptom | Likely cause | What the host logs |
|---|---|---|
| Plugin doesn't appear at all | No `McPlugin_GetInfo` export, or it's not `extern "C"`/undecorated | "has no McPlugin_GetInfo export -- not an MC Plugin" |
| Plugin doesn't appear at all | `magic` isn't `MC_PLUGIN_MAGIC` | "magic mismatch -- not an MC Plugin" |
| Plugin loads but never activates | API version incompatible (see §2) | "API version X.Y incompatible with host A.B" |
| Plugin loads but never activates | Listed in the user's `Config::disabledPluginIds` | "disabled via Config::disabledPluginIds" |
| Plugin loads but never activates | `McPlugin_Init` returned `false`, or crashed | "McPlugin_Init returned false" / "crashed calling McPlugin_Init" |
| Menu item never appears | `topMenuMnemonic` doesn't match any built-in top menu | (silently skipped — check your mnemonic against File/Selection/Commands/Network/View/Settings) |
| Menu item appears but does nothing | Declared `kMcPluginCapMenu` without exporting `McPlugin_OnMenuAction` | "declared kMcPluginCapMenu but has no McPlugin_OnMenuAction export" |
| Shortcut silently doesn't fire | `RegisterShortcut` returned `false` (a conflict) and your code didn't check it | (nothing — check your own return value; the host doesn't log a rejected registration attempt, since it's not an error on the host's part) |

Every log line above is written wherever the user's own `Config::logLevel` points
(`%APPDATA%\MyCommander\mycommander.log`) — ask the user testing your plugin to set `logLevel =
debug` temporarily if you need to see all of it.
