## ADR-0127: gap-closure task #106 continuation -- UDR real Trace Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (39 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0126). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/{servingPlmnId}/provisioned-data/trace-data` (real response schema
`TraceDataOrSharedTraceDataId` -- a real `oneOf` of the full `TraceData` object
(`TS29571_CommonData.yaml` -- `traceRef`/`traceDepth`/`neTypeList`/others) or a bare `SharedDataId`
string reference) is genuinely `GET`-only (`QueryTraceData`) -- no create/update operation exists
at all, same real reasoning already established for every other `provisioned-data` sub-resource.
Same real `(ueId, servingPlmnId)` composite key as the other six sub-resources already in this
table. This project's stores persist/return raw opaque JSON for every `provisioned-data`
sub-resource (no strong DTO layer at the store level), so the real `oneOf` union needs no special
handling -- whichever real shape is seeded is returned verbatim; used the full `TraceData` object
shape for this project's own seed data since it's the more informative of the two real
alternatives. Same real "genuinely part of the same resource group" shape ADR-0106 established for
`lcs_bca_data` -- `trace_data` follows that exact precedent as a 7th column.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `trace_data JSONB` column on `udr_provisioned_data`, plus the
  matching `ALTER TABLE ... ADD COLUMN IF NOT EXISTS` for existing databases.
- `nfs/udr/src/stores.hpp`/`.cpp`: `ProvisionedDataStore::seed()` gains a 9th parameter
  (`trace_data`); new `get_trace_data()` reuses the existing `get_provisioned_column()` helper,
  same as every other sibling accessor.
- `nfs/udr/src/main.cpp`: one new `GET` route at `.../provisioned-data/trace-data`, reusing the
  existing shared `provisioned_data_get_counter`. Seed data extended with `traceRef:
  "99970-A1B2C3"` (real, cited pattern from `TS 32.422` -- MCC+MNC concatenated with a real lab
  PLMN `99970`, plus an arbitrary 3-octet hex Trace ID, this project's own representative test
  value) and `traceDepth: "MEDIUM"` (a real enum value).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded (SUPI, PLMN) pair -> real `200` with `{"traceDepth":"MEDIUM","traceRef":"99970-A1B2C3"}`;
`GET` on the same SUPI with an unseeded PLMN -> real `404`; cross-checked the sibling `sms-data`
column on the same row is unaffected -> real `200` with the unchanged expected body. Direct `psql`
query against `udr_provisioned_data` independently confirmed both seeded rows' `sms_data` and
`trace_data` columns hold their own correct, distinct values.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (same disclosed
manual-live-verification precedent already established for every GET-only seeded resource in this
series), zero regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #40 of free5GC's ~42+ real `Nudr_DataRepository` resources; roughly 2 remain a real,
open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

