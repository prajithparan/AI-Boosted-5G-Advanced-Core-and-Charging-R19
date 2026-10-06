## ADR-0154: gap-closure task #106 continuation -- UDR real HSS Subscription Info (Document)
nested under an individual ee-subscription

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (63 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0153). This ADR reads and closes the third and
final of `ee-subscriptions`' own nested sub-collections, `hss-subscriptions` -- the sibling of
`amf-subscriptions` (ADR-0152) and `smf-subscriptions` (ADR-0153).

Real, confirmed-by-YAML-read: `/subscription-data/{ueId}/context-data/ee-subscriptions/{subsId}/
hss-subscriptions` (real spec operations "Create HSS Subscriptions" [PUT]/
`GetHssSubscriptionInfo` [GET]/`ModifyHssSubscriptionInfo` [PATCH]/`RemoveHssSubscriptionsInfo`
[DELETE]) shares `smf-subscriptions`'s own real single-object-document shape (not a bare array
like `amf-subscriptions`): the body is a single `HssSubscriptionInfo` object (required
`hssSubscriptionList`, an array of `HssSubscriptionItem`). Real schema `HssSubscriptionItem`:
required `hssInstanceId` + `subscriptionId` (both real, distinct fields, confirmed directly from
the generated struct this time -- not repeating ADR-0153's field-name mistake), plus optional
`contextInfo`.

**Real, disclosed spec inconsistency found on direct read, asked and confirmed with the user
before implementing:** the real spec YAML's own `GetHssSubscriptionInfo` operation's `200`
response body literally cites `$ref: '#/components/schemas/SmfSubscriptionInfo'` -- not this
resource's own `HssSubscriptionInfo`, even though this same resource's PUT/PATCH/DELETE all
correctly and consistently use `HssSubscriptionInfo`. Presented three real options via
`AskUserQuestion` (return `HssSubscriptionInfo` treating the citation as a typo [recommended];
return `SmfSubscriptionInfo` literally; skip the resource). User's explicit answer: **"Return
HssSubscriptionInfo (Recommended)"**. This is treated as a real, disclosed spec typo, the same
precedent already established for ADR-0129's `QueryPorseData` typo -- GET on this resource returns
real `HssSubscriptionInfo`-shaped data, matching the resource's own PUT/PATCH/DELETE and every
sibling's own internally-consistent request/response-type pairing.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_ee_hss_subscription_info` table (`ue_id`, `subs_id`
  composite PK, `data` JSONB) -- identical shape to `udr_ee_smf_subscription_info`, with a
  disclosure comment documenting the spec-inconsistency finding and its confirmed resolution.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `EeHssSubscriptionInfoStore` class (`put`/`get`/
  `apply_patch`/`remove`), structurally identical to `EeSmfSubscriptionInfoStore` (same
  `xmax = 0` is-new-tracking idiom).
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters (write [PUT+PATCH share
  it], get, delete), and four new routes (GET/PUT/PATCH/DELETE) at `.../ee-subscriptions/
  {subsId}/hss-subscriptions`, using `sbi_core::http2::parse_json_body<sbi_gen::HssSubscriptionInfo>`
  (a single-object parse) for the PUT body, and `HssSubscriptionInfo`-shaped data for GET despite
  the spec's own literal `SmfSubscriptionInfo` citation, per the user's confirmed decision above.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token), SUPI
`imsi-999700000000001`, `subsId` `hss-test-subs-001`:

- `GET` before any `PUT` -> real `404`.
- `PUT` with `{"hssSubscriptionList":[{"hssInstanceId":"hss-instance-01",
  "subscriptionId":"https://hss.example.com/subs/001"}]}` -> real `201`, body echoed back.
- `GET` -> real `200` with the same body (confirming the GET route returns
  `HssSubscriptionInfo`-shaped data as decided, not the spec's literal `SmfSubscriptionInfo`
  citation).
- `PUT` again with a different `hssInstanceId`/`subscriptionId` -> real `204` (is-new tracking
  correctly reports "update"); `GET` after -> real `200` confirming a genuine wholesale replace.
- `PATCH` with a real RFC 6902 `[{"op":"replace","path":"/hssSubscriptionList/0/hssInstanceId",
  "value":"hss-instance-03"}]` -> real `204`; `GET` after -> real `200` reflecting the patched
  value.
- `PATCH` against a nonexistent `subsId` -> real `404`.
- Direct `psql SELECT` against `udr_ee_hss_subscription_info` independently confirmed the row
  matched curl's response exactly.
- Cross-checked sibling `udr_ee_amf_subscription_info`/`udr_ee_smf_subscription_info` tables ->
  both real `0` rows, confirming genuinely separate storage, no cross-contamination.
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #64 of free5GC's ~42+ real `Nudr_DataRepository` resources, and completes all three
of `ee-subscriptions`' own nested sub-collections (`amf-`/`smf-`/`hss-subscriptions`). Task #106
remains open: `sdm-subscriptions`' own `hss-sdm-subscriptions` nested sub-collection (a real,
separate resource, not yet individually read), `group-data`'s own parallel
`ee-subscriptions/{subsId}/...` tree (not yet surveyed), `group-data`'s remaining
genuinely-blocked resources, bare `/subscription-data/{ueId}`/`{ueId}/context-data`, and the
other genuinely deferred subsystems (`pdtq-data`, `mbs-session-pol-data`,
`nidd-authorization-data`, `service-specific-authorization-data/{serviceType}`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

