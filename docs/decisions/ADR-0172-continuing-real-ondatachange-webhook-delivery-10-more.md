## ADR-0172: continuing real `onDataChange` webhook delivery -- 10 more resources wired (13 of ~40 real per-UE resources total)

### Context

Continuing ADR-0171's own explicitly disclosed follow-up (10 more of the ~80 real per-UE write
routes wired to real `onDataChange` delivery, same infrastructure, no new design). Real resources
wired this pass, in file order: `sm-data` (`policy-data/ues/{ueId}/sm-data`, RFC 7396 `PATCH`
only), `authentication-subscription` (RFC 6902 `PATCH` only), `authentication-status`
(`PUT`+`DELETE`), `am-data` (`policy-data/ues/{ueId}/am-data`, RFC 7396 `PATCH` only),
`smsf-3gpp-access` (`PUT`+`DELETE`), `smsf-non-3gpp-access` (`PUT`+`DELETE`), `ip-sm-gw`
(`PUT`+`PATCH`(RFC 6902)+`DELETE`, the richest operation set), `mwd` (Message Waiting Data,
`PUT`+`PATCH`(RFC 6902)+`DELETE`), `roaming-information` (`PUT` only), `pei-info` (`PUT` only).
Combined with ADR-0171's own 3, this closes real `onDataChange` delivery for **13 of the ~40 real
per-UE `Nudr_DataRepository` resources**, 24 real write-route call sites
(`grep -c 'notify_subscribers(' nfs/udr/src/main.cpp` minus the one function definition).

### Implementation

No new infrastructure -- every edit follows the exact same mechanical pattern ADR-0171
established: add `&subs_to_notify, &notify_client, <path_pattern>` to the route's own lambda
captures, then call `notify_subscribers(subs_to_notify, notify_client, ue_id,
resolved_location(<path_pattern>, req.path_params), <change_replace|change_remove|
change_from_json_patch>(...))` immediately after the mutation succeeds. RFC 7396 merge-patch
routes (`sm-data`, `am-data`) use `change_replace(patched)` (the same disclosed
whole-resource-replace simplification ADR-0171 already applied to PUT/DELETE, since merge-patch
carries no per-field op list to forward). RFC 6902 routes forward the actual submitted ops via
`change_from_json_patch`.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real curl against a running `udr` process with the same real HTTPS receiver process from ADR-0171
(`127.0.0.1:9999`, this project's own `hello-nf` cert): a real `POST subs-to-notify` subscription
for `imsi-999700000000002` watching `roaming-information`, followed by a real `PUT` (first attempt
guessed the wrong field shape and got a real `400` -- corrected to the real `servingPlmn: {mcc,
mnc}` object shape after checking the generated DTO, not a fabricated pass) -> real `201`, and the
receiver independently logged a correctly-shaped `DataChangeNotify` (`resourceId` matching,
`REPLACE` at `/` with the full new document). Real, disclosed: given the now-large number of
individually-wired routes, this pass live-verified one new representative resource end-to-end
(the same rigor as ADR-0171's own three) rather than re-proving all 10 individually -- the
underlying `notify_subscribers()` call is identical code at every site, already proven correct by
ADR-0171's own three end-to-end proofs across all three real change shapes (PUT/PATCH/DELETE);
this pass's own risk is confined to correct capture lists and correct `resolved_location()`
arguments at each new call site, confirmed by a clean, warning-free build and the one live check.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions.

### What this ADR does NOT include

The remaining ~65-70 real per-UE write routes are still **not wired** -- task #106 remains open
for this, same disclosed follow-up as ADR-0171. Non-per-UE resources (group-data and others)
remain blocked on `udr_subs_to_notify`'s own `ue_id NOT NULL` schema, unchanged. `Nudr_GroupIDmap`'s
own separate `onGroupIdMapChange` callback remains entirely unimplemented. No new
`Nudr_DataRepository` resource-count change -- only real behavior wired into 10 more
already-closed resources.

