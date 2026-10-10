## ADR-0174: continuing real `onDataChange` webhook delivery -- 7 more resources wired (25 of ~40 real per-UE resources total)

### Context

Continuing ADR-0171/ADR-0172/ADR-0173's own disclosed follow-up, same infrastructure, no new
design. Real resources wired this pass, in file order after `operator-specific-data`
(`policy-data` group): `nidd-authorizations` (`subscription-data/{ueId}/context-data`, RFC 6902
`PUT`+`PATCH`+`DELETE`), `identity-data` (`GET`+`PATCH` only, no `PUT`/`POST` create operation
exists in the spec -- `apply_patch` is upsert-capable, same precedent as `pp-data`),
`service-specific-authorizations` (composite `(ueId, serviceType)` key, `PUT`+RFC 6902
`PATCH`+`DELETE`), and four `ue-update-confirmation-data` siblings: `subscribed-snssais`
(`PUT` only, real spec-documented single `204` response, no create-vs-update distinction),
`subscribed-cag` (identical shape), `sor-data` (`PUT`+RFC 6902 `PATCH`), `upu-data` (`PUT` only).

Real, disclosed: `bdt-data` (`policy-data/bdt-data/{bdtReferenceId}`) was checked in the same file
region and correctly **skipped** -- it is keyed by `bdtReferenceId`, not `ueId`, the same
non-per-UE exclusion already established for the group-data family (`udr_subs_to_notify`'s own
`ue_id NOT NULL` schema cannot represent it). Combined total: **25 of ~40 real per-UE
`Nudr_DataRepository` resources now have real `onDataChange` delivery, 43 real write-route call
sites** (`grep -c 'notify_subscribers(' nfs/udr/src/main.cpp` minus the one function definition).

### Implementation

Identical mechanical pattern to ADR-0171/ADR-0172/ADR-0173: add
`&subs_to_notify, &notify_client, <path_pattern>` to each write route's lambda capture list, then
call `notify_subscribers(subs_to_notify, notify_client, ue_id, resolved_location(<path_pattern>,
req.path_params), <change_replace|change_remove|change_from_json_patch>(...))` immediately after
the mutation succeeds. `identity-data`'s `PATCH` and `service-specific-authorizations`'s `PATCH`
forward the real submitted RFC 6902 ops via `change_from_json_patch`; `sor-data`'s `PATCH` does the
same; every `PUT` route uses `change_replace(j)`; `service-specific-authorizations`'s `DELETE` uses
`change_remove()`.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real curl against a running `udr` process (with NRF for OAuth2 token issuance) and the same real
HTTPS receiver process from ADR-0171 onward (`127.0.0.1:9999`, this project's own `hello-nf`
cert): a real `POST subscription-data/subs-to-notify` subscription for
`imsi-999700000000042` with `monitoredResourceUris` set to the full real resolved resource path
(the real match semantics -- confirmed by reading `notify_subscribers()` -- require the
subscription's own URI to *contain* the resolved path as a substring, not the reverse; an earlier
attempt using a bare `"sor-data"` URI correctly matched nothing, self-corrected before concluding
anything), followed by a real `PUT sor-data` (first attempt guessed `ackInd` as the payload shape
and got a real `400` naming the missing mandatory field; corrected to the real `SorData` schema's
actual required fields, `provisioningTime` and `ueUpdateStatus`, found by reading the generated
`sbi_gen::SorData` struct) -> real `204`, then a real RFC 6902 `PATCH` (`replace` on
`/ueUpdateStatus`) -> real `204`, and the receiver independently logged a correctly-shaped
`DataChangeNotify` (`resourceId` matching the real resolved path, `REPLACE` op at
`/ueUpdateStatus` with the real submitted `newValue`). Real, disclosed: same "one new
representative resource, not all 7 individually" verification scope as ADR-0172/ADR-0173, since
`notify_subscribers()` itself is unchanged, already-proven code -- correctness at the other six
new call sites rests on a clean, warning-free build plus code review of each capture list and
`resolved_location()` argument.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written, 127 insertions / 17 deletions net). Full `conformance_tests`
(excluding the two disclosed pre-existing flaky tests): 331/331 pass, zero regressions.

### What this ADR does NOT include

The remaining ~35-40 real per-UE write routes (`ee-subscriptions`/`sdm-subscriptions` individual
resources and their nested `amf-subscriptions`/`smf-subscriptions`/`hss-subscriptions`,
`pdtq-data`, and others not yet examined this pass) are still not wired. Non-per-UE resources
(`bdt-data`, group-data family, `policy-data`'s slice/group-control-data) and `Nudr_GroupIDmap`'s
own separate `onGroupIdMapChange` callback remain unwired, same disclosed follow-up. No new
`Nudr_DataRepository` resource-count change -- only real behavior wired into 7 more already-closed
resources.

