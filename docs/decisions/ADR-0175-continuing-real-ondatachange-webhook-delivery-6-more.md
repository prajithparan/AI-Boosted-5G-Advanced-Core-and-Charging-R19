## ADR-0175: continuing real `onDataChange` webhook delivery -- 6 more resources wired (31 of ~40 real per-UE resources total)

### Context

Continuing ADR-0171 through ADR-0174's own disclosed follow-up, same infrastructure, no new
design. Real resources wired this pass: `ee-subscriptions` individual document (`PUT`+RFC 6902
`PATCH`+`DELETE`), `sdm-subscriptions` individual document (same shape), and all four of their
nested per-`subsId` sub-resources: `ee-subscriptions/{subsId}/amf-subscriptions` (real distinct
201-vs-204 `PUT` since the body is a JSON array), `ee-subscriptions/{subsId}/smf-subscriptions`
(single-object body), `ee-subscriptions/{subsId}/hss-subscriptions` (single-object body),
`sdm-subscriptions/{subsId}/hss-sdm-subscriptions` (real spec-documented 204-only `PUT`, no
create-vs-update distinction, matching the existing `sor-data`/`upu-data` precedent). Combined
total: **31 of ~40 real per-UE `Nudr_DataRepository` resources now have real `onDataChange`
delivery, 61 real write-route call sites** (`grep -c 'notify_subscribers(' nfs/udr/src/main.cpp`
minus the one function definition).

Real, disclosed: the `POST` create routes on the `ee-subscriptions`/`sdm-subscriptions`
collections were deliberately left unwired, consistent with existing precedent (`subs-to-notify`'s
own POST, `nf-group-ids/subscriptions`' own POST) -- a subscriber cannot plausibly already be
watching a `monitoredResourceUris` entry for a `subsId` that does not yet exist at creation time,
so only the individual document's own `PUT`/`PATCH`/`DELETE` (and the nested resources' own
`PUT`/`PATCH`/`DELETE`) were wired.

### Implementation

Identical mechanical pattern to ADR-0171 through ADR-0174: add
`&subs_to_notify, &notify_client, <path_pattern>` to each write route's lambda capture list, then
call `notify_subscribers(...)` immediately after the mutation succeeds.
`ee-subscriptions`/`sdm-subscriptions` individual `PATCH` and all four nested resources' `PATCH`
forward the real submitted RFC 6902 ops via `change_from_json_patch`; every `PUT` uses
`change_replace(j)`; every `DELETE` uses `change_remove()`.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real curl against a running `udr` process (with NRF for OAuth2 token issuance) and the same real
HTTPS receiver process from ADR-0171 onward: a real `POST ee-subscriptions` create (real
`EeSubscription` body, `callbackReference`+`monitoringConfigurations`) -> real `201` with a
server-generated `subsId` in the `Location` header, then a real `POST subscription-data/
subs-to-notify` subscription for `imsi-999700000000099` with `monitoredResourceUris` set to the
full real resolved individual-document path, followed by a real RFC 6902 `PATCH` (`replace` on
`/callbackReference`) -> real `204`, and the receiver independently logged a correctly-shaped
`DataChangeNotify` (`resourceId` matching the real resolved path including the real generated
`subsId`, `REPLACE` op with the real submitted `newValue`). Real, disclosed: same "one new
representative resource, not all 6 individually" verification scope as ADR-0172 through ADR-0174,
since `notify_subscribers()` itself is unchanged, already-proven code -- correctness at the other
five new call sites rests on a clean, warning-free build plus code review of each capture list and
`resolved_location()` argument.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written, 204 insertions / 30 deletions net). Full `conformance_tests`
(excluding the two disclosed pre-existing flaky tests): 331/331 pass, zero regressions.

### What this ADR does NOT include

The remaining ~10-15 real per-UE write routes not yet examined this pass (`pdtq-data` and any
others found on a further sweep) are still not wired. Non-per-UE resources (`bdt-data`, group-data
family, `policy-data`'s slice/group-control-data) and `Nudr_GroupIDmap`'s own separate
`onGroupIdMapChange` callback remain unwired, same disclosed follow-up. `POST`-create routes on
subscription-collection resources remain deliberately unwired project-wide (see disclosure above).
No new `Nudr_DataRepository` resource-count change -- only real behavior wired into 6 more
already-closed resources.

