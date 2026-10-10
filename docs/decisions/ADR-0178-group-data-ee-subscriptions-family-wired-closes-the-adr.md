## ADR-0178: group-data `ee-subscriptions` family wired -- closes the ADR-0176 non-per-UE candidate list

### Context

Continuing ADR-0176/ADR-0177's own disclosed follow-up, no new infrastructure. Real resources
wired this pass: `group-data/{ueGroupId}/ee-subscriptions` individual document
(`PUT`+RFC 6902 `PATCH`+`DELETE`, real update-only `PUT`) and its 3 nested per-`subsId`
sub-resources -- `amf-subscriptions` (array body, real distinct 201-vs-204 `PUT`),
`smf-subscriptions` (single-object body), `hss-subscriptions` (single-object body). All four are
the group-data-scoped structural twins of the already-wired per-UE `ee-subscriptions` family
(ADR-0175), keyed by `ueGroupId` instead of `ueId`, confirming their non-per-UE classification
from ADR-0176's own sweep. Same established exclusion as every other collection resource: the
`POST` create route on the collection remains deliberately unwired (no subscriber can watch a
not-yet-created `subsId`).

This closes ADR-0176's own disclosed non-per-UE candidate list in full -- every resource named as
"now unblocked but not yet wired" across ADR-0176 and ADR-0177 is now wired. The only remaining
real gap in `onDataChange`-style delivery is `Nudr_GroupIDmap`'s own separate
`onGroupIdMapChange` callback, a genuinely different real API (not `subs-to-notify`-backed at
all) that is currently unfireable regardless, since `nf-group-ids` itself has no write path
(confirmed again by direct read, unchanged since ADR-0171's own original disclosure).

### Implementation

Identical mechanical pattern to ADR-0177, using `notify_subscribers_ue_less()`: add
`&subs_to_notify, &notify_client, <path_pattern>` to each write route's lambda capture list, call
`notify_subscribers_ue_less(...)` immediately after the mutation succeeds. `PATCH` routes forward
the real submitted RFC 6902 ops via `change_from_json_patch`; `PUT` routes use `change_replace`;
`DELETE` routes use `change_remove`.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real curl against a running `udr` process (with NRF for OAuth2 token issuance) and the same real
HTTPS receiver process from ADR-0171 onward: a real `POST group-data/ue-grp-001/ee-subscriptions`
create -> real `201` with a server-generated `subsId`, then a real `POST subs-to-notify`
subscription (no `ueId`) watching the full resolved individual-document path, followed by a real
RFC 6902 `PATCH` (`replace` on `/callbackReference`) -> real `204`, and the receiver independently
logged a correctly-shaped `DataChangeNotify` with no `"ueId"` key present. Real, disclosed: same
"one new representative resource, not all 4 individually" verification scope as every prior batch,
since `notify_subscribers_ue_less()` is unchanged, already-proven code.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written, 120 insertions / 18 deletions net). Full `conformance_tests`
(excluding the two disclosed pre-existing flaky tests): 331/331 pass, zero regressions.

### What this ADR does NOT include

`Nudr_GroupIDmap`'s own separate `onGroupIdMapChange` callback remains entirely unimplemented
(and currently unfireable regardless, per the disclosure above) -- the one remaining real gap in
this project's `onDataChange`-adjacent delivery coverage. `gpsis`/`ext-group-ids` filtering across
several already-closed GET resources remains a separate, already-disclosed, deliberate scope
narrowing (not new). Combined total: **61 real per-UE call sites + 26 real non-per-UE call sites =
87 real write-route call sites**, 41 distinct resource types now wired (31 per-UE + 10
non-per-UE).

