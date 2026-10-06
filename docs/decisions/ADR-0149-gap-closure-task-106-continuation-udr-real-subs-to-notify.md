## ADR-0149: gap-closure task #106 continuation -- UDR real Subs To Notify collection + individual
document resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (59 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0148). `subs-to-notify` had been explicitly,
individually surveyed and deferred earlier this session (recorded in `nfs/udr/src/main.cpp`'s own
header comment): "real POST-based collection with a server-generated Location header and real
webhook callback registration (`{$request.body#/notificationUri}`), no existing project precedent
for either." `ee-subscriptions` (ADR-0148, this same session) established the first real
server-generated-ID precedent, resolving half of that blocker. This ADR closes the resource on
that basis while being explicit that the second half -- outbound webhook delivery -- is
deliberately still not built.

Real, confirmed-by-YAML-read: `/subscription-data/subs-to-notify` (real spec operations
`SubscriptionDataSubscriptions` [POST] / `QuerySubsToNotify` [GET]) is a real collection, global
(NOT scoped under `{ueId}` in its own path, unlike `ee-subscriptions`). `QuerySubsToNotify`'s own
`ue-id` query parameter is real and `required: true`, but -- unlike every genuinely-blocked
array-query-param this project has found -- it's a plain string (`VarUeId`), not an array, so it's
directly usable. `/subscription-data/subs-to-notify/{subsId}` (real spec operations
`QuerySubscriptionDataSubscriptions`/`ModifysubscriptionDataSubscription`/
`RemovesubscriptionDataSubscriptions`) has real GET+PATCH+DELETE -- genuinely no PUT exists for
this resource at all (confirmed by reading past the DELETE/PATCH/GET blocks in sequence, no PUT
operation follows). Schema `SubscriptionDataSubscriptions` -- required `callbackReference` (Uri) +
`monitoredResourceUris` (array of Uri), plus a real dozen-plus optional fields including its own
optional `ueId`.

Real design decision, disclosed: since the collection isn't path-scoped under `{ueId}`, and the
real `ue-id` GET filter must match against *something*, this project stores the POST body's own
optional `ueId` field (empty string if the caller omits it) as a real, indexed column and filters
on it -- the natural, spec-consistent interpretation, not an invented field.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_subs_to_notify` table (`subs_id` PK, `ue_id` column,
  `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SubsToNotifyStore` class (`create`/`get`/`list_by_ue_id`/
  `apply_patch`/`remove`) -- structurally similar to `EeSubscriptionsStore` but with no
  `update()`, since the real spec has no PUT for this resource.
- `nfs/udr/src/main.cpp`: store construction, five new OTel counters (create, list, get, write
  [PATCH], delete), and five new routes: `GET`/`POST` on the collection path (`GET` returns a real
  `400` if the required `ue-id` query parameter is missing), `GET`/`PATCH`/`DELETE` on the
  individual path. `subsId` generated the same way as `ee-subscriptions`
  (`sbi_core::generate_uuid_v4()`); `Location` header follows the same established
  append-to-collection-path convention.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL, SUPI `imsi-999700000000013`:

- `GET` the collection with no `ue-id` query param -> real `400`.
- `GET` the collection with `ue-id=imsi-999700000000013` before any `POST` -> real `200` with an
  empty array.
- `POST` with `{"ueId":"imsi-999700000000013","callbackReference":"https://af.example.com/notify",
  "monitoredResourceUris":["https://udr.example.com/subscription-data/.../am-data"]}` -> real
  `201`, a real, freshly-generated UUID v4 `subsId` in `Location`, body echoed back.
- `GET` the collection with the same `ue-id` -> real `200` with a one-element array.
- `GET` the collection with a *different* `ue-id` -> real `200` with an empty array, confirming
  the filter is genuinely enforced, not a no-op.
- `GET` the individual resource by `subsId` -> real `200` with the same body.
- `PATCH` with a real RFC 6902 `[{"op":"replace","path":"/callbackReference","value":"..."}]` ->
  real `204`; `GET` after -> real `200` reflecting the patched value.
- `PATCH` against a nonexistent `subsId` -> real `404` (confirms `apply_patch` is NOT
  upsert-capable).
- Direct `psql SELECT` against `udr_subs_to_notify` independently confirmed the single row
  (`subs_id`, `ue_id`, `data`) matched curl's response exactly at this point.
- `DELETE` -> real `204`; individual `GET` after -> real `404`; collection `GET` after -> real
  `200` with an empty array again; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Real,
disclosed, deliberately NOT built: the real `onDataChange` webhook callback
(`{$request.body#/notificationUri}` in the spec, real `DataChangeNotify` POST to the caller's
`callbackReference` when underlying subscribed data changes) -- this project stores and answers
CRUD on subscriptions, but has no outbound-webhook-delivery mechanism yet, and nothing in this
project currently detects "underlying data changed" as an event to notify on. This closes UDR
resource #60 of free5GC's ~42+ real `Nudr_DataRepository` resources. Task #106 remains open:
`group-data`'s remaining genuinely-blocked resources, bare
`/subscription-data/{ueId}`/`{ueId}/context-data`, `ee-subscriptions`'s own nested
sub-collections, `sdm-subscriptions`, and the other genuinely deferred subsystems (`pdtq-data`,
`mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

