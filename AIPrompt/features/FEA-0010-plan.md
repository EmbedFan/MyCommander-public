# Feature Plan — FEA-0010 (Network Info Plugin)

Source: `AIPrompt/additional_feature_requests.md`:

> **(FEA-0010) Network Info Plugin** — Create a Plugin, that shows the network area:
> - available shares, machines
> - If ready, available Wifi networks, if this machine is connected to it shall be emitted.
> - this is shown as a panel on left or right panel, the not active panel shall be used.
> - before hide the not used panel, save its directory and selected item
> - The network panel can be closed by pressing ESC
> - After closing network panel the file panel shall be restored using saved configuration

Delivered as an **MC Plugin** (`Source/NetInfoPlugin/`), like the ZIP, FTP and SFTP plugins: own `.vcxproj`, outside
`MyCommander.sln`, built by `build.cmd`, deployed to `Plugins\`, bundled by the installer. New requirement namespace
**`NET-`** (`NET-001`…`NET-010`, a new §21.3 of the spec).

## What exists today (checked)

| Need | Existing mechanism | Note |
|---|---|---|
| Show custom content in a panel | `RequestPanelContent` / `OnPanelDraw` / `OnPanelKey` / `OnPanelClick`, `PanelDrawText`, `ReleasePanelContent` (PLUG-012) | The ZIP/FTP plugins used it before EXT-002 |
| Which panel is active | `GetActiveSide()`; the other side is `1 - active` | |
| Keep the hidden panel's folder and cursor | **Already guaranteed by the host.** `Panel::SetContent` leaves the panel's real path, history, entries, selection and cursor untouched underneath (`Panel.h`, FEA-0002 §8). The folder watch is closed while content is installed. | This satisfies "save its directory and selected item" and "restore" without copying anything. The plugin still records the folder and cursor name for the log and for a safety check (F5 below). |
| Menu item, shortcut, help | `AddMenuItem` on the Network menu, `RegisterShortcut`, `McPlugin_GetHelpMarkdown` | |
| Messages, progress | `ShowMessage`, `ShowChoice`, `ShowForm`, `Log` | |
| Re-list a real panel | `RefreshPanel(side)` | |
| **Open a folder in a panel** | **Missing.** There is no host call to make a panel go to a path. | Needed for "Enter on a share opens it" (D6). |

Nothing in the code today enumerates machines or Wi-Fi: `NetDrives.cpp` maps drives (`WNet*`), `NetShares.cpp` lists
*this* computer's shares, nothing else touches the network.

## Design

### D1 — Plugin shape

| Item | Value |
|---|---|
| Project | `Source/NetInfoPlugin/` (`NetInfoPlugin.vcxproj`, `/utf-8`, C++20, v143, `PostBuildEvent` deploys to `Build\x64\<Config>\Plugins\`) |
| Files | `NetInfoPlugin.cpp` (exports, menu, panel UI), `NetInfoModel.{h,cpp}` (pure: items, sections, sorting, row text, key handling), `NetInfoSources.{h,cpp}` (the Windows calls behind an interface), `FakeNetInfoSources.h` (test double) |
| Capabilities | `kMcPluginCapMenu \| kMcPluginCapShortcuts \| kMcPluginCapHelp \| kMcPluginCapPanel` |
| Id / name | `netinfo` / "Network Info" |
| Libraries | `mpr.lib` (WNet), `netapi32.lib`, `wlanapi.lib`, `iphlpapi.lib`, `ws2_32.lib`; all in the Windows SDK — **no vendored code** |
| Tests | Two layers: `NetInfoModelTests.cpp` (pure, with the fake sources) and a standalone harness like `FtpPanelTests` that includes `NetInfoPlugin.cpp` with a fake `McPluginHost`; the real sources get opt-in smoke tests (see Tests) |

### D2 — What the panel shows

One scrollable list in three sections, headings drawn in `kMcPanelColorSection`:

1. **This computer** — its name; its own shares (`NetShareEnum`, administrative shares dimmed or hidden — see open
   question 2); mapped network drives (`WNetEnumResource`, the same data as NAV-014).
2. **Wi-Fi** — available networks: name (SSID), signal bar (`▂▄▆█` or ASCII `[###.]` under ACC-006), security
   (`Open` / `WPA2` / `WPA3`...) and the channel/band if known. The network **this computer is connected to is
   emitted** (highlighted, `kMcPanelColorHighlight`, prefixed `*` so it's identifiable without colour — ACC-001 style,
   and "connected" in the status text). "If ready" = the section shows only when a WLAN interface exists and its
   service runs; otherwise one line: `No Wi-Fi adapter` / `WLAN service not running` / `Wi-Fi is turned off`.
3. **Network** — the machines found: name, and the address (IPv4/IPv6 as known). Enter on a machine expands it in
   place: its shares (`NetShareEnum` level 1 on `\\machine`), each with type (Disk / Printer / IPC / Device) and remark.

Header line (the panel title the host draws): `Network info`; a status row at the bottom of the content area shows what is
happening (`Scanning... 12 machines`, `Last updated 14:02:31`, an access-denied note for a machine).

### D3 — Where the data comes from, and honest limits

This is the part with real uncertainty; each source is isolated behind `INetInfoSource` so a failing one only empties
its own section.

- **Shares of this computer:** `NetShareEnum` (works unelevated, verified for IS-0022).
- **Machines:** there is no single reliable Windows API.
  - **Source A, SMB/NetBIOS browse:** `WNetOpenEnum(RESOURCE_GLOBALNET)` and `NetServerEnum`. Works on LANs where the
    Computer Browser/NetBIOS still runs (many home routers, older NAS), but is **empty on a default Windows 10/11
    machine**, since Microsoft removed SMB1 browsing.
  - **Source B, the neighbour table:** `GetIpNetTable2` (hosts this computer talked to recently), then a reverse
    lookup for the name (`getnameinfo`, then a NetBIOS node-status query to UDP 137 as fallback), with a short
    timeout and an Esc/close-panel cancel. Reliable for "machines I can see", cheap.
  - **Source C, WS-Discovery/SSDP** (what Explorer's Network folder uses): possible with a small UDP multicast probe
    (WS-Discovery `Probe` on 239.255.255.250:3702), parsed only for device name and address. It gives Windows
    machines and many devices. Planned as the **second step** (Phase 2): the plugin works without it.
  - Results are merged by address/name and marked with the source that found them.
  - **Limit stated in the user guide:** it shows machines that answer; a switched-off or firewalled machine is absent.
- **Shares of a remote machine:** `NetShareEnum(level 1, "\\\\machine")`. It needs the current user to be allowed on
  that machine; otherwise `Access denied` is shown on that row and the user may press `C` to retry with credentials
  via `WNetAddConnection2` (a password prompt masked with the existing `kMcFormFieldPassword`; the password is used
  for this call only, never stored, logged or put in history — same rule as FTP/SFTP). Without credentials entry (phase 1) the row only
  reports the error.
- **Wi-Fi:** the WLAN API: `WlanOpenHandle`, `WlanEnumInterfaces`, `WlanGetAvailableNetworkList` (+ `WlanScan` to ask for
  a fresh scan, results arrive via the notification callback within a few seconds) and `WlanQueryInterface` with
  `wlan_intf_opcode_current_connection` for the connected network. **Read-only: the plugin never connects,
  disconnects, or reads saved keys/profiles' passwords.**
  - **Known risk, Windows 11 24H2 (this machine is build 26200):** Windows hides Wi-Fi names/lists from desktop apps
    unless **Location** access is turned on for apps (`WlanScan` returns `ERROR_ACCESS_DENIED`, or the list is empty
    or unnamed). The plugin detects an empty list with a running adapter and shows `Windows hides Wi-Fi names until Location
    access for desktop apps is on (Settings > Privacy & security > Location)`. That is the honest limit; it can't be
    worked around legitimately.

### D4 — Threading and refresh

The host calls `OnPanelDraw` on every loop pass (~16 ms) from the main thread, and plugin host callbacks are
main-thread only (`PanelDrawText`, `RefreshPanel` ...). So:

- A **worker thread** per scan (one at a time) fills a model snapshot under a mutex; `OnPanelDraw` only reads the
  last snapshot. No host call is ever made from the worker.
- Scans: on open (Wi-Fi and machines in parallel, each source with its own timeout: 5 s Wi-Fi, 8 s machines), `F5`/`Ctrl+R`
  to rescan, and an optional timer rescan of Wi-Fi only (every 30 s while the panel is open, since signals move; machine
  discovery is manual). Never while closed.
- Closing the panel (or `Shutdown`) cancels the scan and **joins** the thread with a bounded wait so no thread can touch the
  unloaded DLL; a thread that doesn't stop in 2 s is detached from all state (it only owns its own copy) and the DLL
  is pinned (`GetModuleHandleEx` pin) so it can never run unloaded code. This is the dangerous part of any threaded
  plugin, so it gets its own test (cancel while a fake source is blocked).
- Every Win32 call is wrapped so a failure becomes a section message, never a crash (the host also SEH-wraps plugin calls).

### D5 — Opening, the non-active panel, Esc and restore

- **Open:** Network menu > `Network info`, and the shortcut `Alt+F11` (check against `BuiltinShortcutInventory`; to be verified
  free). If the plugin already owns a panel, the command closes it (a toggle).
- **Which panel:** the **non-active** one (`1 - GetActiveSide()`), as asked. The active panel stays active: the user keeps
  working where they were, and the network panel sits beside it. To use the network panel the user presses `Tab` like for
  any panel.
  - If the non-active panel is in provider mode (an archive or server), the plugin asks to use the active one instead or
    cancels; it never forces a provider closed (`IsPanelInProvider`).
  - If the non-active panel already shows plugin content (a tree), it is replaced only if the host allows it (`RequestPanelContent`
    fails when another plugin owns it): the message says so.
- **Saving the hidden panel's state:** the host keeps the folder and cursor in the panel itself (see "What exists"); the plugin records
  `GetPanelPath(side)` and the cursor name at open, and on close checks that the panel still reports the same
  folder. It calls `RefreshPanel(side)` after releasing so the listing reflects any change made while the panel was
  hidden, and the host restores the cursor by name. If the folder vanished meanwhile, the host's existing behaviour applies (`..` /
  nearest existing parent, LIST-006 banner).
- **Esc closes:** when the network panel is active, `Esc` releases it (`ReleasePanelContent`) and the file panel reappears
  exactly as before. A second route when the network panel is not active: the same menu command / shortcut closes it.
  (See open question 1.)
- Panel resize/terminal-too-small: handled by the host; the panel redraws at the new size.
- Auto-close: if the plugin is disabled/unloaded the host releases the content automatically (host-owns-cleanup).

### D6 — Keys and actions in the panel

| Key | Action |
|---|---|
| `Up` `Down` `PgUp` `PgDn` `Home` `End` | Move (headings are skipped) |
| `Enter` / `Right` | On a machine: expand/collapse its shares. On a share (local or remote): open it in the **other** (file) panel as `\\machine\share`. On a Wi-Fi network: show its details (SSID, security, signal, channel, BSSID count, connected or not), read-only |
| `Left` | Collapse the machine / jump to its row |
| `F5` or `Ctrl+R` | Rescan |
| `Esc` | Close the panel |
| `C` | (Phase 2) Retry a denied machine with credentials |
| `F3`/`Space` | Details box for the cursor row (all known fields) |
| mouse | Single click moves the cursor, double click = Enter (the keyboard path always exists, KEY-001) |

**Opening a share in the other panel needs a new host call.** The SDK has no way to navigate a panel. Plan: API **1.13**,
`bool NavigatePanel(McPanelSide side, const wchar_t* path)` — a thin wrapper over `Panel::NavigateTo`, refused (false) when the
panel shows content or a provider; backward compatible (appended field, the `structSize` check as for the other additions).
Which panel is "the other" is the one the plugin did **not** take. If NavigatePanel is missing (older host) the share
path is copied to the log/status line instead.

### D7 — Menu, config, installer

- Menu: `Network > Network info` added by the plugin (`AddMenuItem`), mnemonic checked against the existing Network menu
  (`N` Network Connection, `F`, `W` ...).
- Plugin settings: none in `config.ini` for now; the plugin reads a few optional values from its own
  `%APPDATA%\MyCommander\netinfo.conf` only if the user creates it (`wifiRefreshSeconds = 30`, `machineScanSeconds = 8`,
  `showAdminShares = false`), so no `Config.h` change.
- Installer: bundle `NetInfoPlugin.dll` like the other plugins (INST-007), `build.cmd` builds it, and the setup payload adds it.
- Log (ERR-004): scan start/end, counts, errors — **never** SSIDs' keys, credentials or addresses beyond the count (ERR-005: logs
  carry paths/errors only; machine and Wi-Fi *names* are not logged either).

### D8 — Privacy and safety

- Read-only everywhere; no network change, no connect, no key read.
- No telemetry; scans only while the panel is open; the neighbour-table lookups and discovery probes stay on the local link.
- A plugin crash is isolated by the host's SEH wrapper; the worker thread is bounded (D4).

## Requirements (new spec §21.3, all P3, IN-MVP; rows added when implemented, Not Started now)

| ID | Requirement |
|---|---|
| NET-001 | A Network Info plugin shall show the network area in a panel: this computer's shares and mapped drives, Wi-Fi networks and the machines found. |
| NET-002 | The panel shall use the panel that is not active and leave the active one untouched. |
| NET-003 | The hidden file panel's folder and selected item shall be preserved while the network panel is shown and restored when it closes. |
| NET-004 | `Esc` in the network panel shall close it and bring the file panel back; the menu command / shortcut shall toggle it. |
| NET-005 | The Wi-Fi network this computer is connected to shall be emitted (highlighted and marked with a text marker). |
| NET-006 | Wi-Fi shall be shown only when a WLAN adapter is ready; otherwise the reason shall be stated. Windows' location-access limit shall be reported, not hidden. |
| NET-007 | A machine shall expand to its shares (type and remark); an access error shall be shown on that machine only. |
| NET-008 | Enter on a share shall open it in the other panel (needs host API 1.13 `NavigatePanel`). |
| NET-009 | Scanning shall not block the UI, shall be cancellable by closing the panel, and shall never run while the panel is closed. |
| NET-010 | The plugin shall be read-only: no connect/disconnect, no key or password read, no credential stored or logged. |

## Files touched

| File | Change |
|---|---|
| `Source/NetInfoPlugin/*` (new project, `.rc` with version, `Version.h`) | The plugin |
| `Source/MyCommanderPluginSdk/McPluginHost.h`, `McPluginAbi.h` (minor 12 to 13), `DEVELOPER_GUIDE.md` | `NavigatePanel` |
| `Source/MyCommander/PluginManager.{h,cpp}` | `HostNavigatePanel` + thunk, `structSize` |
| `Source/MyCommanderTests/PluginManager*Tests.cpp` | `NavigatePanel` host tests |
| `build.cmd`, `Tools/*`, `Source/MyCommanderSetup/*` (payload list, `.rc`) | Build and bundle the new DLL (INST-007 pattern) |
| `AIPrompt/my-commander.md` | New §21.3 (`NET-001`…`010`), history note |
| `README.md`, `USERGUIDE.md`, `CLAUDE.md` (source layout), `AIPrompt/ManualTests.md`, changelog, backlog (none) | Docs |
| `Source/MyCommander/Version.h` | Minor bump (new plugin and API) |

## Phasing

- **Phase 1 (this plan's core):** project and model; this computer's shares and mapped drives; Wi-Fi list with the connected
  network emitted; machines from the neighbour table (Source B) and `NetServerEnum` (Source A); expand to shares; open a
  share in the other panel (`NavigatePanel`); Esc/toggle; threading; docs.
- **Phase 2 (afterwards, separately agreed):** WS-Discovery machines, credentials retry (`C`), Wi-Fi details box, optional `netinfo.conf`.

## Tests

- **`NetInfoModelTests.cpp`** (pure, in `MyCommanderTests` or the plugin's own harness): section building from a fake source;
  sorting (connected network first, then signal strength; machines by name); the signal bar and security text;
  the cursor skipping headings; expand/collapse; row text clipped by `StringDisplayWidth` with a wide-character SSID;
  the "no adapter / service stopped / location blocked" messages; admin shares hidden/dimmed.
- **Harness with a fake `McPluginHost`** (like `FtpPanelTests`): opening takes `1 - active`, never the active panel; a panel in
  provider mode is refused; `Esc` releases; toggle by menu; `RefreshPanel` is called after the release; scan results appear
  in the drawn text (captured `PanelDrawText` calls); a **blocked source** plus closing the panel returns within the bound
  (the thread test); no `PanelDrawText` is ever called from the worker thread.
- **Host:** `NavigatePanel` refuses a content/provider panel and navigates a normal one (`PluginManager` tests).
- **Opt-in smoke tests against the real machine** (`MC_NETINFO_TEST=1`): local shares non-empty on this machine, Wi-Fi call returns
  a defined status (never crashes), neighbour table call works. Read-only, makes no change.
- Full existing suite must stay green; the plugin DLL builds in `build.cmd`.

## Manual tests (new MT-069)

1. With the left panel active, open Network info: it appears in the **right** panel; the left stays active and unchanged.
2. Put the right panel on a deep folder with the cursor on a file, then open Network info and close it with Esc: the right panel shows the same folder and the same item selected.
3. Wi-Fi: the connected network is highlighted with `*`; switch Wi-Fi off in Windows and rescan: the reason is shown. Turn Location off/on for desktop apps and compare.
4. Expand a machine of your LAN: its shares appear; one you can't access shows `Access denied` only on that row.
5. Enter on a share: it opens in the other panel.
6. Close the panel while a scan runs (unplug the cable first for a slow case): the panel closes at once and the app does not hang or crash.
7. The panel works at 60x15 and when the terminal is resized while it is open.

## Open questions

1. **Esc needs focus.** The network panel is the non-active one, so `Esc` closes it only after you `Tab` into it (or use the menu/shortcut toggle). Alternatives: (a) as planned; (b) the panel also becomes active when opened (then Esc works at once, but the "keep working in the other panel" idea is lost); (c) Esc closes it even when it is not the active panel, if the active panel has nothing to cancel (but an unexpected Esc in a file panel closing a hidden thing is surprising). **Recommended: (a) with the toggle shortcut `Alt+F11`.**
2. **Admin shares (`C$`, `ADMIN$`, `IPC$`) of this computer:** hide them by default (recommended) or show them dimmed?
3. **Machine discovery scope:** Phase 1 uses the neighbour table plus `NetServerEnum`; WS-Discovery is Phase 2. OK, or do you want WS-Discovery in the first delivery (it finds the most machines on a modern home network but is more work)?
4. **Credentials for a denied machine:** Phase 2 (a masked prompt, used once, never stored), or not at all?
5. **New SDK call `NavigatePanel` (API 1.13):** OK to add it? Without it, Enter on a share can't open it in the other panel.
6. **Wi-Fi on Windows 11 24H2:** the location-access limit may make the list empty on your machine until you enable Location for desktop apps. Is a clear message (no workaround) acceptable?
7. **Version:** a minor bump (2.14.0) for the new plugin and the API addition?

## Status

Planned 2026-10-16. **Phase 1 implemented 2026-10-16** (spec 1.52.0, host 2.14.0, plugin 0.1.0) with the decisions of the same day: Esc needs focus (toggle on the menu and `Alt+F11`), administrative shares hidden, WS-Discovery left for Phase 2, no credentials retry at all, `NavigatePanel` added (API 1.13), the Wi-Fi Location message accepted, minor version bump.

Differences from the plan:

- **Closing does not wait up to 2 s for workers.** A worker only touches its own private state and the DLL is pinned, so closing waits at most 50 ms; the scanner is then dropped. Closing while a slow Windows call is pending returns at once (tested).
- **The controller sits behind `IPanelHost`** (`NetInfoController.{h,cpp}`), so the open/close/restore rules, Enter on a share and the rescan timer are unit-tested in `MyCommanderTests` with a fake host rather than in a separate harness. `NetInfoPlugin.cpp` is only the adapter.
- **The host refuses to replace a view it installed itself** (`RequestPanelContent` fails while the directory tree is on that panel).
- **No `netinfo.conf` yet** (it was Phase 2): the 30-second Wi-Fi refresh and the hidden administrative shares are constants.
- **The NetBIOS node-status fallback for naming machines** was left out: a machine with no reverse-DNS name is shown by its IPv4 address (an unnamed IPv6 neighbour is not listed).
- Tests: `NetInfoModelTests` (29 cases), `NetInfoControllerTests` (19, including the scanner) and one `PluginManagerTests` case; full suite 1,097 cases, 18,438 checks, 0 failed (Debug and Release); an opt-in smoke test (`MC_NETINFO_TEST=1`) reads the real machine.

**Not verified here:** how it looks in a real console, the real Wi-Fi and machine lists on your network, and the real Location-access behaviour on Windows 11 24H2 (manual test MT-069).

**Shortcut changed (2026-10-16):** `Ctrl+Shift+N` is Windows Terminal's "new window", which the terminal keeps for itself, so the application never receives it. The toggle is now `Alt+F11` (free in the key table, not bound by Windows Terminal).
