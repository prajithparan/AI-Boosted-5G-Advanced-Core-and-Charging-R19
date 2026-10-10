## ADR-0111: gap-closure task #106 continuation -- UDR real Operator-Specific Data Container (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (25 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0110). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/operator-specific-data` (real
response shape: a map keyed by operator-specific data element name, values real schema
`OperatorSpecificDataContainer` -- mandatory `dataType` (enum: string/integer/number/boolean/
object/array) + `value`, no top-level wrapper struct) has a real `GET`+`PATCH`-only operation set:
`QueryOperSpecData`, `ModifyOperSpecData` (real `application/json-patch+json`, RFC 6902, same
standard `PpDataStore`'s own patch already uses) -- confirmed by direct read, no PUT/DELETE exists
for this resource, and no POST/create operation exists either, so (same disclosed, deliberate
precedent already established for `PpDataStore`/`AuthenticationSubscriptionDataStore`)
`apply_patch` is upsert-capable.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_operator_specific_data` table (`ue_id` PK, `data`
  JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `OperatorSpecificDataStore` class (`get`/`apply_patch`),
  byte-for-byte matching `PpDataStore`'s own upsert-capable `apply_patch` pattern.
- `nfs/udr/src/main.cpp`: two new routes (`GET`/`PATCH`) at
  `/subscription-data/{ueId}/operator-specific-data` (no DTO needed -- the real response is a raw
  map, matching `PpDataStore`'s own established shape) and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PATCH` with a real RFC 6902 `add` operation adding a
`customFlag` entry (`{"dataType":"boolean","value":true}`, a spec-valid `OperatorSpecificDataContainer`
per the real mandatory-field/enum requirements, this project's own representative test key/value)
-> real `200` with the created document (originating via upsert, no prior `PUT`/`POST`); `GET`
immediately after -> real `200`; `PATCH` again with a real `replace` on `/customFlag/value` ->
real `200` with the updated document; `GET` again -> real `200` confirming the update. Direct
`psql` query against `udr_operator_specific_data` independently confirmed the persisted document
matches the API's final response.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #26 of free5GC's ~42+; roughly 16 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

