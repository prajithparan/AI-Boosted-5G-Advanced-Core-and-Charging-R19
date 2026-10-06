## ADR-0105: gap-closure task #106 continuation -- UDR real LCS Mobile Originated Subscription Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (19 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0104). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/lcs-mo-data` (real schema
`LcsMoData`, `$ref`'d verbatim from `TS29503_Nudm_SDM.yaml` -- mandatory `allowedServiceClasses`
(array, minItems 1) + optional `moAssistanceDataTypes`) is a real, genuinely GET-only resource
(`QueryLcsMoData`) -- confirmed by grepping every operationId referencing this path, no
create/update operation exists at all, same real "provisioned out-of-band, seeded at startup"
shape already established for `LcsSubscriptionDataStore` (ADR-0104) and its own siblings.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_lcs_mo_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `LcsMoDataStore` class (`seed`/`get` only, matching
  `LcsSubscriptionDataStore`'s own real GET-only shape).
- `nfs/udr/src/main.cpp`: one new route (`GET`) at `/subscription-data/{ueId}/lcs-mo-data`, and a
  real seed loop for the same two real test SUPIs every other GET-only UDR resource in this
  project already seeds, populated with `{"allowedServiceClasses":["BASIC_SELF_LOCATION"]}` --
  `BASIC_SELF_LOCATION` is a real enum value from `LcsMoServiceClass`
  (`TS29503_Nudm_SDM.yaml`), this project's own representative test choice for the mandatory
  field -- and one new OTel get counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded SUPI `imsi-999700000000001` -> real `200` with the exact seeded `allowedServiceClasses`
document; `GET` for an unseeded SUPI -> real `404`. Direct `psql` query against `udr_lcs_mo_data`
independently confirmed both seeded rows persisted correctly across the fresh startup.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) --
`moAssistanceDataTypes` is left unpopulated in the seed data, a real, disclosed gap rather than a
guessed nested shape, same precedent as this resource's own siblings. This closes UDR resource #20
of free5GC's ~42+; roughly 22 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

