## ADR-0101: gap-closure task #106 continuation -- UDR real PEI Information (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (15 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0100). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/context-data/pei-info` (PEI
Information (Document) of the 5GC/EPC domains) has a real, simple `PUT`+`GET`-only operation set:
`CreateOrUpdatePeiInformation`, `QueryPeiInformation` -- confirmed by direct read, no PATCH/DELETE
exists for this resource in the spec, same shape as `RoamingInformationStore` (ADR-0100). Real
schema is an `allOf` composition -- `TS29505_Subscription_Data.yaml`'s own `PeiUpdateInfo` merges
`TS29503_Nudm_UECM.yaml`'s base `PeiUpdateInfo` (mandatory `pei`) with this file's own
`PeiUpdateInfoExt` (`lastPeiChangeTimestamp`/`lastImeiChangeTimestamp`/`previousPei`/
`previousPeiTimestamp`) -- confirmed already correctly flattened and disambiguated by sbi-codegen
into `sbi_gen::PeiUpdateInfo_Subscription_Data`, distinct from the base type's own
`PeiUpdateInfo_Nudm_UECM`, before writing any application code. `CreateOrUpdatePeiInformation`'s
`PUT` genuinely distinguishes `201 Created` from `204` (updated), same real distinction already
established for `AmfContextStore`/`AmfNon3GppContextStore`/`MessageWaitingDataStore`/
`RoamingInformationStore`.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_pei_info` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `PeiInfoStore` class (`put`/`get` only) -- `put()` reuses
  the same real `xmax = 0` UPSERT idiom already used to report the 201-vs-204 distinction in one
  statement.
- `nfs/udr/src/main.cpp`: two new routes (`PUT`/`GET`) at
  `/subscription-data/{ueId}/context-data/pei-info`, reusing the already-vendored
  `sbi_gen::PeiUpdateInfo_Subscription_Data` DTO (no new codegen work needed) and one new OTel
  write counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PUT` with `{"pei":"imei-490154203237518"}` on a new `ueId` ->
real `201 Created` with `Location` header and the created document in the body; `GET` immediately
after -> real `200` with the identical document; `PUT` again on the same `ueId` with a changed
`pei` plus `previousPei` -> real `204` (genuinely distinct from the `201` create path above); `GET`
again -> real `200` confirming the updated document, including the composed `PeiUpdateInfoExt`
field (`previousPei`) alongside the base `pei` field, proving the `allOf` flattening is real and
correct end-to-end. Direct `psql` query against `udr_pei_info` independently confirmed the
persisted document matches what the API returned.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #16 of free5GC's ~42+; roughly 26 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

