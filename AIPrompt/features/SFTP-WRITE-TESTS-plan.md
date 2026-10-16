# Test Plan — SFTP write checks against a real server (EXT-004 evidence)

Source: `AIPrompt/features/EXT-004-plan.md` ("checks need a writable server (`MC_SFTP_TEST_DIR`) and have not been
run yet"), and the spec's SFTP rows: SFTP-003 (transfers never leave a partial or destroyed file), SFTP-004 (file
operations on the server), SFTP-006 (no secret in traces) and SFTP-008 (cancel and connection loss).

This is a **verification task**, not a feature. No product code changes, unless a check fails.

## Current state (2.0.0.368)

- **`Source/SftpPlugin/SftpIntegrationTests.cpp`** is an opt-in harness against a **real** SSH server, built and run
  by `Source\SftpPlugin\BuildTests.cmd`. Without `MC_SFTP_TEST_HOST` it prints "skipped" and exits 0.
- **Run so far:** only the read-only part, against the public `test.rebex.net` (user `demo`, password `password`):
  - connect;
  - the host-key prompt, then a remembered key connects silently, and a changed key is refused (SFTP-007);
  - list, download;
  - a missing folder is an error, not a lost connection;
  - no secret in the trace.
- **Never run:** the block guarded by `MC_SFTP_TEST_DIR` (`SftpIntegrationTests.cpp:133-154`):

  | # | Check | Requirement |
  |---|---|---|
  | W1 | `mkdir` of a unique `mc_it_<tick>` folder | SFTP-004 |
  | W2 | upload of 300,000 bytes | SFTP-003 |
  | W3 | rename on the server | SFTP-004 |
  | W4 | download what was uploaded, byte-identical | SFTP-003 |
  | W5 | **Esc mid-upload:** `Cancelled`, and the session is still usable afterwards (a List works) | SFTP-008 |
  | W6 | delete the file | SFTP-004 |
  | W7 | `rmdir` | SFTP-004 |

- The panel-level write paths (F5/F6/F7/F8/F2 with conflicts and progress) are covered by `SftpPanelTests.cpp`
  against a fake client only.

## Step 1 — Get a writable test server (the user's choice)

| Option | Effort | Notes |
|---|---|---|
| **A. Windows OpenSSH Server on this PC** (recommended) | One-time, needs admin: `Add-WindowsCapability -Online -Name OpenSSH.Server~~~~0.0.1.0`, `Start-Service sshd` | Host `localhost`, a local account's password, or a key. Fast and repeatable. Tests run as that account, so `MC_SFTP_TEST_DIR` can be e.g. `/C:/Users/<user>/mc_sftp_test`. Stop or disable the service afterwards if it isn't wanted permanently |
| B. Docker `atmoz/sftp` | Needs Docker Desktop | `docker run -p 2222:22 -d atmoz/sftp test:test:::upload`, then `MC_SFTP_TEST_PORT=2222`, `MC_SFTP_TEST_DIR=/upload`. A Linux server, closer to real hosting |
| C. WSL Ubuntu + `openssh-server` | WSL is **currently broken** on this machine (missing `ext4.vhdx`) | Needs a WSL repair first |
| D. A real remote Linux server or VPS | Depends on access | The most realistic: real latency, which makes W5 more meaningful |

**No public test server is writable** (test.rebex.net is read-only), so one of these is needed. A and B only touch
the user's machine. Nothing here asks Claude to change system settings: **the user installs or starts the
server**, since it needs elevation (SEC-002).

## Step 2 — Run the existing write checks

```cmd
set MC_SFTP_TEST_HOST=localhost
set MC_SFTP_TEST_USER=<user>
set MC_SFTP_TEST_PASSWORD=<password>    (or MC_SFTP_TEST_KEY / MC_SFTP_TEST_AGENT=1)
set MC_SFTP_TEST_DIR=<writable folder>
Source\SftpPlugin\BuildTests.cmd
```

- The password is set by the user in their own shell, e.g. with `! set ...` or before starting the session. It is
  never written into a file, a plan or the changelog (SFTP-006). The harness's trace check (`:192-197`) also
  verifies it never appears in the trace.
- **Expected:** all W1–W7 pass, plus the existing read-only checks. The run leaves nothing behind on the server: the
  test folder is removed by W7.
- **If W5 is flaky** on localhost (the upload finishes before the first progress callback can cancel it), make the
  upload bigger for that check (e.g. 8 MB). That's a test-only change.

## Step 3 — Additional write checks (proposed; added to the same harness block)

| # | Check | Why |
|---|---|---|
| W8 | **Overwrite:** upload over an existing file. The content is replaced and the size is right | SFTP-003; the conflict "overwrite" path at the client level |
| W9 | **Esc mid-download:** no partial local target is left (`.mc-part` removed); the original local file, if any, is intact | SFTP-003, symmetric to W5 |
| W10 | **Large file:** 50 MB up and down, identical (a SHA-256 comparison), with the throughput printed | Chunking and window handling over many round trips |
| W11 | **Unicode names:** `árvíztűrő_тест_日本.txt` up, list, down, rename, delete | UTF-8 on the wire (SFTP-005) |
| W12 | **A file with a space and a leading dash** (`- odd name.txt`) | Quoting/escaping safety |
| W13 | **Recursive delete** of a folder holding files and a subfolder (through the plugin's tree walk) | SFTP-004 |
| W14 | **A symlink on the server** (where the server allows `symlink`): it's listed as a link and never followed by a recursive delete | SEC-006 for remote trees |
| W15 | **Permission denied:** writing into a read-only folder gives `Failed` with the server's message, and the session stays usable | Error reporting, SFTP-008 |
| W16 | **Dropped connection:** stop `sshd` (or the container) mid-transfer; expect `ConnectionLost`, and then a working reconnect | SFTP-008. **Manual**, since it needs the server stopped at the right moment |

W8–W15 are automated and guarded by `MC_SFTP_TEST_DIR`, exactly like W1–W7. W14 is skipped with a note when the
server refuses to create a link.

## Step 4 — Manual checks in the app (same server)

- Connect from the Network menu, with the password saved and not saved (Credential Manager, opt-in).
- F5 a folder tree to the server and back; F6 within the server (a rename); F8 a tree; F7 mkdir.
- Esc during a large F5. The progress box closes, and the panel shows no partial file afterwards.
- Overwrite prompts (Overwrite / Skip / All).
- Stop `sshd` during a transfer: the reconnect prompt appears.
- Check `%APPDATA%\MyCommander\mycommander.log` (at `logLevel = debug`) and `sftp-connections.conf` for the password.
  **It must not appear.**

## Step 5 — Record the results

- **Changelog entry:**
  - which server was used (the kind and OpenSSH version, never the credentials);
  - the pass/fail count;
  - any fixes.
- **Spec:** add the run as evidence to the SFTP-003/004/008 rows. Mark EXT-004's "write checks not yet run" note
  as done in `EXT-004-plan.md`.
- **`ManualTests.md`:** MT entries for Step 4. The DOC-SYNC plan's MT-025…MT-047 numbering continues from them.
- **Commit:** the harness additions (W8–W15) and the docs.

## Files touched

| File | Change |
|---|---|
| `Source/SftpPlugin/SftpIntegrationTests.cpp` | W8–W15 (and a larger W5 if needed) |
| `AIPrompt/features/EXT-004-plan.md`, spec SFTP rows, `ManualTests.md`, changelog | Evidence |

## Open questions

1. **Which server:** A (Windows OpenSSH on localhost, recommended), B (Docker), or D (a remote server you have)?
2. **Additional checks:** add W8–W15 now, or only run the existing W1–W7 first?
3. **Dropped-connection test (W16):** do it manually only (recommended)? Or automate it by stopping the service from
   the harness, which needs admin rights?

## Status

Planned 2026-10-12.

**2026-10-14 — checks written, not yet run against a writable server.**

- W8–W15 are in `SftpIntegrationTests.cpp`, inside the same `MC_SFTP_TEST_DIR` block as W1–W7, ahead of W6/W7 so
  W7 still removes an empty folder (a recursive cleanup runs only if something was left behind). W5 keeps its
  300,000-byte upload; enlarge it only if it proves flaky.
- W9 uses the real `remote::DownloadReplacing` with the SFTP plugin's `.mcsftp-part` suffix. W10 compares the
  50 MB file byte by byte (stronger than a SHA-256 comparison) and prints the throughput. W13/W14 use a copy of
  `RemotePanel.cpp`'s `DeleteTree` rule. W14 tries both `SSH_FXP_SYMLINK` argument orders (servers disagree) and is
  skipped with a note if the server refuses. W15 needs the new optional `MC_SFTP_TEST_READONLY_DIR`, else it is
  skipped with a note.
- One small addition outside the harness: `sftp::Client::MakeRemoteSymlink` (not on `remote::Client`; the panel
  never creates links), needed by W14.
- `BuildTests.cmd` now also links `RemoteUi.cpp` into the integration harness, and finds Visual Studio through
  `vswhere` when the hard-coded VS 2022 Community path doesn't exist.
- **Run so far:** skip mode (no host) and read-only against `test.rebex.net`: 11 checks, 0 failures; logic 37/0,
  panel 38/0. No writable server was available on this machine (no `sshd`, no Docker, port 22 closed), so
  W1–W15 have still **never run**. W16 and MT-051 (Step 4) are manual.
- **Which tests need a real SFTP server** (also marked in `SftpIntegrationTests.cpp`'s header, its skip messages
  and `BuildTests.cmd`):

  | Harness / checks | Server needed |
  |---|---|
  | `SftpLogicTests` (37) | None |
  | `SftpPanelTests` (38): panel write paths against `FakeRemoteClient` | None (in-memory fake) |
  | `SftpIntegrationTests` read-only checks (11): connect, host keys, list, download, error, trace | **Real server**, read-only is enough (`test.rebex.net`) |
  | `SftpIntegrationTests` W1–W15 | **Real writable server** (`MC_SFTP_TEST_DIR`); W15 also `MC_SFTP_TEST_READONLY_DIR` |
  | W16, MT-051 | **Real writable server**, manual |

- **Remaining:** the user starts a writable server (Step 1) and runs Step 2 with `MC_SFTP_TEST_DIR` set. Then
  Step 5: spec evidence on SFTP-003/004/008, the EXT-004 plan note, MT-051 result, changelog.
