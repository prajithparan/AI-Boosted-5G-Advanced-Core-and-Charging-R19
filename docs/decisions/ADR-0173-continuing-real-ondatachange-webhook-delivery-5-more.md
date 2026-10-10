## ADR-0173: continuing real `onDataChange` webhook delivery -- 5 more resources wired (18 of ~40 real per-UE resources total)

### Context

Continuing ADR-0171/ADR-0172's own disclosed follow-up, same infrastructure, no new design. Real
resources wired this pass: `pp-data` (RFC 6902 `PATCH` only), `pp-data-store`
(`PUT`+`DELETE`, composite `(ueId, afInstanceId)` key), `operator-specific-data`
(`subscription-data` group, RFC 6902 `PATCH` only), `ue-policy-set` (`policy-data` group,
`PUT`+RFC 7396 `PATCH`), `operator-specific-data` (`policy-data` group, a real, distinct resource
from the `subscription-data` one above despite the shared name -- RFC 6902 `PATCH` only). Real,
disclosed: two other resources checked in the same file region (`ee-profile-data`,
`coverage-restriction-data`/`lcs-privacy-data`/`lcs-subscription-data`/`lcs-mo-data`) were
confirmed GET-only (no write route exists) by direct read before moving on -- correctly skipped,
not overlooked. Combined total: **18 of ~40 real per-UE `Nudr_DataRepository` resources now have
real `onDataChange` delivery, 31 real write-route call sites**
(`grep -c 'notify_subscribers(' nfs/udr/src/main.cpp` minus the one function definition).

### Real fix applied while wiring `ue-policy-set`'s own PATCH

`UePolicySetStore::merge_patch()` already returned the real patched document (confirmed by direct
read of its own real declaration, `nlohmann::json merge_patch(...)`), but the existing PATCH
route discarded that return value (`ue_policy_set.merge_patch(ue_id, patch);` with no assignment)
since the route itself only ever needed to return `204`. Wiring `notify_subscribers()` needed the
real patched document to build a correct `change_replace()` payload, so the call site now captures
it (`const auto patched = ue_policy_set.merge_patch(ue_id, patch);`) -- a real, minimal, disclosed
fix to consume an already-available return value, not a new store method.

### Implementation

Identical mechanical pattern to ADR-0171/ADR-0172. RFC 7396 merge-patch (`ue-policy-set`) uses
`change_replace(patched)`; RFC 6902 routes forward the actual submitted ops via
`change_from_json_patch`; `pp-data-store`'s own `PUT`/`DELETE` use `change_replace`/`change_remove`
at the resource's own composite-keyed path.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real curl against a running `udr` process with the same real HTTPS receiver process from ADR-0171/
ADR-0172: a real `POST subs-to-notify` subscription for `imsi-999700000000001` watching
`policy-data/ues/{ueId}/ue-policy-set`, then a real `PUT` -> real `201`, receiver logs a correct
`DataChangeNotify` (`REPLACE` at `/`, `newValue` matching the submitted body); then a real RFC 7396
`PATCH` (`application/merge-patch+json`) -> real `204`, receiver logs a second correct
`DataChangeNotify` with the real **patched** (merged) document as `newValue`, not the raw patch
body -- confirming the `merge_patch()` return-value fix above actually threads through correctly,
not just compiles. Real, disclosed: same "one new representative resource, not all 5 individually"
verification scope as ADR-0172, since `notify_subscribers()` itself is unchanged, already-proven
code.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions.

### What this ADR does NOT include

The remaining ~60-65 real per-UE write routes are still not wired. Non-per-UE resources and
`Nudr_GroupIDmap`'s own separate `onGroupIdMapChange` callback remain unwired, same disclosed
follow-up. No new `Nudr_DataRepository` resource-count change -- only real behavior wired into 5
more already-closed resources.

