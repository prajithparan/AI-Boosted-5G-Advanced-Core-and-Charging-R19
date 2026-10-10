## ADR-0100: gap-closure task #106 continuation -- UDR real Roaming Information (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (14 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0099). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/context-data/roaming-information`
(Roaming Information (Document) of the EPC domain, real schema `RoamingInfoUpdate` --
`TS29503_Nudm_UECM.yaml`, an optional `roaming` bool plus a mandatory `servingPlmn`
(`PlmnId`) and an optional `contextInfo`) has a real, simple `PUT`+`GET`-only operation set:
`UpdateRoamingInformation`, `QueryRoamingInformation` -- confirmed by direct read, no PATCH/DELETE
exists for this resource in the spec, same shape as the already-closed `AmfNon3GppContextStore`
(ADR-0093). Real, confirmed: `UpdateRoamingInformation`'s `PUT` genuinely distinguishes
`201 Created` from `204` (updated), same real distinction `AmfContextStore`/
`AmfNon3GppContextStore`/`MessageWaitingDataStore` already established -- not `IpSmGwContextStore`'s
always-`204` shape.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_roaming_information` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `RoamingInformationStore` class (`put`/`get` only) --
  `put()` reuses the same real `xmax = 0` UPSERT idiom `AmfContextStore`/`AmfNon3GppContextStore`/
  `MessageWaitingDataStore` already use to report the 201-vs-204 distinction in one statement.
- `nfs/udr/src/main.cpp`: two new routes (`PUT`/`GET`) at
  `/subscription-data/{ueId}/context-data/roaming-information`, reusing the already-vendored
  `sbi_gen::RoamingInfoUpdate` DTO (confirmed present in the same generated group file as
  `IpSmGwRegistration`/`MessageWaitingData` before writing any application code -- no new codegen
  work needed) and one new OTel write counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PUT` with `{"roaming":true,"servingPlmn":{"mcc":"001","mnc":"01"}}`
on a new `ueId` -> real `201 Created` with `Location` header and the created document in the body;
`GET` immediately after -> real `200` with the identical document; `PUT` again on the same `ueId`
with a changed `roaming`/`servingPlmn` -> real `204` (genuinely distinct from the `201` create path
above); `GET` again -> real `200` confirming the updated document. Direct `psql` query against
`udr_roaming_information` independently confirmed the persisted document matches what the API
returned.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #15 of free5GC's ~42+; roughly 27 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

