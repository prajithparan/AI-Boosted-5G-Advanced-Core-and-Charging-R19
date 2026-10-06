## ADR-0152: gap-closure task #106 continuation -- UDR real AMF Subscription Info (Document)
nested under an individual ee-subscription

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (61 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0151). ADR-0148/ADR-0151 corrected ADR-0122's
own blanket "genuinely deeply-nested" deferral for `ee-subscriptions`/`sdm-subscriptions`
themselves, but their own deeper nested sub-collections (`amf-`/`smf-`/`hss-subscriptions` under
each `ee-subscriptions/{subsId}`, `hss-sdm-subscriptions` under each `sdm-subscriptions/{subsId}`)
had not yet been individually read. This ADR performs that read for the first of them,
`amf-subscriptions`, rather than continuing to leave the whole nested tree blanket-deferred.

Real, confirmed-by-YAML-read: `/subscription-data/{ueId}/context-data/ee-subscriptions/{subsId}/
amf-subscriptions` (real spec operations "Create AMF Subscriptions" [PUT]/
`GetAmfSubscriptionInfo` [GET]/`ModifyAmfSubscriptionInfo` [PATCH]/`RemoveAmfSubscriptionsInfo`
[DELETE]) is genuinely buildable -- no query-param blockers, no complex path-key encoding (both
`ueId` and `subsId` are plain path segments this project already knows how to handle). Two real,
disclosed shapes, both new for this project:
1. The document body is a JSON ARRAY of `AmfSubscriptionInfo` (`minItems: 1`), not a single
   object -- confirmed by reading the real schema `$ref` inside a `type: array` wrapper on both
   the PUT request body and the GET/PUT response. Real schema `AmfSubscriptionInfo`: required
   `amfInstanceId` + `subscriptionId` (a `Uri`, despite the name -- confirmed against the
   generated DTO field-for-field before use), plus optional `subsChangeNotifyCorrelationId`/
   `contextInfo`.
2. PUT documents a real, distinct `201` (create) vs. `204` (update) -- the same
   is-new-tracking shape this project already uses for `amf-3gpp-access` (`AmfContextStore`), not
   a new pattern to invent.

Real, disclosed design decision: no referential integrity is enforced against the parent
`ee-subscriptions/{subsId}` resource (`EeSubscriptionsStore`) -- this `amf-subscriptions` document
is addressable by any `(ueId, subsId)` pair regardless of whether that `ee-subscriptions` entry
was itself ever created via `POST`. This matches this project's own established precedent
elsewhere (e.g. `pp-data-store`) of not enforcing cross-resource existence checks the real spec
itself does not document as a requirement.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_ee_amf_subscription_info` table (`ue_id`, `subs_id`
  composite PK, `data` JSONB -- the JSONB value holds the whole array).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `EeAmfSubscriptionInfoStore` class (`put`/`get`/
  `apply_patch`/`remove`) -- `put()` uses the same real `xmax = 0` Postgres idiom as
  `AmfContextStore` to report is-new for 201-vs-204 selection in one UPSERT statement.
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters (write [PUT+PATCH share
  it], get, delete), and four new routes (GET/PUT/PATCH/DELETE) at
  `.../ee-subscriptions/{subsId}/amf-subscriptions`, using
  `sbi_core::http2::parse_json_body<std::vector<sbi_gen::AmfSubscriptionInfo>>` for the PUT body
  -- `parse_json_body`'s existing template already handles a vector element type transparently via
  nlohmann::json's own array deserialization, no new helper needed.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`) backed by real PostgreSQL, SUPI `imsi-999700000000017`,
`subsId` `test-subs-001`:

- `GET` before any `PUT` -> real `404`.
- `PUT` with a one-element array
  `[{"amfInstanceId":"<uuid>","subscriptionId":"https://amf.example.com/subs/1"}]` -> real `201`,
  body echoed back as the same array.
- `GET` -> real `200` with the same array.
- `PUT` again with a two-element array (adding a second `AmfSubscriptionInfo`) -> real `204`
  (confirms is-new tracking correctly reports "update" the second time); `GET` after -> real `200`
  with both elements present.
- `PATCH` with a real RFC 6902 `[{"op":"remove","path":"/1"}]` (removing the second array
  element) -> real `204`; `GET` after -> real `200` with only the first element remaining --
  confirms RFC 6902 array-index operations work correctly against this array-valued document, not
  only object-valued ones.
- `PATCH` against a nonexistent `subsId` -> real `404`.
- Direct `psql SELECT` against `udr_ee_amf_subscription_info` independently confirmed the single
  row's JSONB array matched curl's response exactly at this point.
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #62 of free5GC's ~42+ real `Nudr_DataRepository` resources -- the first of
`ee-subscriptions`' own nested sub-collections closed. Task #106 remains open: `ee-subscriptions`'
own remaining `smf-subscriptions`/`hss-subscriptions` sibling sub-collections (structurally
similar, not yet individually read), `sdm-subscriptions`' own `hss-sdm-subscriptions` nested
sub-collection, `group-data`'s own parallel `ee-subscriptions/{subsId}/...` tree (not yet
surveyed), `group-data`'s remaining genuinely-blocked resources, bare
`/subscription-data/{ueId}`/`{ueId}/context-data`, and the other genuinely deferred subsystems
(`pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

