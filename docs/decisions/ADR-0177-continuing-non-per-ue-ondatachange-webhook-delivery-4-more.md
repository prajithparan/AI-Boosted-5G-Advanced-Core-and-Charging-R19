## ADR-0177: continuing non-per-UE `onDataChange` webhook delivery -- 4 more resources wired using the ADR-0176 schema fix

### Context

Continuing ADR-0176's own disclosed follow-up using `notify_subscribers_ue_less()`, no new
infrastructure. Real resources wired this pass: `slice-control-data` (RFC 7396 `PATCH`-only,
upsert-capable, no `PUT`/`POST` create operation exists), `group-control-data` (same shape),
`5g-vn-groups` (`PUT`+RFC 6902 `PATCH`+`DELETE`, real distinct 201-only `PUT`, matching the
`bdt-data` precedent), `mbs-group-membership` (identical shape to `5g-vn-groups`, real spec twin).
All four are keyed by `snssai`/`intGroupId`/`externalGroupId` (not `ueId`), confirming they were
correctly excluded from the earlier per-UE sweep.

### Implementation

Identical mechanical pattern to prior batches, using `notify_subscribers_ue_less()` in place of
`notify_subscribers()` (no `ue_id` argument, since none of these resources have one): add
`&subs_to_notify, &notify_client, <path_pattern>` to each write route's lambda capture list, call
`notify_subscribers_ue_less(subs_to_notify, notify_client, resolved_location(<path_pattern>,
req.path_params), <change_replace|change_remove|change_from_json_patch>(...))` immediately after
the mutation succeeds.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real curl against a running `udr` process (with NRF for OAuth2 token issuance) and the same real
HTTPS receiver process from ADR-0171 onward: a real `POST subs-to-notify` subscription (no `ueId`
field) watching the full resolved `5g-vn-groups/vn-grp-001` path, followed by a real `PUT` -> real
`201` and a real `DELETE` -> real `204`, both independently logged by the receiver as
correctly-shaped `DataChangeNotify` bodies (`REPLACE`/`REMOVE` at the real RFC 6901 root pointer
`"/"`, no `"ueId"` key present in either delivered body). Real, disclosed: same "one new
representative resource, not all 4 individually" verification scope as prior batches, since
`notify_subscribers_ue_less()` is unchanged, already-proven code (live-verified once already in
ADR-0176) -- correctness at the other three new call sites rests on a clean, warning-free build
plus code review of each capture list and `resolved_location()` argument.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written, 80 insertions / 12 deletions net). Full `conformance_tests`
(excluding the two disclosed pre-existing flaky tests): 331/331 pass, zero regressions.

### What this ADR does NOT include

The `group-data/{ueGroupId}/ee-subscriptions` family (+ its 3 nested
`amf-subscriptions`/`smf-subscriptions`/`hss-subscriptions` sub-resources) remains unwired --
real, disclosed follow-up, the last real candidate batch unblocked by ADR-0176's own schema fix.
`Nudr_GroupIDmap`'s own separate `onGroupIdMapChange` callback remains a different API, still
entirely unimplemented. Combined total: **61 real per-UE call sites + 14 real non-per-UE call
sites = 75 real write-route call sites**, 37 distinct resource types now wired (31 per-UE + 6
non-per-UE).

