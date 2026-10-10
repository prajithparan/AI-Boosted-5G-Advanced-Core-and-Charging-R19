## ADR-0176: `udr_subs_to_notify` schema fix (nullable `ue_id` + `list_ue_less()`) and first two non-per-UE resources wired

### Context

A comprehensive sweep of every remaining `PUT`/`PATCH`/`DELETE` route in `nfs/udr/src/main.cpp`
(not a partial scan -- every route's path pattern was checked against its own key) confirmed that
**every genuinely per-UE `Nudr_DataRepository` write route now has real `onDataChange` delivery**
(61 real call sites, ADR-0171 through ADR-0175). Everything still unwired is structurally out of
scope for the existing `notify_subscribers()`: `bdt-data`/`pdtq-data`/`slice-control-data`/
`group-control-data` are keyed by `bdtReferenceId`/`pdtqReferenceId`/`snssai`/`intGroupId`, not
`ueId`; `5g-vn-groups`/`mbs-group-membership`/`group-data/{ueGroupId}/ee-subscriptions` (+ its 3
nested resources) are keyed by `externalGroupId`/`ueGroupId`; `nf-group-ids/subscriptions` belongs
to `Nudr_GroupIDmap`'s own separate `onGroupIdMapChange` callback, a different real API;
`subs-to-notify` itself is the subscription-management resource, not UE data; and the 5 remaining
`POST` routes are all collection-creates on subscription resources, already deliberately excluded
(no subscriber can watch a not-yet-created `subsId`). Presented to the user as a genuine decision
point (task #106's own webhook-delivery scope was complete as chosen); user selected fixing the
`udr_subs_to_notify` schema to unblock the non-per-UE resources, the real, previously-disclosed
prerequisite (first named in ADR-0171).

### Implementation

**Schema fix**: `udr_subs_to_notify.ue_id` changed from `TEXT NOT NULL` (backed by an
empty-string sentinel for UE-less subscriptions, per ADR-0149's own original disclosed
compromise) to a real nullable `TEXT` column. `SubscriptionDataSubscriptions.ueId` is genuinely
OPTIONAL per its own real schema (confirmed by direct read) -- a caller-omitted `ueId` is now
stored as real SQL `NULL`, not the string `""`. `SubsToNotifyStore::create()`'s own signature
changed to take `const std::optional<std::string>& ue_id` (pqxx binds `std::nullopt` to real SQL
`NULL`, same precedent already used elsewhere in this file, e.g. `ProvisionedDataStore::create()`
's own optional JSONB columns). Added `SubsToNotifyStore::list_ue_less()`
(`SELECT ... WHERE ue_id IS NULL`), the real query backing non-per-UE `onDataChange` matching.

**`main.cpp`**: the POST create route's own `const std::string ue_id = body->ueId.value_or("");`
line replaced with passing `body->ueId` (`std::optional<std::string>`) straight through. The
existing `notify_subscribers()` delivery loop (matching + POST logic) refactored into a shared
`deliver_onDataChange()` helper taking `const std::optional<std::string>& ue_id` -- present only
for the per-UE case, since the real `DataChangeNotify.ueId` field is itself genuinely OPTIONAL
(confirmed by direct read of the generated struct), so the ue-less path omits it entirely rather
than sending an empty string. New `notify_subscribers_ue_less(subs_to_notify, notify_client,
resolved_path, changes)` calls `deliver_onDataChange()` against `list_ue_less()`'s own results.
Existing `notify_subscribers()` call sites are unchanged (same signature, same behavior).

**First two non-per-UE resources wired** using the new function: `bdt-data`
(`PUT`+RFC 7396 `PATCH`+`DELETE`) and `pdtq-data` (`PUT`+RFC 7396 `PATCH`+`DELETE`) -- both
already-closed resources (ADR-0116, ADR-0162) reused unmodified aside from the new
`notify_subscribers_ue_less()` calls after each mutation.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real curl against a running `udr` process (with NRF for OAuth2 token issuance) and the same real
HTTPS receiver process from ADR-0171 onward: a real `POST subs-to-notify` subscription with **no
`ueId` field in the body at all** (not an empty string -- genuinely absent), `monitoredResourceUris`
watching the full resolved `bdt-data` path, followed by a real `PUT bdt-data` -> real `201`, and
the receiver independently logged a correctly-shaped `DataChangeNotify` with **no `"ueId"` key
present in the delivered JSON at all** (schema-conformant, not merely absent by omission bug).
Cross-checked directly against the real PostgreSQL row via `psql`: `SELECT subs_id, ue_id IS NULL
... FROM udr_subs_to_notify` confirmed the new subscription's `ue_id` is real SQL `NULL`, distinct
from 7 pre-existing rows from earlier ADR-0171-175 test sessions that still carry the old
empty-string sentinel (harmless legacy test data, not a migration concern -- `schema.postgres.sql`
is applied to a fresh database in CI and in any clean lab bring-up).

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written, across `main.cpp`/`stores.cpp`/`stores.hpp`). Full
`conformance_tests` (excluding the two disclosed pre-existing flaky tests): 331/331 pass, zero
regressions.

### What this ADR does NOT include

`slice-control-data`, `group-control-data`, `5g-vn-groups`, `mbs-group-membership`, and the
`group-data/{ueGroupId}/ee-subscriptions` family (+ its 3 nested resources) are now genuinely
unblocked by this schema fix but **not yet wired** -- real, disclosed follow-up, not a full sweep
of the newly-unblocked scope in this pass. `Nudr_GroupIDmap`'s own separate
`onGroupIdMapChange` callback remains a different API, still entirely unimplemented. Combined
total across both functions: **61 real per-UE call sites (`notify_subscribers`) + 6 real
non-per-UE call sites (`notify_subscribers_ue_less`) = 67 real write-route call sites**, 33
distinct resource types now wired (31 per-UE + 2 non-per-UE).

