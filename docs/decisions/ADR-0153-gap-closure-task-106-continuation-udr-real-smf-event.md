## ADR-0153: gap-closure task #106 continuation -- UDR real SMF Event Subscription Info
(Document) nested under an individual ee-subscription

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (62 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0152). This ADR reads and closes the second of
`ee-subscriptions`' own nested sub-collections, `smf-subscriptions` -- the sibling of
`amf-subscriptions` (ADR-0152).

Real, confirmed-by-YAML-read: `/subscription-data/{ueId}/context-data/ee-subscriptions/{subsId}/
smf-subscriptions` (real spec operations "Create SMF Subscriptions" [PUT]/
`GetSmfSubscriptionInfo` [GET]/`ModifySmfSubscriptionInfo` [PATCH]/`RemoveSmfSubscriptionsInfo`
[DELETE]) shares `amf-subscriptions`'s own real, distinct 201-vs-204 PUT shape, but is genuinely
different in one real, disclosed way, confirmed by direct read rather than assumed from the
sibling: the document body is a SINGLE `SmfSubscriptionInfo` object (required
`smfSubscriptionList`, an array of `SmfSubscriptionItem`), not a bare array like
`amf-subscriptions`'s own `AmfSubscriptionInfo[]`. Real schema `SmfSubscriptionItem`: required
`smfInstanceId` + `subscriptionId` (both real, distinct fields -- confirmed the hard way, see
below), plus optional `contextInfo`.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_ee_smf_subscription_info` table (`ue_id`, `subs_id`
  composite PK, `data` JSONB) -- identical shape to `udr_ee_amf_subscription_info`.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `EeSmfSubscriptionInfoStore` class (`put`/`get`/
  `apply_patch`/`remove`), structurally identical to `EeAmfSubscriptionInfoStore` (same `xmax = 0`
  is-new-tracking idiom).
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters (write [PUT+PATCH share
  it], get, delete), and four new routes (GET/PUT/PATCH/DELETE) at `.../ee-subscriptions/
  {subsId}/smf-subscriptions`, using `sbi_core::http2::parse_json_body<sbi_gen::SmfSubscriptionInfo>`
  (a single-object parse, not the `std::vector<...>` used for `amf-subscriptions`) for the PUT
  body.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL, SUPI `imsi-999700000000019`, `subsId`
`test-subs-002`:

Real bug found and fixed mid-verification, disclosed honestly: the first `PUT` attempt used a
hand-crafted `smfSubscriptionList` item with fields `smfInstanceId`/`pduSessionId` and got a real
`400` (`"key 'subscriptionId' not found"`) -- checking the generated `SmfSubscriptionItem` struct
confirmed this was a test error, not an implementation bug: the real schema's second required
field is `subscriptionId` (a `Uri`), there is no `pduSessionId` field on this schema at all (a
name confused with SMF registration's own real `pduSessionId` field elsewhere in this project).
Corrected and re-ran:

- `GET` before any `PUT` -> real `404`.
- `PUT` with `{"smfSubscriptionList":[{"smfInstanceId":"<uuid>",
  "subscriptionId":"https://smf.example.com/subs/1"}]}` -> real `201`, body echoed back.
- `GET` -> real `200` with the same body.
- `PUT` again with a different `subscriptionId` -> real `204` (is-new tracking correctly reports
  "update"); `GET` after -> real `200` confirming a genuine overwrite (single-object replace, not
  append -- the list itself is replaced wholesale, matching the real PUT semantics for a
  single-object document).
- `PATCH` with a real RFC 6902 `[{"op":"replace","path":"/smfSubscriptionList/0/subscriptionId",
  "value":"..."}]` (a nested array-index path within the object) -> real `204`; `GET` after ->
  real `200` reflecting the patched value.
- Direct `psql SELECT` against `udr_ee_smf_subscription_info` independently confirmed the row
  matched curl's response exactly at this point.
- Cross-checked the sibling `amf-subscriptions` resource for the same `(ueId, subsId)` -> real
  `404` (never created there, confirms genuinely separate storage).
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #63 of free5GC's ~42+ real `Nudr_DataRepository` resources -- the second of
`ee-subscriptions`' own nested sub-collections closed. Task #106 remains open: `ee-subscriptions`'
own remaining `hss-subscriptions` sibling sub-collection, `sdm-subscriptions`' own
`hss-sdm-subscriptions` nested sub-collection, `group-data`'s own parallel
`ee-subscriptions/{subsId}/...` tree (not yet surveyed), `group-data`'s remaining
genuinely-blocked resources, bare `/subscription-data/{ueId}`/`{ueId}/context-data`, and the
other genuinely deferred subsystems (`pdtq-data`, `mbs-session-pol-data`,
`nidd-authorization-data`, `service-specific-authorization-data/{serviceType}`,
`Nudr_GroupIDmap`'s own `/nf-group-ids`) remain real, open, disclosed gaps.

