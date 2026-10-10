## ADR-0180: `Nudr_GroupIDmap`'s `onGroupIdMapChange` callback -- fired from the one real mutation point this data has

### Context

The one remaining real gap disclosed at the end of ADR-0178: `Nudr_GroupIDmap`'s own separate
`onGroupIdMapChange` callback (a genuinely different real API from `Nudr_DataRepository`'s
`subs-to-notify`/`onDataChange` infra, backed by `nf-group-id-subscriptions`, ADR-0170) had never
fired at all, because `nf-group-ids` itself has no write path. Presented as a genuine decision
point (giving `nf-group-ids` "a real write path" isn't something the real spec supports -- see
below); user chose to fire the callback from the one real mutation this project's own mapping data
already undergoes, rather than inventing an API surface.

**Direct read of `TS29504_Nudr_GroupIDmap.yaml` confirms**: `/nf-group-ids` genuinely has only a
`get:` operation (`GetNfGroupIDs`) -- no `PUT`/`POST`/`PATCH`/`DELETE` exists anywhere for this
resource in the real spec. This matches this project's own pre-existing code comment on
`NfGroupIdStore` (written at ADR-0164): *"No create/update operation exists anywhere in this
service for the mapping data -- seed() + get() only."* `onGroupIdMapChange` is real and does exist
in the spec, declared as a callback on `POST /nf-group-ids/subscriptions` (`CreateGroupIdSubscription`,
already implemented) -- it conceptually fires when the underlying NF-Group-ID mapping *data*
changes, and per the real spec that data has no SBI-level provisioning path in `Nudr_GroupIDmap`
at all (presumably OAM/configuration-provisioned in a real deployment). Inventing a write endpoint
to hang this off would mean fabricating an API path the YAML doesn't define -- this project's own
single worst disclosed failure mode -- so it was not done.

### Implementation

New `notify_group_id_map_change(nf_group_id_subscriptions, notify_client, subscriber_id, nf_type,
nf_group_id)` in `nfs/udr/src/main.cpp`: looks up every subscription via a new
`NfGroupIdSubscriptionStore::list_all()` (no `nfType`/`nfGroupId` columns exist in the real table,
same "filter in the caller" precedent as `FiveGVnGroupStore::list_all()`'s own `/internal`
filter), matches by the real `SubscriptionData` schema's own required `nfType`+`nfGroupId` fields
(NOT `subscriberId` -- the real subscription key is genuinely coarser than the mapping key,
confirmed by direct read, not an invented simplification), and POSTs a real `GroupIdMapNotify`
body (`subscriberId`+`nfType`+`nfGroupId`, its own three real required fields) to each match's
`notificationUri`. Called three times in `main()`, once per `nf_group_ids.seed(...)` entry,
immediately after `notify_client` is constructed (the seed calls themselves are unchanged, still
exactly where ADR-0164 put them) -- the one real, honest trigger point this data has. Explicitly
disclosed in both the helper's own comment and the call site: this fires during process startup,
before the server accepts any connections, so in every real run it will find zero subscriptions
and deliver nothing in practice -- not hidden, a direct, unavoidable consequence of there being no
live mutation to hook into.

### Live verification (real, live PostgreSQL + real mTLS webhook delivery, not self-consistency)

Real constraint: no live `POST nf-group-ids/subscriptions` call could ever precede this callback
firing (it fires before the server accepts connections), so the normal "subscribe via the live API,
then mutate" methodology used in every prior ADR in this series does not apply here -- the only way
to exercise this real code path end-to-end is to pre-seed a subscription row directly into
PostgreSQL before starting `udr`. Done via `psql`: inserted a row into
`udr_nf_group_id_subscriptions` with `nfType=AMF`, `nfGroupId=amf-group-01`, `notificationUri`
pointing at a real standalone HTTPS receiver, then started `nrf`+`udr`. The receiver independently
logged a correctly-shaped `GroupIdMapNotify`
(`{"nfGroupId":"amf-group-01","nfType":"AMF","subscriberId":"imsi-999700000000001"}`) during
`udr`'s own startup sequence, with zero warnings in `udr`'s own logs -- confirming the matching
logic (by `nfType`+`nfGroupId`, correctly ignoring the non-matching `SMF`/`amf-group-02` seed
entries) and the delivery path both work as designed. A real live-testing mistake was found and
fixed along the way: the receiver script listens on a hardcoded port 9999 (no port argument), and
the first attempt pointed the seeded subscription at port 19999 (this session's own convention
from ADR-0179's automated test) -- the delivery attempt correctly logged a `udr: onGroupIdMapChange
delivery ... failed or non-204` warning (real code behaving correctly against real nothing-
listening), not a silent failure; corrected and re-verified successfully.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18`. Full
`conformance_tests`+`integration_tests` (`ctest`, excluding the two disclosed pre-existing flaky
tests): 332/332 pass, zero regressions.

### What this ADR does NOT include

This does not give `Nudr_GroupIDmap`'s mapping data any new real provisioning path -- it remains
genuinely read-only from this project's own in-scope APIs, matching the real spec. In any real
run of this project's own binaries (not this ADR's own pre-seeded-row live verification), zero
`onGroupIdMapChange` deliveries will ever actually occur, since no subscription can exist before
the one trigger point (startup) has already passed -- disclosed above, not glossed over. If a real
live provisioning path for this mapping data is ever added to a future R19 item in scope, this
same helper is the real integration point to call from it.

