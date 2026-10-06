## ADR-0102: gap-closure task #106 continuation -- UDR real Enhanced Coverage Restriction Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (16 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0101). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/coverage-restriction-data`
(real schema `EnhancedCoverageRestrictionData`, `$ref`'d verbatim from `TS29503_Nudm_SDM.yaml` --
a `plmnEcInfoList` of `PlmnEcInfo` entries, each a mandatory `plmnId` plus optional
`ecRestrictionDataWb`/`ecRestrictionDataNb`) is a real, genuinely GET-only resource
(`QueryCoverageRestrictionData`) -- confirmed by grepping every operationId referencing this path
in the file, no create/update operation exists at all, same real shape already established for
`ProvisionedDataStore` (ADR-0069): there is no live provisioning path for this data in the real
spec, so it's seeded at startup rather than created via the API.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_coverage_restriction_data` table (`ue_id` PK, `data`
  JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `CoverageRestrictionDataStore` class (`seed`/`get` only,
  no `put`/`patch`/`remove` -- matching `ProvisionedDataStore`'s own real GET-only shape).
- `nfs/udr/src/main.cpp`: one new route (`GET`) at
  `/subscription-data/{ueId}/coverage-restriction-data`, reusing the already-vendored
  `sbi_gen::EnhancedCoverageRestrictionData` DTO indirectly (the store persists/returns raw
  `nlohmann::json`, matching `ProvisionedDataStore`'s own established pattern for this resource
  shape -- no new codegen work needed), and a real seed loop for the same two real test SUPIs
  every other GET-only UDR resource in this project already seeds
  (`imsi-999700000000001`/`...002`), populated with this project's own real lab PLMN
  (mcc=999/mnc=70, ADR-0016) and an explicit `ecRestrictionDataNb: false` test value, and one new
  OTel get counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded SUPI `imsi-999700000000001` -> real `200` with the exact seeded
`plmnEcInfoList`/`ecRestrictionDataNb` document; `GET` for an unseeded SUPI -> real `404`. Direct
`psql` query against `udr_coverage_restriction_data` independently confirmed both seeded rows
persisted correctly across the fresh startup.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions.

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) --
`ecRestrictionDataWb` (the `EcRestrictionDataWb` nested object) is left unpopulated in the seed
data, a real, disclosed gap rather than a guessed nested shape, same "OPAQUE FALLBACK" precedent
already disclosed for `provisioned-data`'s own seed. This closes UDR resource #17 of free5GC's
~42+; roughly 25 remain a real, open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106
remains open (not fully closed).

