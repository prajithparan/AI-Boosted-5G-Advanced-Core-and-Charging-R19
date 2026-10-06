## ADR-0113: gap-closure task #106 continuation -- UDR real UE Policy Set (policy-data group)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (27 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0112). Real, confirmed-by-YAML-read:
`TS29519_Policy_Data.yaml`'s `/policy-data/ues/{ueId}/ue-policy-set` (real schema `UePolicySet`
-- optional `praInfos`, `subscCats`, `uePolicySections`, and others) has a real `GET`+`PUT`+
`PATCH` operation set: `ReadUEPolicySet`, `CreateOrReplaceUEPolicySet` (real distinct `201`-vs-
`204` response codes, same `xmax = 0` idiom already established), `UpdateUEPolicySet` (real
`application/merge-patch+json`, RFC 7396, same standard `AmPolicyDataStore`'s own merge-patch
already uses) -- no `DELETE` exists for this resource. Real, disclosed difference from
`AmPolicyDataStore`'s own `PATCH`: the real spec here documents **only** `204` as the success
response for `UpdateUEPolicySet` (no `200`-with-body option), confirmed by direct read -- unlike
`am-data`'s own `PATCH`, which the real spec permits either `204` or `200` for and this project
deliberately chose `200`-with-body for. This resource genuinely combines a real `PUT`
(create-or-replace, matching `AmfContextStore`'s own 201-vs-204 shape) with a real `PATCH`
(merge-patch, matching `AmPolicyDataStore`'s own shape) on the same resource -- the first
`policy-data` group resource in this project with both.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_ue_policy_set` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `UePolicySetStore` class (`put`/`get`/`merge_patch`) --
  `put()` reuses the established real `xmax = 0` UPSERT idiom; `merge_patch()` matches
  `AmPolicyDataStore::merge_patch()`'s own pattern exactly.
- `nfs/udr/src/main.cpp`: three new routes (`GET`/`PUT`/`PATCH`) at
  `/policy-data/ues/{ueId}/ue-policy-set` (no DTO needed -- store persists/returns raw
  `nlohmann::json`, matching this resource-class's own established pattern), with the `PATCH`
  route explicitly returning `204` with no body (not `200`-with-body), matching the real spec's
  own narrower response set here, and three new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PUT` with `{"subscCats":["cat1"]}` -> real `201 Created` with
`Location` header and the created document; `GET` immediately after -> real `200`; `PUT` again with
a changed `subscCats` -> real `204` (genuinely distinct from the `201` create path); `GET` again ->
real `200` confirming the update; `PATCH` (`application/merge-patch+json`) with `{"subscCats":
["cat3"]}` -> real `204` with **no body**, confirmed correct per the real spec's narrower response
set for this resource; `GET` again -> real `200` confirming the merge-patched value took effect.
Direct `psql` query against `udr_ue_policy_set` independently confirmed the persisted document
matches the API's final response.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure) --
`praInfos`/`uePolicySections` are left unpopulated in the seed/test data, a real, disclosed gap
rather than a guessed nested shape; `ue-policy-set`'s own real PLMN-keyed sibling
(`/policy-data/plmns/{plmnId}/ue-policy-set`) remains open, deferred to its own scoped turn. This
closes UDR resource #28 of free5GC's ~42+; roughly 14 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

