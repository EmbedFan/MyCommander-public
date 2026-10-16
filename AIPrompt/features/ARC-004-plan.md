# Implementation Plan — ARC-004 (password-protected archives)

Source: `AIPrompt/my-commander.md`:

| ID | Requirement | Priority | Disposition | Implemented |
|---|---|---:|---|---|
| ARC-004 | Password-protected archive support shall not expose passwords in logs or command histories. | P2 | LATER | Not Started |
| ZIP-008 | Nested archive browsing, password/encrypted archives, and an archive integrity check are useful extensions not required for the initial release. | P3 | LATER | Not Started |

ARC-004 is a safety rule about passwords. It only becomes meaningful once there is password-protected archive
support at all, which ZIP-008 lists. This plan delivers both: **encrypted ZIP read and write** in the ZIP plugin,
following ARC-004's rule. The rest of ZIP-008 (nested archives, an integrity check) stays LATER.

## Implementation check (2.0.0.368)

- **The ZIP plugin uses vendored miniz 3.1.2 (DEC-010).** miniz **refuses encrypted entries in every read path**,
  even with `MZ_ZIP_FLAG_COMPRESSED_DATA`. It checks the general-purpose bit 0 and strong encryption and returns
  `MZ_ZIP_UNSUPPORTED_ENCRYPTION` (`miniz.c:3970/4597/4814/5026`). It has no writer support for encryption.
- **Today:** an encrypted entry fails to extract ("failed extracting …", logged). Browsing still lists the names,
  because ZIP names are not encrypted. Nothing asks for a password.
- **Reusable:**
  - the plugin already **patches ZIP headers after miniz finalizes** (external attributes, ZIP-003), so writing extra
    header fields is an established technique;
  - the SDK's masked password field (`kMcFormFieldPassword`, API 1.9, FTP-010);
  - the FTP/SFTP secret-handling rules (memory only, `SecureZeroMemory`, never logged), and the tests that prove "no
    secret in the log";
  - CNG (`bcrypt.lib`) for AES, HMAC-SHA1 and PBKDF2.

## Design

### D1 — Formats

| Format | Read | Write | Notes |
|---|---|---|---|
| **WinZip AES** (method 99, extra field `0x9901`, AE-1/AE-2, AES-128/192/256) | Yes | **Yes, AES-256 AE-2** (the default) | PBKDF2-HMAC-SHA1 with 1000 iterations from salt + password; AES-CTR with a little-endian counter from 1; HMAC-SHA1 authentication code (10 bytes); 2-byte password verifier. 7-Zip, WinZip and WinRAR read it |
| **Traditional PKWARE ("ZipCrypto")** | Yes | No (open question 2) | Cryptographically weak; supported so old archives open. The 12-byte header check rejects a wrong password, the CRC confirms a right one |
| PKWARE strong encryption, `.7z`/`.rar` | No | No | Out of scope; reported clearly ("not supported") |

- **Crypto:** CNG only (`BCryptOpenAlgorithmProvider`: AES in ECB to build CTR, HMAC-SHA1, `BCryptDeriveKeyPBKDF2`).
  No new third-party code. ZipCrypto's tiny CRC-based stream cipher is implemented directly, in about 40 lines.

### D2 — Reading encrypted entries without miniz's reader

- **New module `ZipCrypto.{h,cpp}`** in the ZIP plugin, pure apart from CNG:
  1. Find the entry's local header from miniz's central-directory stat (`m_local_header_ofs`, `m_comp_size`). Read
     the local header (30 bytes + name + extra) and the raw data straight from the file.
  2. Decrypt it: AES (verify the password verifier, then CTR-decrypt and check the HMAC) or ZipCrypto (header
     check).
  3. Inflate with miniz's raw `tinfl` (method 8) or copy (method 0). Check the CRC (ZipCrypto, AE-1) or the HMAC
     (AE-2).
- **Wrong password:** reported as "wrong password", not as corruption. The panel or unpack asks again (up to three
  times), with Cancel.

### D3 — Writing encrypted entries

- **Pack form:** a new **"Encrypt with password (AES-256)"** checkbox. When ticked, a masked password plus a
  confirmation field are asked for, with a minimum length warning.
- **Write path:**
  1. Each file is deflated by miniz's `tdefl` into memory.
  2. It is encrypted (AE-2, CRC 0).
  3. It is added with `mz_zip_writer_add_mem_ex_v2(... MZ_ZIP_FLAG_COMPRESSED_DATA ...)` plus the `0x9901` extra field
     as user extra data.
  4. **After finalize**, the patch step (which already exists for attributes) rewrites the method to 99 and sets
     general-purpose bit 0 in both the local and central headers.
- **Browsing a protected archive:** files copied **into** it are encrypted with the archive's password, if one was
  given in this session. Mixing encrypted and plain entries is allowed by the format, but the plugin avoids it.

### D4 — Password handling (ARC-004 proper)

- **Asked:** in a masked field (API 1.9), once per archive per session, when the first encrypted entry is needed.
  That covers viewing (F3), copying out, unpacking and browsing into it.
- **Kept:** in memory only, in a `SecureString` buffer owned by the open archive view or the unpack operation. It is
  zeroed (`SecureZeroMemory`) when the archive is closed or the operation ends, and on plugin shutdown.
- **Never** written to the log (the entry names and outcomes are logged, the password never), the command history,
  the session file, the config, `keys.conf`, the Credential Manager, or a temporary file.
- **Temporary files:** F3 on an encrypted entry writes the **decrypted** content to a temporary file for the
  viewer (as today). It is created in `%TEMP%` with a non-obvious name, deleted immediately after viewing, and the
  limitation is documented.
- **Unpacking many protected archives:** one prompt per archive. A "use the same password for the rest" checkbox is
  offered; the password then lives only until that batch ends.

## Files touched

| File | Change |
|---|---|
| `Source/ZipPlugin/ZipCrypto.{h,cpp}` (new) | AES (CNG), ZipCrypto, raw entry reader, encrypt for write |
| `Source/ZipPlugin/ZipPlugin.cpp` | Password prompts; the read paths (view, copy out, unpack, browse); the pack form option; copy-in encryption; the header patch extension |
| `Source/ZipPlugin/ZipPlugin.vcxproj` | `bcrypt.lib`, new sources |
| `Source/ZipPlugin/ZipCryptoTests.cpp` (new harness) + `ZipNavigationTests.cpp` | Tests |
| `Source/ZipPlugin/Version.h` | Plugin MINOR |
| README, USERGUIDE (ZIP section: password-protected archives), the plugin help markdown, CLAUDE.md, spec (ARC-004 → IN-MVP / Implemented; ZIP-008 split: encrypted archives Implemented, nested/integrity still LATER), changelog, `Version.h` | Docs |

## Tests (`ZipCryptoTests.cpp` harness plus the existing ZIP harnesses)

- **Known-answer tests:**
  - AES-CTR, HMAC-SHA1 and PBKDF2-SHA1 against the published WinZip AE test vectors;
  - the ZipCrypto key schedule on a known archive;
  - **fixture archives** (checked in, small) produced by 7-Zip: AES-128 and AES-256, AE-1 and AE-2, ZipCrypto,
    stored and deflated. All decrypt to the expected content.
- **Round trip:** pack with AES-256, then read back with our reader. The archive must also be readable by 7-Zip,
  checked manually and recorded in the test notes.
- **Wrong password:** reported as "wrong password", no partial output file, re-prompt.
- **Corruption:** a flipped ciphertext byte fails the HMAC check; nothing is written.
- **ARC-004:** run every flow with the password `"S3cretPw!"`, then scan the captured log, the session and config
  files, and the temporary folder. **The password must not appear anywhere.** After closing, the in-memory buffer is
  zeroed (checked through a test hook).
- **Zip-slip** still holds for encrypted entries.

## Phasing

1. Read: AES and ZipCrypto, view/copy out/unpack/browse, the password prompt and its lifetime. Tests and fixtures.
2. Write: AES-256 packing, copying into a protected archive.
3. Docs and spec.

## Risks

- **Getting crypto right:** mitigated by published test vectors, fixtures from a reference tool (7-Zip), and CNG
  doing the primitives (no hand-written AES).
- **Decrypted temporary files for F3:** they are short-lived and deleted at once, and this is documented. Viewing
  in memory would need the viewer to accept a buffer: a possible later improvement.
- **Weakness of ZipCrypto:** we only read it, and the UI says "legacy, weak encryption" when such an archive is
  opened.

## Open questions

1. **Scope:** read and write (AES-256) as planned? Or read-only first?
2. **ZipCrypto:** offer writing it for compatibility with very old tools (with a warning)? Or read-only (recommended)?
3. **Password lifetime:** remember an archive's password for the whole session after the first prompt
   (recommended)? Or ask again for every operation?

## Status

Implemented 2026-10-14 (read and write, with the recommended answers to the open questions: AES-256 write,
ZipCrypto read-only, password cached for the open archive).

Differences from the plan:

- `ZipBuilder` assembles the archive itself instead of patching miniz's writer output (no `0x9901` patching
  needed; classic ZIP only: 65,535 entries / 4 GiB, reported when exceeded).
- No "use the same password for the rest" checkbox: an Unpack batch simply tries the previous archive's password
  silently before asking again.
- Esc at the password prompt stops the whole unpack; for browsing it leaves the archive closed. An encrypted entry
  that cannot be read aborts opening (never silently dropped, so a later save cannot lose it).
- An edited archive that was encrypted is saved encrypted with the cached password (re-asked if a failed reload
  cleared it, never written plain); on plugin shutdown without the password the auto-save is skipped and logged.
- Fixtures are produced by Windows' `tar.exe` (libarchive) rather than 7-Zip, which is not installed here.
- Still true: F3 on an entry hands the viewer a decrypted temporary copy (the existing archive-viewer behaviour).
