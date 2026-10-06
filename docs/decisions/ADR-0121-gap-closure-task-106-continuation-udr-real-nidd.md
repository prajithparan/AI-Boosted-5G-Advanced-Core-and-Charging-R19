## ADR-0121: gap-closure task #106 continuation -- UDR real NIDD Authorization Info context-data (self-correction of an earlier deferral)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (34 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0119, plus one real distinct-API resource from
ADR-0120). While looking for the next real candidate, individually re-checked each item this
project's own file header had lumped together as "context-data's other sub-resources
(ee-subscriptions, sdm-subscriptions, nidd-authorizations)" and deferred as a bundle without a
real per-resource YAML read at the time. That bundling was wrong for at least one of the three:
`/subscription-data/{ueId}/context-data/nidd-authorizations` (real schema
`NiddAuthorizationInfo` -- required `niddAuthorizationList`, array of the real `AuthorizationInfo`
schema, `snssai`/`dnn`/`mtcProviderInformation`/`authUpdateCallbackUri` required,
`afId`/`nefId`/`validityTime`/`contextInfo` optional) is genuinely a **flat per-UE document**, not
a nested sub-subscription resource -- confirmed by direct read of
`CreateNIDDAuthorizationInfo`/`GetNiddAuthorizationInfo`/`ModifyNiddAuthorizationInfo`/
`RemoveNiddAuthorizationInfo`: real `PUT`+`GET`+`PATCH`+`DELETE`, same shape as
`amf-3gpp-access`'s own real context-data resource (real distinct `201`-vs-`204` PUT response
codes, real RFC 6902 `application/json-patch+json` PATCH), plus a real `DELETE` (which
`amf-3gpp-access`'s own resource lacks). `ee-subscriptions`/`sdm-subscriptions` were NOT
re-verified this pass and remain genuinely deferred -- this ADR corrects the record for
`nidd-authorizations` specifically, not the whole bundle.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_nidd_authorization_info` table (`ue_id` PK, `data`
  JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `NiddAuthorizationInfoStore` class
  (`put`/`get`/`apply_patch`/`remove`), byte-for-byte matching `AmfContextStore`'s own real
  distinct-201-vs-204 PUT + RFC 6902 `apply_patch` pattern, with `remove()` added (the one real
  operation `AmfContextStore`'s own resource doesn't have).
- `nfs/udr/src/main.cpp`: four new routes (`PUT`/`GET`/`PATCH`/`DELETE`) at
  `/subscription-data/{ueId}/context-data/nidd-authorizations`, using the real generated
  `sbi_gen::NiddAuthorizationInfo` DTO (sbi-codegen groups this schema into
  `TS29122_CommonData_grp.hpp` alongside its own `AuthorizationInfo` dependency -- a real codegen
  grouping detail, not a hand-written type), and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database, for
`imsi-999700000000001`: `GET` on the unseeded key -> real `404`; `PATCH` on that same unseeded
key -> real `404`; `PUT` with a real spec-valid `AuthorizationInfo` body (`snssai`,
`dnn: "internet"`, `mtcProviderInformation: "mtc1"`, `authUpdateCallbackUri`) -> real `201` with
the created document; `GET` immediately after -> real `200` matching; `PUT` again with a
different `authUpdateCallbackUri` -> real `204` (not `201`, confirming the real distinct-status
UPSERT behavior); `PATCH` (`application/json-patch+json`) with a real `replace` op on `/dnn` ->
real `204`; `GET` again -> real `200` confirming the patched `dnn`; `DELETE` -> real `204`; `GET`
again -> real `404`. Direct `psql` query against `udr_nidd_authorization_info` independently
confirmed zero rows remained after the delete.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure).
`ee-subscriptions`/`sdm-subscriptions` remain genuinely deferred, not individually re-verified
this pass -- explicitly NOT claimed closed by this ADR. This closes UDR resource #35 of free5GC's
~42+ real `Nudr_DataRepository` resources; roughly 7 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

