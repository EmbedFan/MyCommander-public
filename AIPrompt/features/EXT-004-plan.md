# Implementation Plan — EXT-004 (SFTP as a remote file-system provider)

Source: `AIPrompt/my-commander.md` §21:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| EXT-004 | SFTP support may be implemented as a remote file-system provider. | P3 | LATER | Not Started |

Neighbours:

- EXT-005: FTP, delivered 1.23.0 as `Source/FtpPlugin/`;
- EXT-003: the plugin API; panel content and `OnReceiveFiles` are the provider mechanism;
- EXT-002: a common provider interface, LATER;
- SEC-001…: never elevate, never leak secrets;
- FTP-011: FTPS, LATER.

## Implementation check (2026-10-09)

Nothing speaks SSH or SFTP today. What exists and is directly reusable:

| What | Where | Reuse for SFTP |
|---|---|---|
| A complete remote-panel UI: connection manager and form, masked password with "save to Credential Manager", server panel (browse, F3/F5/F6/F7/F2/F8, Ctrl+R, selection, mouse), conflict prompts, download-to-temp-then-replace, upload via `OnReceiveFiles`, reconnect, Disconnect menu item, progress and Esc | `Source/FtpPlugin/FtpPlugin.cpp` (~1 200 lines). Only **57 lines** touch `ftp::Client`; everything else is protocol-neutral | **All of it**, once the protocol calls go behind an interface (D2) |
| Saved connections (DEC-007 text, no secrets) and Credential Manager store | `FtpConnections.{h,cpp}` | The same code, with a few extra keys (auth method, key file) |
| `FtpPluginUi` helpers, `FakeFtpServer.h`, the two `cl` harnesses | `Source/FtpPlugin/` | The harness pattern; the panel harness becomes protocol-neutral (D7) |
| Plugin SDK: masked password field (API 1.9), viewer routing (1.10), progress, `PollCancel` | `Source/MyCommanderPluginSdk/` | No SDK change is needed |
| Windows' own OpenSSH client (`ssh.exe`, `sftp.exe`) | `C:\Windows\System32\OpenSSH\` (present on this machine; an optional Windows feature) | Not used as the transport (D1). Its `%USERPROFILE%\.ssh\` keys and agent are honored (D4) |

## Summary

A new **SFTP MC Plugin** (`Source/SftpPlugin/`) gives SFTP the same experience the FTP plugin gives FTP:

- **Menus:** Network → "SFTP connection", "New SFTP connection" and "Disconnect SFTP" (shown while connected);
- **The server panel:** the same keys as FTP;
- **Transfers:** downloads and uploads with conflict choices and Esc;
- **Saved connections and passwords:** passwords in the Credential Manager when opted in.

Three things are SSH-specific:

- **Host-key verification:** first-use trust shows the SHA-256 fingerprint for confirmation. A changed key is a
  hard stop with a warning, never a silent reconnect;
- **Authentication:** password, private key file (with passphrase), or the Windows OpenSSH agent;
- **The panel shows Unix permissions and symbolic links.**

The FTP plugin's protocol-neutral UI is first moved into a shared `Source/PluginCommon/RemotePanel` layer, so both
plugins share one implementation. FTP behavior doesn't change, and its 92 harness checks must keep passing.

## Design

### D1 — The SSH transport: vendor libssh2 with Windows CNG (new DEC-014)

| Option | Verdict |
|---|---|
| **Vendor libssh2 (BSD-3-Clause) built with its WinCNG crypto backend** | **Recommended.** A mature, widely deployed SSH/SFTP client library in plain C. The WinCNG backend uses Windows' own CNG cryptography, so there is **no OpenSSL to vendor or update**. It supports non-blocking sessions (so Esc and timeouts work as in FTP), keyboard-interactive and public-key auth, the agent (including the Windows OpenSSH agent pipe), and known-hosts handling. It is vendored like miniz (DEC-010): source in `Source/SftpPlugin/third_party/libssh2/`, its license kept, compiled into the plugin DLL only, nothing added to the host |
| Drive Windows' `sftp.exe` / `ssh.exe` through pipes | Rejected. It needs the optional Windows feature, scrapes human-oriented output, and `sftp.exe` reads passwords from the console (not stdin), so password logins can't be driven reliably. Progress, Esc and error mapping would be guesswork |
| Write our own SSH implementation | Rejected. It is a large, security-critical protocol (key exchange, ciphers, MACs, host keys); getting it subtly wrong is a vulnerability |

**Algorithm risk to verify in Phase 1:** the WinCNG backend's coverage of modern defaults. OpenSSH servers commonly
offer:

- `curve25519-sha256`, `ecdh-sha2-nistp256`, `diffie-hellman-group14/16-sha256`;
- `aes*-ctr` / `aes*-gcm` ciphers;
- `ssh-ed25519`, `ecdsa-sha2-nistp256`, `rsa-sha2-256/512` host keys.

Phase 1 builds the pinned libssh2 release with WinCNG and connects to a stock Windows OpenSSH Server and a Linux
OpenSSH server. If Ed25519 or Curve25519 is required and unsupported by WinCNG in that release, the fallback is
named in DEC-014's options: libssh2 with its mbedTLS backend (Apache-2.0, also vendorable, no OpenSSL). The plan
reports which backend shipped and the negotiated algorithms appear in the Debug log.

### D2 — Shared remote panel (`Source/PluginCommon/`), FTP refactored onto it

```cpp
// RemoteClient.h -- what the panel needs from a protocol (both plugins implement it)
struct RemoteEntry { std::wstring name; bool isDirectory, isLink; uint64_t size, modifiedUtc; std::wstring permissions; };
enum class RemoteStatus { Ok, Failed, Cancelled, ConnectionLost };
class RemoteClient {
public:
    virtual ~RemoteClient() = default;
    virtual RemoteStatus Connect(const RemoteConnection&, const std::wstring& secret, const CancelFn&, std::wstring& error) = 0;
    virtual void Disconnect() = 0;
    virtual RemoteStatus HomeDirectory(std::wstring&, ...) = 0;   // FTP: PWD; SFTP: realpath(".")
    virtual RemoteStatus List(...) = 0;  virtual RemoteStatus Download(...) = 0;  virtual RemoteStatus Upload(...) = 0;
    virtual RemoteStatus MakeDirectory(...) = 0;  RemoveDirectory, DeleteFile, Rename ...
};
// RemotePanel.{h,cpp} -- today's FtpPlugin.cpp UI, parameterized by a RemoteProtocol
// descriptor: display scheme ("ftp"/"sftp"), default port (21/22), menu labels and
// mnemonics, the connections file name, the credential target prefix, a client
// factory, and the connection-form hook for protocol-specific fields (D4).
```

- **`FtpPlugin.cpp`** shrinks to its exports plus an `FtpRemoteClient` adapter over `ftp::Client`.
- **Builds:** the shared files are compiled into both plugin DLLs, with no new shared DLL and no ABI between
  plugins (the precedent of `../MyCommander/TextWidth.h` in plugin projects). Each plugin stays separately
  installable and disableable (PLUG-010), and a user without SFTP needs never load libssh2.
- **Safety net:** the FTP panel harness (`FtpPanelTests.cpp`) runs unchanged against the refactored code before
  any SFTP code lands.

### D3 — SFTP client (`Source/SftpPlugin/SftpClient.{h,cpp}`, over libssh2)

- **Session:**
  - a non-blocking TCP connect (the `FtpClient` pattern: `select()` ticks, `PollCancel`, a connection timeout);
  - then `libssh2_session_handshake`;
  - host-key check (D5);
  - authentication (D4);
  - `libssh2_sftp_init`.
- **Operations:**
  - `realpath(".")` for the home folder;
  - `opendir`/`readdir_ex`, where the long entry gives permissions and attributes give size, mtime and type;
    symlinks are resolved with `stat` to tell a folder link from a file link;
  - `open`/`read` in 64 KB chunks for downloads (to `<target>.mcftp-part`, then replace, via the shared helper);
  - `open(WRITE|CREAT|TRUNC)`/`write` for uploads, deleting the partial remote file on cancel;
  - `mkdir`, `rmdir`, `unlink`, `rename` (with the overwrite flag only when the user chose Overwrite).
- **Esc and timeouts:** every libssh2 call returning `LIBSSH2_ERROR_EAGAIN` waits on the socket with
  `libssh2_session_block_directions`, ticking `PollCancel` and bounded by the timeout, the same FTP-009 behavior.
- **Names:** SFTP v3 names are bytes; UTF-8 is assumed (OpenSSH), with an ANSI fallback for invalid sequences
  (`DecodeName`, reused).
- **Trace:** Debug-level lines for session steps and SFTP operations, **never** a password, passphrase or key
  material (SFTP-006).

### D4 — Connections and authentication

**`%APPDATA%\MyCommander\sftp-connections.conf`** uses the same format as FTP's, plus:

- `auth = password | key | agent`;
- `key-file = C:\Users\me\.ssh\id_ed25519`;
- the default port is 22.

Never a password or passphrase (warned and ignored, as in FTP).

The **connection form** has the FTP fields, plus:

| Field | Notes |
|---|---|
| Authentication | Choice: Password / Key file / SSH agent. The default is **SSH agent** if the Windows OpenSSH agent pipe exists, else Password |
| Key file | Text (shown for Key file). Defaults to the first of `%USERPROFILE%\.ssh\id_ed25519`, `id_ecdsa`, `id_rsa` that exists |
| Password / passphrase | Masked (API 1.9). It is the account password for Password, and the key's passphrase (empty if none) for Key file |
| Save the password/passphrase (Windows Credential Manager) | Same opt-in as FTP; the target is `MyCommander/SFTP/<name>` |

- The FTP-specific "Passive mode" field isn't shown.
- **Every login prompt** offers saving to the Credential Manager, the same as the FTP rule from 2026-10-08.
- `keyboard-interactive` is supported for servers that use it for passwords: each prompt is answered with the
  password, or shown through the masked form when it is something else, such as an OTP.

### D5 — Host-key verification (new DEC-015; SEC)

- **The store:** `%APPDATA%\MyCommander\known_hosts`, in OpenSSH format, read and written through libssh2's
  knownhost API. **`%USERPROFILE%\.ssh\known_hosts` is also read (never written)**, so hosts the user already
  trusts with `ssh.exe` are accepted without a prompt.
- **An unknown host** asks:

  > **Unknown SSH host**
  > `example.com:22` presented an ED25519 key
  > `SHA256:9Xk…Q2c`
  > [T]rust and remember · [O]nce · [C]ancel

- **A changed key** is a **hard stop**: "The host key for example.com has CHANGED — this could be an attack…",
  showing the old and new fingerprints. There is no "connect anyway" button. The user must remove the old line
  from `known_hosts` deliberately; the message names the file and line.
- Fingerprints are always SHA-256, base64, the same as `ssh-keygen -l`.

### D6 — Menus, keys, installer

- **Network menu:** "SFTP connection" (`P`), "New SFTP connection" (`H`) and "Disconnect SFTP" (`X`, shown while
  connected), with the same visibility rule as FTP.
- **The installer** bundles `SftpPlugin.dll` (an FTP-012 analog), and `build.cmd` builds it.
- **Licensing:** libssh2's `COPYING` ships with the source, and README's third-party notice gains an entry next to
  miniz.

### D7 — Tests

- **Pure parsers:** `sftp-connections.conf` round trip; auth and key-file keys; the password/passphrase line
  ignored with a warning.
- **The shared panel harness:** `FtpPanelTests.cpp` is generalized into `RemotePanelTests.cpp` and driven by an
  **in-memory fake `RemoteClient`**. That covers every panel flow for both protocols with no network: browse,
  conflicts, download-to-temp, upload, F6/F7/F2/F8, reconnect, disconnect, and the SFTP host-key prompt flow
  through a fake that reports "unknown" / "changed".
- **The FTP protocol harness** keeps running unchanged (64 checks) against `FakeFtpServer.h`.
- **SFTP against a real server:** there is no practical in-process SSH server to fake. An **opt-in** integration
  harness `SftpIntegrationTests.cpp` runs when `MC_SFTP_TEST_HOST` / `_USER` / `_KEY` are set, for example the
  Windows OpenSSH Server optional feature on localhost or a test VM. It covers connect, host-key first use, list,
  download, upload, mkdir/rename/delete, a cancel mid-transfer and a dropped connection, and it is skipped (not
  failed) when unset.
- **Secrets:** a password and passphrase never appear in the trace or the log. This is checked in both the
  fake-client harness and the integration harness.

## Requirements to add (spec §21.2, `SFTP-` namespace, with this plan's implementation)

| ID | Requirement (summary) |
|---|---|
| SFTP-001 | Network → "SFTP connection" / "New SFTP connection" / "Disconnect SFTP" (shown only while connected) from an SFTP plugin |
| SFTP-002 | A connected server is a navigable remote directory in the active panel; leaving its root asks to disconnect |
| SFTP-003 | Download (F5/F6) and upload (F5 from the other panel), recursive, with progress, Esc, conflicts and no partial or destroyed files |
| SFTP-004 | F7 / F8 (always confirmed) / F2 / Ctrl+R / F3 on the server |
| SFTP-005 | Authentication by password, private key file (with passphrase) or the Windows OpenSSH agent; keyboard-interactive supported |
| SFTP-006 | Passwords and passphrases never on screen, in logs or in files; optionally in the Credential Manager, offered at every prompt |
| SFTP-007 | Host keys verified against MyCommander's and the user's OpenSSH `known_hosts`. First use requires confirming the SHA-256 fingerprint; a changed key refuses the connection with no override |
| SFTP-008 | Every wait is bounded and Esc works; a dropped connection offers reconnect |
| SFTP-009 | Saved connections in `sftp-connections.conf` (no secrets) |
| SFTP-010 | The installer bundles the plugin |
| SFTP-011 (LATER) | Changing remote permissions, resuming transfers, background transfers, server-to-server copy, port forwarding/jump hosts |

The new decisions are **DEC-014** (SSH transport: vendored libssh2 + WinCNG, with mbedTLS as the fallback) and
**DEC-015** (host-key trust: MyCommander's `known_hosts`, also reading OpenSSH's, TOFU with SHA-256 confirmation,
no override on a changed key).

## Files touched

| File | Change |
|---|---|
| `Source/PluginCommon/` (new): `RemoteClient.h`, `RemotePanel.{h,cpp}`, `RemoteConnections.{h,cpp}`, `RemoteUi.{h,cpp}`, `RemotePanelTests.cpp`, `FakeRemoteClient.h` | Shared remote-panel layer and harness (moved from FtpPlugin) |
| `Source/FtpPlugin/` | `FtpPlugin.cpp` reduced to exports plus `FtpRemoteClient`; project includes the shared files; `FtpPanelTests.cpp` replaced by `RemotePanelTests` (FTP mode) |
| `Source/SftpPlugin/` (new): `SftpPlugin.vcxproj/.rc`, `Version.h`, `SftpPlugin.cpp`, `SftpClient.{h,cpp}`, `KnownHosts.{h,cpp}`, `SftpIntegrationTests.cpp`, `third_party/libssh2/` (+ `COPYING`, `VENDORED.md`) | The plugin |
| `build.cmd`, `Tools/RebuildReleaseForInstaller.ps1`, `Source/MyCommanderSetup/*` | Build and bundle |
| `README.md`, `USERGUIDE.md` ("Connecting to an SFTP server"), `DEVELOPER_GUIDE.md` (shared remote layer note), `CLAUDE.md`, `AIPrompt/my-commander.md`, changelog, `Version.h` | Docs, spec §21.2, DEC-014/015, EXT-004 → Implemented, version 1.25.0 |

## Phasing

1. **Phase 1, the foundation:**
   - extract `PluginCommon/RemotePanel` from FtpPlugin, with the FTP harnesses green and FTP behavior identical;
   - vendor libssh2 (WinCNG), connect, handshake and list against Windows OpenSSH Server and a Linux OpenSSH, and
     record the negotiated algorithms and the backend decision (DEC-014).
2. **Phase 2, SFTP browsing:**
   - `SftpClient` (connect, host keys with `KnownHosts`, the three auth methods, list, download, F3);
   - the SFTP connection form and manager on the shared panel;
   - fake-client panel tests including the host-key prompts.
3. **Phase 3, SFTP writes and delivery:**
   - upload, F6/F7/F2/F8, reconnect, Credential Manager;
   - the integration harness;
   - installer, docs, spec (§21.2, DEC-014/015, EXT-004 → Implemented), version, changelog.

## Risks and notes

- **Crypto coverage of the WinCNG backend:** the main technical risk, retired in Phase 1 by a real handshake test,
  with the mbedTLS backend as the planned fallback.
- **Security surface:** vendored C code that parses network input. It is pinned to a current libssh2 release
  (recorded in `VENDORED.md`, upgraded deliberately). Host-key checking is strict by design; a changed key is never
  bypassable from the UI.
- **The refactor risk to FTP** is contained by running the existing 92 FTP checks before and after the move.
- **No in-process SSH server** for tests, hence the fake `RemoteClient` for UI coverage plus an opt-in real-server
  harness. The real-server part is manual unless the environment variables are set.
- **The agent** depends on the Windows "OpenSSH Authentication Agent" service running. If its pipe is missing,
  "SSH agent" reports that clearly instead of failing obscurely.
- **Out of scope:** SCP, port forwarding, jump hosts / ProxyCommand, `~/.ssh/config` parsing (beyond keys and
  `known_hosts`), chmod, resume, background transfers, FTP↔SFTP direct copy.

## Decisions (accepted by the user 2026-10-09)

1. **Transport:** vendored libssh2 with the Windows CNG backend; the mbedTLS backend is the fallback if Phase 1
   shows WinCNG lacks the algorithms modern servers require (DEC-014).
2. **Structure:** a separate `SftpPlugin`, sharing a `Source/PluginCommon/` remote-panel layer with FTP.
3. **Host keys:** MyCommander's own `known_hosts`, plus the user's OpenSSH `%USERPROFILE%\.ssh\known_hosts`
   read-only. First use needs SHA-256 confirmation; a changed key is refused with no override (DEC-015).
4. **Authentication:** password, private key file with passphrase, and the Windows OpenSSH agent;
   keyboard-interactive is supported.

## Status

Planned 2026-10-09. Decisions accepted 2026-10-09. **Implemented 2026-10-09** (host 1.25.0, SftpPlugin 0.1.0),
all three phases. Differences from the plan, decided during implementation:

- **The crypto backend.** WinCNG with `LIBSSH2_ECDSA_WINCNG` was enough: `test.rebex.net` negotiated
  `ecdh-sha2-nistp256` / `ecdsa-sha2-nistp256` / `chacha20-poly1305`, so the mbedTLS fallback wasn't needed.
- **Key files.** WinCNG loads only PEM RSA (with or without a passphrase) and unencrypted OpenSSH-format ECDSA.
  Ed25519, OpenSSH-format RSA and encrypted ECDSA key files get a clear message pointing to the SSH agent; RSA keys
  also get the one-time `ssh-keygen -p -m PEM` conversion. They are listed under SFTP-011. The default key file
  order is therefore `id_ecdsa`, `id_rsa`, `id_ed25519`.
- **The connection form.** It always shows the "Key file" field, because the SDK form can't hide fields.
- **Tests.**
  - `FtpPanelTests.cpp` was kept as-is (28 checks green on the shared panel) instead of being renamed.
  - The SFTP panel coverage is a new `SftpPanelTests.cpp` on `PluginCommon/FakeRemoteClient.h`.
  - All the harnesses are built by `Source/SftpPlugin/BuildTests.cmd`.
- **A new panel rule for both protocols.** A symbolic link to a folder is never descended into when copying, and
  deleting a link removes only the link.
- **Integration test.** It ran read-only against `test.rebex.net`, with 11 checks: first-use prompt and remember,
  silent reconnect, a changed key refused (line reported), list, download and errors. The write and Esc
  checks need a writable server (`MC_SFTP_TEST_DIR`) and have not been run yet.
