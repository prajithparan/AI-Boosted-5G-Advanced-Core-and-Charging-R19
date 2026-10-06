## ADR-0283: CDR retention and archival (P14) -- archive first, delete second

**Date:** 2026-09-05
**Status:** Accepted

The last P4.12 item. `CdrWriter::apply_retention` archives every `cdr` row older than a configured
window into newline-delimited JSON, then deletes **only after** that archive is written and
flushed.

### The ordering is the design

These are billing records. Deleting one that was not archived destroys revenue evidence, so:

- the rows are SELECTed in full first;
- the archive file is written, flushed, and its stream state checked;
- **only then** does the DELETE run;
- any failure at any step deletes nothing and returns `failed`, and the next hourly sweep retries.

Data kept twice is a storage cost. Data deleted once is gone. The DELETE deliberately repeats the
SELECT's own predicate rather than deleting by collected key, so a row that aged between the two
statements is archived by the *next* sweep instead of being deleted unarchived by this one.

### Off by default, and not only out of caution

`cdr_retention_days` absent or <= 0 disables the sweep. A retention window is an operator
compliance decision -- regulatory retention periods differ by jurisdiction -- and a default that
silently deleted CDRs after N days would be this project choosing someone's compliance posture for
them.

### Test, and where it really runs

`tests/integration/test_cdr_retention.cpp` drives CHF's **real** `CdrWriter` (its translation units
are compiled into the test binary; CHF is an executable, so there is no library target to link).
It `GTEST_SKIP`s without a reachable Doris and runs for real in CI's **`build` job**, which is the
one that attaches `apache/doris:all-in-one` on 9030 with the schema applied and sets the
`CHF_DORIS_*` env. The two **sanitizer jobs have no Doris service, so it skips there** -- verified
in run 33952950176 rather than assumed: `build` reports `Passed 0.21s`, `sanitize (tsan)` reports
`Skipped`. Stated at this precision because "validated in CI" would otherwise imply all three legs
exercise it.

What it asserts is chosen deliberately:

- rows written seconds ago **survive** a 3650-day window -- a retention sweep that deletes live data
  is a worse bug than one that never runs;
- `retention_days <= 0` does nothing at all;
- `deleted <= archived` -- the invariant the whole feature exists to hold;
- and when anything was swept, the archive file is **read back** and its JSON lines counted against
  what was deleted, rather than concluding from a shrinking table that the data was saved.

### Disclosed

- **The archive sink is a local directory, not object storage.** `docs/DATA_MODEL.md`'s E4 assigns
  archival to an object store; none is deployed in this project (no MinIO/S3 in the compose file).
  Newline-delimited JSON is what such a loader would ingest, so the format is right and the
  destination is not.
- **No restore path.** Nothing reads an archive file back into Doris; archival without a tested
  restore is only half of an archival story, and this is the half that exists.
- **The sweep is hourly and unsynchronised.** Two CHF instances would each sweep; the DELETE is
  idempotent by predicate so this is not a correctness bug, but it is duplicated archive files --
  which lands with the state-externalisation work assigned to P11 (ADR-0284).

---

