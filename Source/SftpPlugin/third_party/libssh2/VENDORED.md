# Vendored: libssh2

- **Upstream:** https://github.com/libssh2/libssh2
- **Version:** 1.11.1 (release tarball `libssh2-1.11.1.tar.gz`)
- **License:** BSD-3-Clause (`COPYING`, kept unchanged)
- **Decision:** DEC-014 (EXT-004, `AIPrompt/features/EXT-004-plan.md`)

## What was copied

- `include/*.h`;
- `src/*.c` and `src/*.h`, **except** the crypto backends that aren't used: `openssl.*`, `mbedtls.*`, `libgcrypt.*`,
  `os400qc3.*`.

No file is modified.

## How it is built

The files are compiled directly into `SftpPlugin.dll`, with no separate library project:

- **Preprocessor definitions:**
  - `LIBSSH2_WINCNG`: the Windows CNG (`bcrypt`) crypto backend, with no OpenSSL;
  - `LIBSSH2_ECDSA_WINCNG`: ECDSA host keys and `ecdh-sha2-nistp*` key exchange. These need Windows 10 or later,
    which is the app's target anyway;
  - `LIBSSH2_API=`: a static build inside the DLL (otherwise `_WINDLL` turns on dllimport).
- **Not compiled on their own:** `agent_win.c`, `blowfish.c` and `wincng.c`. They are `#include`d by `agent.c`,
  `bcrypt_pbkdf.c` and `crypto.c`.
- **Linked libraries:** `ws2_32`, `bcrypt`, `crypt32`, `user32` (the Pageant window lookup in the agent code).

## What the WinCNG build supports (checked 2026-10-09)

Checked against `test.rebex.net`:

- **Negotiated:** `ecdh-sha2-nistp256`, `ecdsa-sha2-nistp256` and `chacha20-poly1305@openssh.com` with
  `hmac-sha2-256`.
- **Also available:** `diffie-hellman-group14/16/18-sha*`, `rsa-sha2-256/512`, `aes*-ctr`.
- **Not available with WinCNG:**
  - `curve25519-sha256` key exchange;
  - `ssh-ed25519` host keys, and **Ed25519 private key files**. An Ed25519 key still works through the SSH agent,
    which does the signing;
  - `aes*-gcm`.

## Upgrading

Replace the files with a newer release's in the same way. Then re-run the plugin's harnesses and the opt-in
integration test (`SftpIntegrationTests.cpp`), and update the version above.
