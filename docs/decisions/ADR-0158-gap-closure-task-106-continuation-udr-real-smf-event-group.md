## ADR-0158: gap-closure task #106 continuation -- UDR real SMF Event Group Subscription Info
(Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (67 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0157). This ADR closes the second of
`group-data`'s own `ee-subscriptions/{subsId}/...` nested sub-collections, `smf-subscriptions`
-- the sibling of `amf-subscriptions` (ADR-0157).

Real, confirmed-by-YAML-read: `/subscription-data/group-data/{ueGroupId}/ee-subscriptions/
{subsId}/smf-subscriptions` (real spec operations `CreateSmfGroupSubscriptions` [PUT]/
`GetSmfGroupSubscriptions` [GET]/`ModifySmfGroupSubscriptions` [PATCH]/
`RemoveSmfGroupSubscriptions` [DELETE]) is structurally identical to
`ee-subscriptions/{subsId}/smf-subscriptions` (ADR-0153) but keyed by `ueGroupId` instead of
`ueId`: same single-object `SmfSubscriptionInfo` document body (unlike its array-valued
`amf-subscriptions` sibling), real distinct 201-vs-204 PUT. `GetSmfGroupSubscriptions`'s own
response correctly cites `SmfSubscriptionInfo` -- no typo this time, unlike the `hss-subscriptions`
family (ADR-0154/ADR-0155).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_group_smf_subscription_info` table (`ue_group_id`,
  `subs_id` composite PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `GroupSmfSubscriptionInfoStore` class (`put`/`get`/
  `apply_patch`/`remove`), structurally identical to `EeSmfSubscriptionInfoStore` (ADR-0153).
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters, and four new routes
  (GET/PUT/PATCH/DELETE) at `.../group-data/{ueGroupId}/ee-subscriptions/{subsId}/
  smf-subscriptions`, using `sbi_core::http2::parse_json_body<sbi_gen::SmfSubscriptionInfo>` for
  the PUT body and the `resolved_location()` helper (ADR-0157) for the `Location` header, so this
  new route never risks the class of bug fixed there.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token), `ueGroupId`
`grp-smf-test-001`, `subsId` `smf-subs-001`:

- `GET` before any `PUT` -> real `404`.
- `PUT` with `{"smfSubscriptionList":[{"smfInstanceId":"<uuid>",
  "subscriptionId":"https://smf.example.com/subs/001"}]}` -> real `201`, body echoed back,
  `Location` header confirmed to contain both the real `ueGroupId` and real `subsId`.
- `GET` -> real `200` with the same body.
- `PUT` again with different values -> real `204`; `GET` after confirms a genuine wholesale
  overwrite.
- `PATCH` (real RFC 6902, `/smfSubscriptionList/0/smfInstanceId`) -> real `204`; `GET` after
  reflects the patched value.
- Direct `psql SELECT` against `udr_group_smf_subscription_info` independently confirmed the row
  matched curl's response.
- Cross-checked `udr_group_amf_subscription_info` and `udr_ee_smf_subscription_info` -> both real
  `0` rows, confirming genuinely separate storage from both the `amf-subscriptions` sibling and
  the `ueId`-scoped `smf-subscriptions` sibling.
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #68 of free5GC's ~42+ real `Nudr_DataRepository` resources. `group-data`'s own
`ee-subscriptions/{subsId}/hss-subscriptions` nested sub-collection (the third and final sibling)
remains genuinely deferred, not yet built. Task #106 remains open: that nested sub-collection,
`group-data`'s remaining genuinely-blocked resources, bare
`/subscription-data/{ueId}`/`{ueId}/context-data`, and the other genuinely deferred subsystems
(`pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

