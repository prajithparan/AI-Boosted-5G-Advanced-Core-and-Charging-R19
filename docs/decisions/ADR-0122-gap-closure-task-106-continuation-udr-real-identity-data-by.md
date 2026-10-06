## ADR-0122: gap-closure task #106 continuation -- UDR real Identity Data by SUPI or GPSI (plus individual re-verification of the ee-/sdm-subscriptions and subs-to-notify deferrals)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (35 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0121). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/identity-data` (real schema
`IdentityData` -- `supiList`/`gpsiList`/`allowedAfIds`, all optional arrays) has a real
`GET`+`PATCH` operation set: `GetIdentityData`, `ModifyIdentityData` (real
`application/json-patch+json`, RFC 6902 -- NOT RFC 7396 merge-patch like `slice-control-data`/
`group-control-data`'s own PATCH standard). Confirmed by direct read: no `PUT`/`POST` create
operation exists for this resource at all, so (same disclosed, deliberate precedent already
established for `PpDataStore`/`OperatorSpecificDataStore`) `apply_patch` is upsert-capable. Real,
disclosed simplification: the real spec's optional `app-port-id` query param (a real, non-body
complex parameter, `content: application/json`) and the real conditional-request headers
(`If-None-Match`/`If-Modified-Since` on the request, `Cache-Control`/`ETag`/`Last-Modified` on the
response) are not implemented -- same "no conditional-GET semantics anywhere in this project yet"
gap already true of every other GET route in this project, not specific to this resource.

Also individually re-verified, per last ADR's own commitment to check rather than re-bundle: real,
confirmed by direct YAML read, `ee-subscriptions`/`sdm-subscriptions` genuinely ARE deeply nested
subscription-lifecycle resources (collection `POST` -> individual `{subsId}` -> further nested
`amf-subscriptions`/`smf-subscriptions`/`hss-subscriptions` sub-collections underneath each
subscription, plus a parallel `group-data`-scoped tree at
`/subscription-data/group-data/{ueGroupId}/ee-subscriptions/...`) -- the original deferral for
these two was correct, unlike `nidd-authorizations` (ADR-0121). Also re-checked
`/policy-data/subs-to-notify`: confirmed genuinely a real `POST`-based collection resource with a
real server-generated `Location` header (no client-supplied resource ID) and a real webhook
callback registration (`callbacks: policyDataChangeNotification:
'{$request.body#/notificationUri}'`) -- no existing precedent in this project for either
server-generated resource IDs via `POST` or webhook callback dispatch, so this remains a genuine,
disclosed gap, not a quick single-resource turn.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_identity_data` table (`ue_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `IdentityDataStore` class (`get`/`apply_patch`),
  byte-for-byte matching `PpDataStore`'s own upsert-capable `apply_patch` pattern.
- `nfs/udr/src/main.cpp`: two new routes (`GET`/`PATCH`) at
  `/subscription-data/{ueId}/identity-data` and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database, for
`imsi-999700000000001`: `GET` on the unseeded key -> real `404`; `PATCH`
(`application/json-patch+json`) with a real `add` operation on `/gpsiList` (originating the
document via upsert, no prior `PUT`/`POST`) -> real `200` with the created document; `GET`
immediately after -> real `200` matching; `PATCH` again with a real `add` operation on `/supiList`
-> real `200` with both fields present; `GET` again -> real `200` confirming both fields persist.
Direct `psql` query against `udr_identity_data` independently confirmed the persisted document
matches the API's final response exactly.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Does NOT
implement `ee-subscriptions`/`sdm-subscriptions`/`subs-to-notify` -- all three individually
re-checked and confirmed as real, disclosed, still-open gaps this pass (see Context above), not
silently re-deferred. This closes UDR resource #36 of free5GC's ~42+ real `Nudr_DataRepository`
resources; roughly 6 remain a real, open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md). Task
#106 remains open (not fully closed).

