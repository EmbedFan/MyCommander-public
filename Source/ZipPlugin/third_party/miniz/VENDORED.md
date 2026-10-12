# Vendored: miniz

- **Source:** https://github.com/richgel999/miniz
- **Version:** 3.1.2 (release tag `3.1.2`, published 2026-07-01)
- **Fetched:** 2026-10-01, from the release's own `miniz-3.1.2.zip` asset
  (https://github.com/richgel999/miniz/releases/download/3.1.2/miniz-3.1.2.zip), which already
  ships the amalgamated `miniz.c`/`miniz.h` pair used here unmodified.
- **License:** MIT — see `LICENSE` in this directory (the repository's own top-level license file
  as of this version). Note: `miniz.h`'s own internal header comment still carries stale
  2013-era "public domain... see unlicense statement at the end of this file" wording from a much
  earlier release; that claim is superseded by this release's actual `LICENSE` file, which is the
  governing license for the version vendored here (DEC-010, `AIPrompt/features/FEA-0007-plan.md`).
- **Not modified** from the upstream release — any future update should replace `miniz.c`/
  `miniz.h`/`LICENSE` wholesale from a newer release rather than hand-editing these copies, so a
  diff against upstream stays trivially checkable.
