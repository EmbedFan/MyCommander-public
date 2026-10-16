# Planned features backlog

Features that have an implementation plan and are not yet implemented. The plan file for each row is in this folder.

**Rule:** when a feature is implemented, remove its row from this table in the same change. A row marked **Postponed**
stays in the table (and its plan stays valid) until it is implemented or the user drops it. When the table is empty,
delete this file.

| ID | Feature | Key | Depends on | Status | Plan |
|---|---|---|---|---|---|
| SRC-011 | Find recently changed files | menu only | SRC-010 (implemented) | **Postponed** | [SRC-011-plan.md](SRC-011-plan.md) |
| SRC-012 | Find entries by attribute | menu only | SRC-010 (implemented) | **Postponed** | [SRC-012-plan.md](SRC-012-plan.md) |

Note for SRC-011 and SRC-012: SRC-010 added only `SearchResult::sizeBytes` (see its plan's status). The `lastWriteUtc` and
`attributes` result fields and the shared `EntryFilter` predicate that these two plans assume are added by whichever of
them is implemented first.

Postponed 2026-10-16 by the user's decision: everything still in the backlog. Neither feature has a row in
`AIPrompt/my-commander.md` yet, so there is no Disposition field or Postponement Register entry to change; a spec row
(Disposition `LATER`) is added only if the feature is taken up again.

Updated 2026-10-16: the other eleven rows (LIST-010, LIST-011, LIST-012, FOP-022, FOP-023, FOP-024, FOP-025, FOP-026,
NAV-018, NAV-019, CLI-008) are implemented and were removed.
