## ADR-0155: gap-closure task #106 continuation -- UDR real HSS SDM Subscription Info (Document)
nested under an individual sdm-subscription

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (64 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0154). This ADR reads and closes
`sdm-subscriptions`' own final deferred nested sub-collection, `hss-sdm-subscriptions`.

Real, confirmed-by-YAML-read: `/subscription-data/{ueId}/context-data/sdm-subscriptions/{subsId}/
hss-sdm-subscriptions` (real spec operations "Create HSS SDM Subscriptions" [PUT]/
`GetHssSDMSubscriptionInfo` [GET]/`ModifyHssSDMSubscriptionInfo` [PATCH]/
`RemoveHssSDMSubscriptionsInfo` [DELETE]) reuses the exact same `HssSubscriptionInfo` schema as
`ee-subscriptions`' own `hss-subscriptions` sibling (ADR-0154) for both the PUT request body and
(per the decision below) the GET response body.

**Two real, disclosed findings from direct read, before implementing:**

1. Unlike `amf-`/`smf-`/`hss-subscriptions` under `ee-subscriptions` (all of which document a
   real, distinct `201`-vs-`204` PUT), this resource's real spec PUT response list documents
   ONLY `204` -- no `201` anywhere in the operation. Checked against this project's own existing
   precedent rather than guessing: `sor-data`/`upu-data` (ADR-0143, `/ue-update-confirmation-data/
   sor-data`) already implement this exact same real "single 204-only, upsert-capable PUT"
   contract, confirmed by direct comparison of both real spec blocks. Applied that established
   precedent directly -- `SdmHssSubscriptionInfoStore::put()` is `void`, always returns `204`,
   no create-vs-update distinction (there is none to make; the real spec never documents one).
2. `GetHssSDMSubscriptionInfo`'s own `200` response again literally cites
   `$ref: '#/components/schemas/SmfSubscriptionInfo'`, not `HssSubscriptionInfo` -- the identical
   typo class just found and resolved (with explicit user confirmation) in ADR-0154 for the
   sibling `hss-subscriptions` resource under `ee-subscriptions`. Rather than re-running
   `AskUserQuestion` for what is the same schema-citation error on a structurally parallel
   resource in the same YAML file, the already-confirmed resolution was applied directly and
   disclosed here: GET returns real `HssSubscriptionInfo`-shaped data, matching this resource's
   own PUT/PATCH/DELETE.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_sdm_hss_subscription_info` table (`ue_id`, `subs_id`
  composite PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SdmHssSubscriptionInfoStore` class (`put`/`get`/
  `apply_patch`/`remove`) -- `put()` is `void` (single-response-code upsert, same idiom as
  `SorDataStore`/`UpuDataStore`), `get`/`apply_patch`/`remove` structurally identical to
  `EeHssSubscriptionInfoStore`'s own composite-key methods.
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters (write [PUT+PATCH share
  it], get, delete), and four new routes (GET/PUT/PATCH/DELETE) at `.../sdm-subscriptions/
  {subsId}/hss-sdm-subscriptions`, using `sbi_core::http2::parse_json_body<sbi_gen::HssSubscriptionInfo>`
  for the PUT body and real `HssSubscriptionInfo`-shaped data for GET. PUT always responds real
  `204`, matching the spec's own single documented success code.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token), SUPI
`imsi-999700000000001`, `subsId` `sdm-hss-test-subs-001`:

- `GET` before any `PUT` -> real `404`.
- `PUT` with `{"hssSubscriptionList":[{"hssInstanceId":"hss-sdm-instance-01",
  "subscriptionId":"https://hss.example.com/sdm-subs/001"}]}` -> real `204` (no `201`, matching
  the real spec's own single documented success code, not this project's usual distinct-PUT
  pattern).
- `GET` -> real `200` with the same body, confirming GET returns `HssSubscriptionInfo`-shaped
  data as decided, not the spec's literal `SmfSubscriptionInfo` citation.
- `PUT` again with a different `hssInstanceId`/`subscriptionId` -> real `204`; `GET` after ->
  real `200` confirming a genuine wholesale replace.
- `PATCH` with a real RFC 6902 `[{"op":"replace","path":"/hssSubscriptionList/0/hssInstanceId",
  "value":"hss-sdm-instance-03"}]` -> real `204`; `GET` after -> real `200` reflecting the
  patched value.
- `PATCH` against a nonexistent `subsId` -> real `404`.
- Direct `psql SELECT` against `udr_sdm_hss_subscription_info` independently confirmed the row
  matched curl's response exactly.
- Cross-checked `udr_ee_hss_subscription_info` and `udr_sdm_subscriptions` -> both real `0` rows,
  confirming genuinely separate storage from both the `ee-subscriptions` sibling and the parent
  `sdm-subscriptions` document.
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #65 of free5GC's ~42+ real `Nudr_DataRepository` resources, and this is
`sdm-subscriptions`' own final deferred nested sub-collection. Task #106 remains open:
`group-data`'s own parallel `ee-subscriptions/{subsId}/...` tree (not yet surveyed), `group-data`'s
remaining genuinely-blocked resources, bare `/subscription-data/{ueId}`/`{ueId}/context-data`, and
the other genuinely deferred subsystems (`pdtq-data`, `mbs-session-pol-data`,
`nidd-authorization-data`, `service-specific-authorization-data/{serviceType}`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

