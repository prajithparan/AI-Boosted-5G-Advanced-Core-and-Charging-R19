## ADR-0107: gap-closure task #106 continuation -- UDR real Parameter Provision (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (21 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0106). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/pp-data` (real schema `PpData`,
`$ref`'d verbatim from `TS29503_Nudm_PP.yaml` -- `communicationCharacteristics`,
`expectedUeBehaviourParameters`, `ecRestriction`, `stnSr`, `lcsPrivacy`, and others, every field
optional) has a real `GET`+`PATCH`-only operation set: `GetppData`, `ModifyPpData` (real
`application/json-patch+json`, RFC 6902, same standard `AuthenticationSubscriptionDataStore`'s own
patch already uses) -- confirmed by direct read, no PUT/DELETE exists for this resource, and no
POST/create operation exists either, so (same disclosed, deliberate precedent already established
for `AuthenticationSubscriptionDataStore`/`SmPolicyDataStore`) `apply_patch` is upsert-capable.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_pp_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `PpDataStore` class (`get`/`apply_patch`), byte-for-byte
  matching `AuthenticationSubscriptionDataStore`'s own upsert-capable `apply_patch` pattern.
- `nfs/udr/src/main.cpp`: two new routes (`GET`/`PATCH`) at `/subscription-data/{ueId}/pp-data`
  (no DTO needed -- store persists/returns raw `nlohmann::json`, matching this GET+PATCH
  resource-class's own established pattern) and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PATCH` with a real RFC 6902 `add` operation on `/stnSr`
(originating the document via upsert, no prior `PUT`/`POST`) -> real `200` with the created
document; `GET` immediately after -> real `200` confirming persistence; `PATCH` again with a real
`replace` on `/stnSr` -> real `200` with the updated document; `GET` again -> real `200`
confirming the update. Direct `psql` query against `udr_pp_data` independently confirmed the
persisted document matches the API's final response.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) --
`pp-data`'s own siblings (`pp-data-store`, `pp-profile-data`) remain open, deferred to their own
scoped turns. This closes UDR resource #22 of free5GC's ~42+; roughly 20 remain a real, open,
disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

