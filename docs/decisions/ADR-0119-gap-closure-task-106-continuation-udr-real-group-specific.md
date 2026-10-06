## ADR-0119: gap-closure task #106 continuation -- UDR real group-specific Policy Control Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (33 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0118). Real, confirmed-by-YAML-read:
`TS29519_Policy_Data.yaml`'s `/policy-data/group-control-data/{intGroupId}` (real schema
`GroupPolicyData` -- `maxGroupMbrUl`/`maxGroupMbrDl`/`remainGroupMbrUl`/`remainGroupMbrDl`/
`suppFeat`, every field optional) has a real `GET`+`PATCH`-only operation set:
`ReadGroupPolCtrlData`, `ModifyGroupPolCtrlData` (real `application/merge-patch+json`, RFC 7396,
request body is the narrower `GroupPolicyDataPatch` schema). Confirmed by direct read: no
`PUT`/`POST` create operation exists for this resource at all, so (same disclosed, deliberate
precedent already established for `AmPolicyDataStore`/`SlicePolicyDataStore` in ADR-0118)
`merge_patch` is upsert-capable. Keyed by `intGroupId` (real `GroupId` schema,
`TS29571_CommonData.yaml` -- a plain string with a real pattern cited from TS 23.003 clause 19.9),
genuinely no path-segment encoding ambiguity, unlike `slice-control-data`'s own `snssai` key
(ADR-0118).

While checking this resource's real siblings for the next candidate, also confirmed and
disclosed: `mbs-session-pol-data`'s key (`MbsSessPolDataId`) is a genuinely deeper problem than
`snssai` was -- a `oneOf` of `{mbsSessionId: MbsSessionId}` (itself an `anyOf` of `tmgi`/`ssm`,
each further-nested objects) or `{afAppId: string}`, with no documented bare-path-segment string
encoding and no existing project precedent to reuse (unlike `snssai`'s own flat two-field shape,
which had ADR-0072's `sst + '-' + sd` convention already established). Implementing it now would
mean inventing a serialization for a multi-level nested object, which this project's own rules
treat as fabrication, not a disclosed convention choice -- left deferred instead.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_group_control_data` table (`int_group_id` PK, `data`
  JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `GroupPolicyDataStore` class (`get`/`merge_patch`),
  byte-for-byte matching `SlicePolicyDataStore`'s own upsert-capable `merge_patch` pattern.
- `nfs/udr/src/main.cpp`: two new routes (`GET`/`PATCH`) at
  `/policy-data/group-control-data/{intGroupId}` and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `intGroupId` (`00112233-100-01-AABBCCDDEE`) -> real `404`; `PATCH`
(`application/merge-patch+json`) with `{"maxGroupMbrUl":"500 Mbps"}` on that same key, no prior
create -> real `200` with the newly-originated document (confirming upsert-capable `PATCH`); `GET`
immediately after -> real `200` matching; `PATCH` again with `{"remainGroupMbrDl":"50 Mbps"}` ->
real `200` with both fields present (`maxGroupMbrUl` retained, `remainGroupMbrDl` merged in);
`GET` again -> real `200` confirming the merge. Direct `psql` query against
`udr_group_control_data` independently confirmed the persisted document
(`{"maxGroupMbrUl": "500 Mbps", "remainGroupMbrDl": "50 Mbps"}`) matches the API's final response
exactly.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #34 of free5GC's ~42+; roughly 8 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md), including `mbs-session-pol-data`'s own newly-disclosed
key-encoding gap (see Context above). Task #106 remains open (not fully closed).

