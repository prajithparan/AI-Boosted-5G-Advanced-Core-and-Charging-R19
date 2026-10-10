## ADR-0170: gap-closure task #106 continuation -- UDR real Nudr_GroupIDmap subscription-management family (nf-group-ids/subscriptions)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure. With `5g-vn-groups`/
`mbs-group-membership`'s entire real resource set closed (ADR-0169), and `GetSSAuData`/
`mbs-session-pol-data` both remaining deliberately deferred on their own already-investigated,
genuine ambiguities (ADR-0160's structural-mismatch finding; a real deeply-nested `oneOf`/`anyOf`
key encoding with no documented flat-string form), the next real, scoped candidate is
`Nudr_GroupIDmap`'s own `/nf-group-ids/subscriptions` family -- surveyed at a high level in
ADR-0164 (which deferred it as "genuinely more complex," but did not investigate its real shape in
detail) and now read in full.

Real, confirmed by direct read (`TS29504_Nudr_GroupIDmap.yaml` lines 86-312):
`CreateGroupIdSubscription` (`POST /nf-group-ids/subscriptions`), `QueryGroupIdSubscription`
(`GET .../{subscriptionId}`), `ModifyGroupIdSubscription` (`PATCH .../{subscriptionId}`, real RFC
6902 `application/json-patch+json`), `RemoveGroupIdSubscription` (`DELETE .../{subscriptionId}`,
real `204`). This is, real and disclosed, structurally the **same shape** as every other real
subscription-management resource this project has already closed (`ee-subscriptions`,
`subs-to-notify`): a real CRUD collection+individual pair with a server-generated ID and a real
spec-documented webhook callback (`onGroupIdMapChange`, delivering `GroupIdMapNotify` to the
caller's own `notificationUri`) that this project does not implement -- not a genuinely new or
more complex shape than what's already been closed repeatedly, contrary to ADR-0164's own
earlier, less-detailed characterization. `SubscriptionData`'s own real required fields:
`notificationUri`, `nfType`, `nfGroupId`; `subscriptionId` is `readOnly` (server-generated);
`expiry` optional. No generated DTO exists for this API (same as every other `Nudr_GroupIDmap`
resource in this project) -- validated here by a direct required-field check on the raw parsed
body, same precedent as `routing_ids`/`nf_group_ids`.

Real, disclosed: `ModifyGroupIdSubscription`'s own response list documents both `200` (with the
updated `SubscriptionData` body) and `204` (empty) for a successful modify, with no further
distinguishing rule in the spec text -- the exact same real ambiguity this project already
resolved for `group_control_data`'s own RFC 7396 PATCH (ADR-0118/ADR-0119): always respond `200`
with the patched body, the richer of the two documented options. Applied identically here despite
the different patch format (RFC 6902 vs RFC 7396) -- the ambiguity is about the response code, not
the patch mechanism.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_nf_group_id_subscriptions` table
  (`subscription_id TEXT PRIMARY KEY`, `data JSONB`).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `NfGroupIdSubscriptionStore` class
  (`create`/`get`/`apply_patch`/`remove`), structurally identical to `SubsToNotifyStore`.
- `nfs/udr/src/main.cpp`: store construction, four new OTel counters, four new routes
  (`POST`/`GET`/`PATCH`/`DELETE`) under `kGroupIdMapApiRoot`. `subscriptionId` generated via
  `sbi_core::generate_uuid_v4()` (the same helper every other subscription resource in this
  project already uses), written into the stored/returned document.
- Real, disclosed gap, stated plainly rather than left implicit: the spec's own
  `onGroupIdMapChange` webhook callback (real outbound `POST` of `GroupIdMapNotify` to the
  caller's `notificationUri` on group-mapping changes) is **not implemented** -- creating,
  modifying, or deleting a subscription here has no observable effect on any consumer; the
  subscription record is stored and CRUD-manageable, but nothing is ever notified. Same disclosed
  gap class as `subs-to-notify`'s own lack of real webhook delivery (ADR-0149) -- this project has
  no real outbound webhook-delivery infrastructure anywhere yet, a real, larger, cross-cutting gap
  that spans multiple resources, not specific to this one.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process (freshly built, freshly started, registered with a
freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token):

- `POST` with `nfType` only (missing `notificationUri`/`nfGroupId`) -> real `400`.
- `POST` with a real, complete `SubscriptionData` body -> real `201`, body echoed back with a
  real server-generated `subscriptionId`, `Location` header containing the real path + ID.
- `GET` the individual resource -> real `200` matching the created body exactly; `GET` a
  nonexistent `subscriptionId` -> real `404`.
- `PATCH` with a real RFC 6902 `replace` op on `/notificationUri` -> real `200` with the updated
  body (confirming the `group_control_data`-precedent resolution of the real `200`-vs-`204`
  ambiguity); `GET` after confirms persistence; `PATCH` a nonexistent `subscriptionId` -> real
  `404`.
- `DELETE` -> real `204`; `GET` after -> real `404`.
- `GET /nf-group-ids` (ADR-0164's own sibling resource, same API root, different, shorter path)
  immediately after -> still real `200` with its own correct seeded data, confirming no
  interference between the two real `Nudr_GroupIDmap` resource families sharing this API root.
- Direct `psql SELECT` against `udr_nf_group_id_subscriptions` independently confirmed zero rows
  remained after the delete, matching the final `404`.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions -- same count as ADR-0169, since this ADR adds no new
automated test (same disclosed manual-live-verification precedent already established for
subscription-management CRUD resources).

### What this ADR does NOT include

No real webhook delivery (`onGroupIdMapChange`) -- disclosed above as a real, larger, cross-cutting
gap shared with `subs-to-notify`, not attempted here. No NF's own existing logic calls these new
routes or would ever receive a real notification through them. This is a real, distinct
`Nudr_GroupIDmap` resource family, same non-counting precedent as `GetRoutingIDs`/`GetNfGroupIDs`
(ADR-0120/ADR-0164) -- does **NOT** increment the "N of free5GC's ~42+ real `Nudr_DataRepository`
resources" metric, still 78. Task #106's real, disclosed
remaining gaps: `policy-data`'s `mbs-session-pol-data` (deferred, key-encoding ambiguity);
`GetSSAuData` (deliberately deferred, ADR-0160); real webhook delivery infrastructure (spans this
resource and `subs-to-notify`, a real, separate, larger piece of work); `ext-group-ids`/`gpsis`/
`supported-features` filtering retrofits across several already-closed resources.

