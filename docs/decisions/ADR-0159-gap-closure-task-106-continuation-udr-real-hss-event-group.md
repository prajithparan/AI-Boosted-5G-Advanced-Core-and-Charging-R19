## ADR-0159: gap-closure task #106 continuation -- UDR real HSS Event Group Subscription Info
(Document), completing group-data's own nested-subscription tree

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (68 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0158). This ADR closes the third and final of
`group-data`'s own `ee-subscriptions/{subsId}/...` nested sub-collections, `hss-subscriptions`
-- the sibling of `amf-subscriptions` (ADR-0157) and `smf-subscriptions` (ADR-0158).

Real, confirmed-by-YAML-read: `/subscription-data/group-data/{ueGroupId}/ee-subscriptions/
{subsId}/hss-subscriptions` (real spec operations `CreateHssGroupSubscriptions` [PUT]/
`GetHssGroupSubscriptions` [GET]/`ModifyHssGroupSubscriptions` [PATCH]/
`RemoveHssGroupSubscriptions` [DELETE]) is structurally identical to
`ee-subscriptions/{subsId}/hss-subscriptions` (ADR-0154) but keyed by `ueGroupId` instead of
`ueId`: same single-object `HssSubscriptionInfo` document body, real distinct 201-vs-204 PUT.
`GetHssGroupSubscriptions`'s own response correctly cites `HssSubscriptionInfo` -- no
response-schema typo here, unlike its `ueId`-scoped sibling (ADR-0154) and the
`sdm-subscriptions`-scoped sibling (ADR-0155).

**Real, disclosed spec inconsistency found on direct read, no ambiguity requiring a choice:** the
real spec's own `DELETE`/`PATCH`/`GET` operations on this path declare a parameter named
`externalGroupId` (`$ref: TS29503_Nudm_SDM.yaml#/components/schemas/ExtGroupId`), but the actual
URL path template for this resource is literally `.../group-data/{ueGroupId}/ee-subscriptions/
{subsId}/hss-subscriptions` -- there is no `{externalGroupId}` placeholder anywhere in the path,
only `{ueGroupId}` and `{subsId}`. `PUT` on this same path correctly declares `ueGroupId`
(`$ref: #/components/schemas/VarUeGroupId`), matching the real path template and every sibling
resource in this family. Unlike the earlier `HssSubscriptionInfo`-vs-`SmfSubscriptionInfo`
response-schema typos (ADR-0154/ADR-0155), this is not a genuine design ambiguity requiring a
choice between two real, valid behaviors: `externalGroupId` simply isn't a bindable value from
the real, literal path template at all (the router only captures named segments that actually
appear in the URL), so `ueGroupId` is the only consistent, correct choice -- implemented
throughout without needing to ask.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_group_hss_subscription_info` table (`ue_group_id`,
  `subs_id` composite PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `GroupHssSubscriptionInfoStore` class (`put`/`get`/
  `apply_patch`/`remove`), structurally identical to `EeHssSubscriptionInfoStore` (ADR-0154).
- `nfs/udr/src/main.cpp`: store construction, three new OTel counters, and four new routes
  (GET/PUT/PATCH/DELETE) at `.../group-data/{ueGroupId}/ee-subscriptions/{subsId}/
  hss-subscriptions`, using `sbi_core::http2::parse_json_body<sbi_gen::HssSubscriptionInfo>` for
  the PUT body and the `resolved_location()` helper (ADR-0157) for the `Location` header, keyed
  on `ueGroupId` (not the spec's inconsistent `externalGroupId` name) per the disclosure above.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process (freshly built, freshly started, registered
with a freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token), `ueGroupId`
`grp-hss-test-001`, `subsId` `hss-subs-001`:

- `GET` before any `PUT` -> real `404`.
- `PUT` with `{"hssSubscriptionList":[{"hssInstanceId":"<uuid>",
  "subscriptionId":"https://hss.example.com/subs/001"}]}` -> real `201`, body echoed back,
  `Location` header confirmed to contain both the real `ueGroupId` and real `subsId`.
- `GET` -> real `200` with the same body.
- `PUT` again with different values -> real `204`; `GET` after confirms a genuine wholesale
  overwrite.
- `PATCH` (real RFC 6902, `/hssSubscriptionList/0/hssInstanceId`) -> real `204`; `GET` after
  reflects the patched value.
- Direct `psql SELECT` against `udr_group_hss_subscription_info` independently confirmed the row
  matched curl's response.
- Cross-checked `udr_group_amf_subscription_info`, `udr_group_smf_subscription_info`, and
  `udr_ee_hss_subscription_info` -> all real `0` rows, confirming genuinely separate storage from
  all three siblings.
- `DELETE` -> real `204`; `GET` after -> real `404`; `psql SELECT` confirmed zero rows remained.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #69 of free5GC's ~42+ real `Nudr_DataRepository` resources, and **completes all
three of `group-data`'s own `ee-subscriptions/{subsId}/...` nested sub-collections**
(`amf-`/`smf-`/`hss-subscriptions`, ADR-0157/ADR-0158/ADR-0159) -- mirroring the same closure
already achieved for the `ueId`-scoped `ee-subscriptions` family (ADR-0152/ADR-0153/ADR-0154).
Task #106 remains open: `group-data`'s remaining genuinely-blocked resources (bare collection
GETs on `5g-vn-groups`/`mbs-group-membership`, blocked on real array-query-params), bare
`/subscription-data/{ueId}`/`{ueId}/context-data`, and the other genuinely deferred subsystems
(`pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

