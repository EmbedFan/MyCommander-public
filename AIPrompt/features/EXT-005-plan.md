# Implementation Plan — EXT-005 (FTP as a remote file-system provider, delivered as an MC Plugin)

Source: `AIPrompt/my-commander.md` §21:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| EXT-005 | FTP support may be implemented as a remote file-system provider. | P3 | LATER | Not Started |

**User decision (2026-10-08):** EXT-005 is delivered as a **standalone MC Plugin** (`Source/FtpPlugin/`). It
follows the ZIP plugin's model (FEA-0007): built on FEA-0002's plugin architecture, outside `MyCommander.sln`,
bundled by the installer (INST-007). The detailed requirements are the new **§21.1 FTP Plugin Requirements
(`FTP-001`…`FTP-012`)** in the spec, added with this plan.

## Implementation check (2026-10-08)

Nothing speaks FTP today. Network → "FTP connection" (`F`) and "New FTP connection" (`W`) are MENU-003 stubs
(`MenuBar.h`). Everything else the plugin needs already exists, and the ZIP plugin exercises all of it in
production:

| Need | Existing mechanism | Where it's proven |
|---|---|---|
| Menu items replacing the stubs | `AddMenuItem` on the Network menu (`N`) | ZIP's Pack/Unpack replaced File-menu stubs |
| Show the server in a panel, navigate, select | `RequestPanelContent` / `OnPanelDraw` / `OnPanelKey` / `OnPanelClick` / `OnPanelMouse`, `PanelDrawText`, `ReleasePanelContent` (PLUG-012) | ZIP-005/006 archive browsing |
| F5 from the other panel **into** the server | `McPlugin_OnReceiveFiles` (API 1.8) | ZIP "add to archive" |
| F5 **from** the server to the other panel | `GetPanelPath(otherSide)` + `RefreshPanel` | ZIP extract |
| Connection form, conflict prompts, messages | `ShowForm` (text/number/checkbox/choice), `ShowChoice`, `ShowMessage`, `ShowTextPrompt` | ZIP pack dialog, IS-0010 overwrite-all |
| Progress with Esc | `BeginProgress` / `UpdateProgress` / `EndProgress` / `PollCancel` | ZIP pack/extract |
| F3 view | Download to `%TEMP%`, then `ShowFileViewer` | ZIP F3 |
| F1 help, log, theme | `McPlugin_GetHelpMarkdown`, `Log` (ERR-004), `GetTheme` | ZIP |
| User-rebindable keys | KEY-005: the panel receives the user's keys translated to the defaults; no plugin work | 1.22.0 |

**One gap:** there is **no masked (password) text field** in the SDK. Typing an FTP password into a plain
`kMcFormFieldText` would show it on screen. A small, backward-compatible SDK addition is planned (D6).

## Design

### D1 — Plugin shape

| Item | Value |
|---|---|
| Project | `Source/FtpPlugin/FtpPlugin.vcxproj` (standalone, like `Source/ZipPlugin/`; post-build deploys `FtpPlugin.dll` to `Build\x64\<Config>\Plugins\`) |
| Id / name | `com.mycommander.ftpplugin` / "FTP" |
| Capabilities | `kMcPluginCapMenu \| kMcPluginCapPanel \| kMcPluginCapHelp` |
| Version | Own `Version.h` (`MCPLUGIN_VERSION_*`, IS-0011 pattern), starting at 0.1.0 |
| Requires | Plugin API **1.9** (D6) |
| Files | `FtpPlugin.cpp` (exports, UI, panel), `FtpClient.{h,cpp}` (protocol client), `FtpParse.{h,cpp}` (pure parsers), `FtpConnections.{h,cpp}` (saved connections + Credential Manager), `FtpPluginTests.cpp` (standalone harness) |
| Third-party code | **None.** Winsock2 (`ws2_32.lib`) for the protocol; `advapi32` `CredReadW`/`CredWriteW` for saved passwords (D5) |

### D2 — The FTP client: own minimal client on Winsock (DEC-012)

**Recommended over WinINet**, for three reasons:

1. **UTF-8 file names.** WinINet's `Ftp*W` functions convert names through the ANSI code page. Accented and CJK
   names from a UTF-8 server come out wrong, and the UTF-8 code-page option is limited. Our own client sends
   `OPTS UTF8 ON` when the server lists `UTF8` in `FEAT`, and decodes names as UTF-8, falling back to the ANSI code
   page.
2. **Cancellation and timeouts.** A blocking WinINet call can't be interrupted by Esc. Our own sockets use
   `select()` with a short tick, so `PollCancel` is honored mid-transfer and mid-connect, and every wait has a
   timeout (default 20 s, configurable per connection).
3. **Future FTPS.** Explicit TLS (`AUTH TLS`) can later wrap the same sockets with Schannel, which WinINet's FTP
   API can't do. That is FTP-011, LATER.

**Scope of the protocol:**

- **Session:** `USER`/`PASS`, `FEAT`, `OPTS UTF8 ON`, `SYST`, `PWD`, `CWD`, `CDUP`, `TYPE I`, `QUIT`.
- **Data connections:** `EPSV`, falling back to `PASV` (passive is the default). Active mode (`PORT`/`EPRT`) is an
  option for servers that need it.
- **Listing:** `MLSD` when `FEAT` advertises it (exact sizes, UTC times, types). Otherwise `LIST`, parsed for the
  Unix `ls -l` and Windows/IIS `MM-DD-YY  hh:mmAM  <DIR>` formats.
- **Transfers and changes:** `RETR`, `STOR`, `DELE`, `MKD`, `RMD`, `RNFR`/`RNTO`, `SIZE`, `MDTM`.
- **Replies:** single- and multi-line replies (`123-…` up to `123 …`). `421` and connection drops are reported as
  "connection lost" (FTP-009).

**The pure parsers** (`FtpParse.{h,cpp}`, no sockets, unit-tested) cover:

- reply framing;
- `227` PASV and `229` EPSV address extraction;
- `MLSD` facts;
- Unix/Windows `LIST` lines, including symlinks `name -> target`, names with spaces, and year-or-time dates;
- the `FEAT` list;
- `MDTM` timestamps;
- path joining on the server's `/` paths.

### D3 — User interface

**Network → "FTP connection"** (`F`, replacing the stub) opens the **connection manager**. It is a
`ShowChoice`-style list of saved connections, with:

- `Enter` to connect;
- **New**, **Edit** and **Delete** (confirmed);
- **Quick connect**.

**Network → "New FTP connection"** (`W`, replacing the stub) opens the connection form directly:

| Field | Kind | Default |
|---|---|---|
| Name | Text | the host |
| Host | Text | — (also accepts `ftp://user@host:port/path`, split into the fields) |
| Port | Number 1–65535 | 21 |
| User | Text | `anonymous` |
| Password | **Password** (D6) | empty (for anonymous: an e-mail-style placeholder is sent) |
| Initial folder | Text | `/` (server default) |
| Passive mode | Checkbox | on |
| Save this connection | Checkbox | on |
| Save the password (Windows Credential Manager) | Checkbox | **off** |

After connecting, **the active panel shows the server** as plugin content:

- **The header** shows `ftp://user@host/path`. A plain-ASCII `*` marks an operation in progress.
- **Rows:** `..`, then folders first, with name / size / date / Unix permissions, or attributes when the server
  gives them. Selected rows are yellow and the cursor row is blue, the same as the ZIP panel.
- **Keys** use the same commander conventions as the ZIP panel (KEY-002):

| Key | Action |
|---|---|
| `Enter` | Enter a folder; on a file, view it (F3) |
| `Backspace` / `..` | Parent folder. At the server root: "Disconnect from host?" (FTP-002, like ZIP-007) |
| `Insert`, Space, `Num+`/`Num-`/`Num*`, `Shift+↑/↓`, right-click/drag | Selection (the same set as ZIP-006) |
| `F3` | View: download the file to `%TEMP%\mcftp-*`, show it, then delete the temporary copy |
| `F5` | Download the selection (or the cursor entry) to the other panel's folder, recursively for folders |
| `F6` | Download, then delete on the server only the items that downloaded successfully (asks first) |
| `F7` | Create a folder on the server |
| `F8` / `Delete` | Delete on the server, recursively for folders. **Always confirmed** (CLAUDE.md destructive-op rule) |
| `F2` | Rename on the server |
| `Ctrl+R` | Re-list the current folder |
| `Esc` | Cancel a running operation; otherwise nothing |
| F5 in the **other** panel while the server is shown | Upload that panel's selection into the current server folder (`OnReceiveFiles`), recursively |

**Conflicts** on both download and upload use `ShowChoice` with Overwrite / Overwrite all / Skip / Skip all /
Rename / Cancel, the same set as the host's own copy. For a download, a local file the user chooses to overwrite
is replaced only after the transfer to a temporary name succeeds, so a failed download never destroys the
existing file.

**Progress:** item name, bytes done / total, speed; `Esc` cancels. A cancelled download leaves no partial file
behind (the temporary file is deleted). A cancelled upload deletes the partial remote file when the server allows
it.

**Errors** never freeze or crash the app (FTP-009):

- connect timeouts, login failure (`530`), a refused data connection or a dropped control connection show one
  message with the server's reply text;
- a dropped connection offers **Reconnect** or **Close**;
- every exchange is logged at `Debug` level with `PASS` masked (FTP-006).

### D4 — Saved connections file

- **`%APPDATA%\MyCommander\ftp-connections.conf`:** UTF-8, DEC-007 style, one `[name]` block per connection
  (`host`, `port`, `user`, `folder`, `passive`, `timeout`).
- **No passwords in the file, ever.** Lines that look like `password = …` are ignored and warned about.
- **Reading and writing:** pure parse/format (`FtpConnections.{h,cpp}`) with unit tests; written atomically, the
  same way as `Utf8TextFile.h`'s `WriteUtf8TextFileAtomic`.

### D5 — Passwords and secrets (FTP-006, the ARC-004 analog for FTP; DEC-013)

- **Storage:** passwords are stored only if the user ticks *Save the password*, and then only in the **Windows
  Credential Manager**:
  - `CredWriteW`, type `CRED_TYPE_GENERIC`, target `MyCommander/FTP/<connection name>`, `CRED_PERSIST_LOCAL_MACHINE`;
  - per-user and DPAPI-protected, with no elevation (SEC-002);
  - removed when the connection is deleted.
- **Otherwise** the password is asked at each connect and kept in memory only for that session.
- **Never in:**
  - the log, where the trace shows `PASS ****`;
  - the command history;
  - `ftp-connections.conf`;
  - panel titles or error messages. A `ftp://user:pass@host` paste is split, and the password is never echoed.
- **Plain FTP sends the password unencrypted.** The connection form and the F1 help say so, recommending SFTP
  (EXT-004) or FTPS (FTP-011) for anything sensitive. Anonymous login is the default.

### D6 — SDK addition: password field (API 1.8 → 1.9, backward compatible)

- **`McPluginTypes.h`:** a new `kMcFormFieldPassword`, appended to `McFormFieldKind`. It has the same
  `textBuffer`/`textBufferSize` as Text, and is drawn as `•` per character (or `*` with ACC-006 basic symbols).
  The real characters never reach the screen buffer.
- **Host:**
  - `Dialog.h` `FormFieldKind` gains `Password`, using the same `LineEdit` (UI-016) with masked rendering;
  - `PluginManager`'s `HostShowForm` maps the new kind;
  - a test in `DialogInputTests` checks that editing works the same as Text while the drawn text is masked.
- **Compatibility:** `MC_PLUGIN_API_VERSION_MINOR` 8 → 9. Older plugins are unaffected (a new enum value at the
  end). FtpPlugin declares 1.9, so an older host refuses it cleanly through the existing version check (PLUG-002).
- **Docs:** `DEVELOPER_GUIDE.md`'s Forms section documents the new kind.

### D7 — Installer and menus

- **`MenuBar.h`:** the "FTP connection" and "New FTP connection" stubs are **removed**, as FEA-0007 did for
  Pack/Unpack. Without the plugin, the Network menu simply doesn't list them. With it, the plugin adds them at the
  same place with the same mnemonics. `MenuBarTests` is updated.
- **INST-007:** `Tools/RebuildReleaseForInstaller.ps1` also builds `FtpPlugin` Release, `MyCommanderSetup.rc`
  embeds `payload\FtpPlugin.dll`, and setup installs it into `Plugins\`. A failed FTP plugin build warns and
  doesn't block, like ZIP (IS-0013).

## Requirements added to the spec (§21.1)

| ID | Requirement (summary) | Disposition |
|---|---|---|
| FTP-001 | An FTP MC Plugin provides Network → "FTP connection" (connection manager) and "New FTP connection" (form), replacing the built-in stubs | IN-MVP |
| FTP-002 | A connected server is shown in the active panel as a navigable remote directory; leaving the server root asks to disconnect | IN-MVP |
| FTP-003 | Download (F5) and download-then-delete (F6) of files and folders to the other panel, with progress, Esc and conflict choices; a failed or cancelled download never leaves a partial or destroyed local file | IN-MVP |
| FTP-004 | Upload from the other panel (F5) into the current server folder, recursively, with progress, Esc and conflict choices | IN-MVP |
| FTP-005 | On the server: create folder (F7), delete (F8/Del, always confirmed, recursive), rename (F2), refresh (Ctrl+R), view (F3 via a temporary download) | IN-MVP |
| FTP-006 | Passwords never appear on screen, in logs, in the command history or in the connections file; they're kept only in memory, or in the Windows Credential Manager when the user opts in | IN-MVP |
| FTP-007 | Saved connections live in `%APPDATA%\MyCommander\ftp-connections.conf` (DEC-007 style, no secrets) and can be added, edited and deleted from the manager | IN-MVP |
| FTP-008 | Passive mode (EPSV, then PASV) by default, active optional; binary transfers; UTF-8 names when the server supports them; MLSD when available, else Unix and Windows LIST formats | IN-MVP |
| FTP-009 | Timeouts, login failures, refused data connections and dropped connections fail safely with the server's message, never freeze or crash the app, and offer reconnect | IN-MVP |
| FTP-010 | The plugin form host gets a masked password field (SDK API 1.9, `kMcFormFieldPassword`) | IN-MVP |
| FTP-011 | FTPS (explicit TLS via Schannel), resuming interrupted transfers, permission changes (chmod), background (queued) transfers and compare/sync against a server panel | LATER |
| FTP-012 | The installer bundles the FTP plugin (INST-007) | IN-MVP |

## Files touched

| File | Change |
|---|---|
| `Source/FtpPlugin/` (new): `FtpPlugin.vcxproj`, `FtpPlugin.rc`, `Version.h`, `FtpPlugin.cpp`, `FtpClient.{h,cpp}`, `FtpParse.{h,cpp}`, `FtpConnections.{h,cpp}`, `FtpPluginTests.cpp` | The plugin and its harness |
| `Source/MyCommanderPluginSdk/McPluginTypes.h`, `McPluginAbi.h`, `DEVELOPER_GUIDE.md` | `kMcFormFieldPassword`, API 1.9, docs |
| `Source/MyCommander/Dialog.{h,cpp}`, `DialogInput.cpp`, `PluginManager.cpp` | Masked password field |
| `Source/MyCommander/MenuBar.h`, `Source/MyCommanderTests/MenuBarTests.cpp`, `DialogInputTests.cpp` | Stubs removed; tests |
| `Tools/RebuildReleaseForInstaller.ps1`, `Source/MyCommanderSetup/*` | Bundle the plugin |
| `README.md`, `USERGUIDE.md` ("Connecting to an FTP server"), `CLAUDE.md`, `AIPrompt/my-commander.md`, changelog | Docs; spec rows and status |

## Tests

**`FtpPluginTests.cpp`:** a standalone harness, built with `cl` like the ZIP harness, with no network beyond
loopback.

- **Parsers:**
  - reply framing (single-line, multi-line, a `421` mid-stream);
  - `227`/`229` parsing, including malformed replies;
  - MLSD facts (type, size, modify, perm, UTF-8 names);
  - Unix LIST (dirs, files, symlinks, names with spaces, `Mon dd hh:mm` vs `Mon dd  yyyy`);
  - Windows LIST (`<DIR>`, AM/PM);
  - FEAT; MDTM; server path join and parent.
- **Connections file:**
  - round trip;
  - a `password =` line is ignored with a warning;
  - unknown keys are warned about;
  - Unicode names.
- **Client against a scripted fake server:** a small FTP server thread on `127.0.0.1:0` replays canned dialogues.
  - login (OK, `530` failure);
  - EPSV with PASV fallback;
  - MLSD listing, and LIST fallback when `FEAT` lacks MLSD;
  - RETR into a temporary file, then rename. A failed or cancelled transfer leaves no partial file, and the
    original file is intact;
  - STOR;
  - MKD/DELE/RMD/RNFR-RNTO;
  - a timeout (the server goes silent) returns an error within the limit;
  - a dropped connection;
  - `PollCancel` mid-transfer stops within one tick.
- **Secrets:** the debug trace of a session contains `PASS ****` and never the password.

**Host tests:**

- `DialogInputTests`: the Password field edits like Text and renders masked;
- `MenuBarTests`: the FTP stubs are gone;
- `PluginManagerTests`: the API version check accepts 1.9.

**Manual:**

- a public test server (anonymous, read-only) and a local FileZilla Server or IIS FTP with a test account;
- browse, download a folder tree, upload, rename, delete;
- Esc during a large transfer;
- unplug the network: the reconnect prompt appears;
- non-ASCII names;
- a saved password appears in Credential Manager (Generic) and nowhere in the log or the conf file.

## Phasing

1. **Phase 1:**
   - the SDK password field (host, API 1.9, tests);
   - `FtpParse` and `FtpConnections` (pure, tests);
   - the plugin project skeleton with its menu items;
   - the stubs removed.
2. **Phase 2:**
   - `FtpClient` (session, EPSV/PASV, MLSD/LIST, RETR/STOR, cancel and timeouts) with the fake-server harness;
   - the read-only server panel (connect, browse, F3, F5 download).
3. **Phase 3:**
   - upload, F6, F7, F8, F2, conflicts, reconnect;
   - the Credential Manager;
   - help, installer, docs, spec status (EXT-005 → IN-MVP / Implemented), version, changelog.

## Risks and notes

- **Plain FTP is unencrypted.** This is stated in the form and docs; FTPS is FTP-011 (LATER) and SFTP is EXT-004.
- **The UI is single-threaded.** Listing and transfers run on the UI thread with `PollCancel` ticks, as the ZIP
  plugin's long operations already do. A very slow server makes the panel wait, but Esc always works and every
  wait has a timeout. Background transfers are FTP-011.
- **Server variety.** LIST formats differ between servers. MLSD is preferred, and the LIST parser is tested against
  the two common formats. An unparseable line is shown by name only, never dropped silently.
- **Firewalls:** active mode is rarely usable behind NAT, which is why passive is the default.
- **EXT-002** (a common provider interface) is not solved here. The plugin uses the panel-content mechanism the
  ZIP plugin uses. A future SFTP plugin can reuse `FtpPlugin`'s panel/transfer UI code, but there is no shared
  host abstraction yet.

## Open questions

1. **DEC-012, the protocol implementation:** our own minimal client on Winsock (recommended: UTF-8, Esc and
   timeouts, future FTPS), or WinINet (less code, but the limits above)?
2. **DEC-013, saved passwords:** in the Windows Credential Manager with an opt-in checkbox (recommended), or never
   saved at all?
3. **FTPS:** keep explicit TLS for a later phase (FTP-011, recommended), or include it in this delivery?
4. **Enter on a file:** view it (planned), or download it to the other panel?

## Status

Implemented 2026-10-08, all three phases (host 1.23.0, FtpPlugin 0.1.0). The open questions were answered as
recommended:

- DEC-012: own Winsock client;
- DEC-013: Credential Manager, opt-in;
- FTPS later (FTP-011);
- Enter on a file views it.

EXT-005 and FTP-001…010 and FTP-012 are Implemented.

Changes made while implementing:

- **The password field is `FormField::masked` on the host's Text kind.** There is no separate host kind, so
  editing reuses `ApplyFormFieldKey` unchanged and only drawing differs, through `DisplayedFieldText`. The SDK
  still exposes `kMcFormFieldPassword` as planned.
- **The connection manager is a form with two choice fields:** the connection, and Connect / Edit / Delete /
  New. The SDK has no scrollable list dialog for plugins.
- **PASV connects to the control connection's peer address,** not the address in the `227` reply. That avoids the
  private-address problem behind NAT. The harness's fake server deliberately answers with `10.0.0.99`.
- **`..` is always the first row,** even at the server root, where it asks to disconnect. Going up puts the cursor
  on the folder just left, the same as IS-0019.
- **Symbolic links:** `Enter` on a link first tries it as a folder, and views it as a file if that fails.
- **Testing:** two standalone `cl` harnesses (`FtpPluginTests.cpp`, 64 checks; `FtpPanelTests.cpp`, 23 checks)
  sharing `FakeFtpServer.h`. Like the ZIP harnesses, they aren't part of `build.cmd`; their build lines are in each
  file's header. Sources are compiled with `/utf-8`, because `cl` otherwise reads a BOM-less file in the system
  ANSI code page.
- **Not done here:** a manual session against a real server. It is listed under Manual tests for the user.
