## ADR-0123: gap-closure task #106 continuation -- UDR real ODB Data (Query by SUPI or GPSI)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (36 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0122). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/operator-determined-barring-data`
(real schema `OdbData`, `TS29571_CommonData.yaml` -- a single optional field, `roamingOdb`, real
`RoamingOdb` enum) is genuinely `GET`-only (`GetOdbData`) -- no create/update operation exists at
all, confirmed by direct read of the block between this path and the next
(`/subscription-data/{ueId}/context-data`). Same real "provisioned out-of-band, seeded at startup"
shape already established for `CoverageRestrictionDataStore` and every other real GET-only
resource closed this series.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_odb_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `OdbDataStore` class (`seed`/`get`), same shape as
  `CoverageRestrictionDataStore`.
- `nfs/udr/src/main.cpp`: one new `GET` route at
  `/subscription-data/{ueId}/operator-determined-barring-data` and one new OTel counter. Seeded
  for the same two real test SUPIs every other GET-only UDR resource seeds, with
  `roamingOdb: "OUTSIDE_HOME_PLMN"` (a real enum value, this project's own representative test
  choice).

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` on the
seeded SUPI (`imsi-999700000000001`) -> real `200` with `{"roamingOdb":"OUTSIDE_HOME_PLMN"}`;
`GET` on an unseeded SUPI (`imsi-999700000000099`) -> real `404`. Direct `psql` query against
`udr_odb_data` independently confirmed both seeded rows match.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (same disclosed
manual-live-verification precedent already established for every GET-only seeded resource in this
series), zero regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #37 of free5GC's ~42+ real `Nudr_DataRepository` resources; roughly 5 remain a real,
open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

