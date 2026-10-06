## ADR-0109: gap-closure task #106 continuation -- UDR real Provisioned Parameter Data Entry (pp-data-store)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (23 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0108, including `pp-data-store` flagged as deferred). Real,
confirmed-by-YAML-read: `TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/pp-data-store/{afInstanceId}` (real schema `PpDataEntry` --
`TS29503_Nudm_PP.yaml`, every field optional) has a real `PUT`+`GET`+`DELETE` operation set
(`Create PP Data Entry`/`Get PP Data Entry`/`Delete PP Data Entry`), plus a real sibling
collection resource `/subscription-data/{ueId}/pp-data-store` (`Get Multiple PP Data Entries`,
real schema `PpDataEntryList`) -- confirmed by direct read, richer than the other `pp-*` siblings
already closed (ADR-0107/0108), matching `SmfRegistrationStore`'s own real composite
`(ueId, pduSessionId)`-style key shape instead -- here `(ueId, afInstanceId)`.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_pp_data_entry` table (`ue_id`, `af_instance_id`, `data`
  JSONB, composite PK).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `PpDataEntryStore` class (`put`/`get`/`remove`/
  `list_for_ue`), `put()` reusing the same real `xmax = 0` UPSERT idiom already established for
  201-vs-204 distinction, `list_for_ue()` matching `SmfRegistrationStore::list_for_ue()`'s own
  pattern exactly.
- `nfs/udr/src/main.cpp`: four new routes (`GET` list / `GET` single / `PUT` / `DELETE`), using
  the already-vendored `sbi_gen::PpDataEntry`/`sbi_gen::PpDataEntryList` DTOs (no new codegen work
  needed) and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET`
single on an unseeded `(ueId, afInstanceId)` -> real `404`; `GET` list before any entries -> real
`200` with an empty `ppDataEntryList`; `PUT` with `{"referenceId":42}` -> real `201 Created` with
`Location` header and the created document; `GET` single immediately after -> real `200`; `GET`
list -> real `200` with the one real entry present; `PUT` again with a changed `referenceId` ->
real `204` (genuinely distinct from the `201` create path); a separate `(ueId, afInstanceId)` pair
was additionally used to directly confirm the `PUT`-update path with an intervening `GET`
(`referenceId` genuinely changed `1` -> `2`, not just a `204` status without effect); `DELETE` ->
real `204`; `GET` single again -> real `404`. Direct `psql` query against `udr_pp_data_entry`
confirmed zero rows remained for the deleted key.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #24 of free5GC's ~42+; roughly 18 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

