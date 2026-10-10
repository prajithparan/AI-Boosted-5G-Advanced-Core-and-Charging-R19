## ADR-0165: gap-closure task #106 continuation -- UDR real GetNiddAuData, whose real blocker
turned out to be a different query-param shape than ADR-0161's array-parsing infra

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (71 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0162, plus two real `Nudr_GroupIDmap` resources
from ADR-0120/ADR-0164). This ADR closes `GetNiddAuData`
(`/subscription-data/{ueId}/nidd-authorization-data`, `TS29505_Subscription_Data.yaml` lines
7403-7466) -- named as a real candidate in ADR-0162's/ADR-0164's own "What this ADR does NOT
include" sections.

Real finding on direct YAML read: this resource is **not** actually blocked by the `style: form,
explode: false` array-query-param gap ADR-0161 closed. Its real REQUIRED `single-nssai` query
param instead uses `content: application/json` (a JSON-encoded query value, `$ref` to `VarSnssai`
-> `Snssai`, required `sst: integer`, optional `sd: string`) -- a genuinely different OpenAPI query
parameter shape, one this project's own earlier deferred-list comments (ADR-0133/ADR-0139) had
correctly flagged as "no parsing precedent," predating ADR-0161's infra and never revisited since.
Also real REQUIRED `dnn` (plain string) and `mtc-provider-information` (plain string,
`MtcProviderInformation`); optional `af-id` (plain string, not honored). Response schema
`AuthorizationData` (required `authorizationData: array of UserIdentifier`, min 1 item; optional
`allowedDnnList`/`allowedSnssaiList`/`allowedMtcProviders`/`validityTime`). Real `404` documented
for the no-match case.

Real, disclosed, notable finding while reading this resource's spec text: `AuthorizationData` is
the exact same schema ADR-0160 found cross-referenced by the deliberately-deferred `GetSSAuData`
(`/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}`) -- and
`AuthorizationData`'s own description, "NIDD Authorization Information", plus its real usage here
in `GetNiddAuData`, confirms its real, unambiguous home is this resource, not `GetSSAuData`. This
doesn't change `GetSSAuData`'s own already-settled deferred status (the user's prior decision to
leave it deferred, ADR-0160, stands on its own separate structural-mismatch grounds against its
own storage sibling, not on this schema-citation question) -- noted here as useful confirmation,
not an action item.

Confirmed by direct read (this file's own path-item block for `/subscription-data/{ueId}/
nidd-authorization-data`, lines 7403-7500) that only a `get:` operation exists -- no
create/update/delete anywhere in this project's real, in-scope APIs for this document. The real
provisioning path for NIDD authorization data is UDM's own `Nudm_NIDDAU` service (confirmed
`AuthorizationData`/`NiddAuthUpdateInfo`/`UserIdentifier` schemas live in
`TS29503_Nudm_NIDDAU.yaml`), out of this project's current scope. Applies the same "GET-only, no
write path exists in this project's own APIs, seeded at startup" precedent already established by
`routing_ids`/`nf_group_ids`.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_nidd_authorization_data` table, composite-keyed by
  `(ue_id, sst, sd, dnn, mtc_provider_information)` per the resource's own real required
  query-param filters. Real, disclosed: `sd` stored as an empty string when absent (`Snssai`'s own
  schema only requires `sst`; PostgreSQL primary-key columns cannot be `NULL`).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `NiddAuthorizationDataStore` class (`seed`/`get`), same
  shape as `RoutingIdStore`.
- `nfs/udr/src/main.cpp`: store construction, real seed data (this project's own already-seeded
  test SUPI `imsi-999700000000001`, this project's own already-established lab S-NSSAI
  `sst=1/sd="000001"` (ADR-0016) and lab DNN `"internet"`, a real-arbitrary
  `mtc-provider-information` string since the schema has no documented format to match; response
  body is a minimal-but-valid `AuthorizationData` document, one `UserIdentifier` with its required
  `supi`), one new OTel counter, and the new `GET` route.
- Real, disclosed implementation choice: `single-nssai`'s JSON-encoded query value is parsed with
  a direct `json::parse()` of the already-percent-decoded query-param string (confirmed
  `libs/sbi-core/src/http2_server.cpp`'s own query-string parser already percent-decodes both key
  and value) -- no new shared infra, since this is a narrow, single-consumer parsing need, unlike
  the array-style `style: form, explode: false` params ADR-0161 built genuinely reusable infra
  for. Malformed JSON or a missing `sst` field both return a real `400` with a specific message.
- Real, disclosed scope choice: the optional `af-id` filter is not honored, matching the
  established "optional filter not honored" precedent.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process (freshly built, freshly started, registered with a
freshly started `nrf`, mTLS client cert + real NRF-issued OAuth2 bearer token), at the real
`/nudr-dr/v2/subscription-data/{ueId}/nidd-authorization-data` path:

- `GET` with all query parameters missing -> real `400`.
- `GET` with only `single-nssai` present (`dnn`/`mtc-provider-information` missing) -> real `400`.
- `GET` with a malformed (non-JSON) `single-nssai` value -> real `400` with a specific parse-error
  message.
- `GET` with a well-formed JSON `single-nssai` missing its required `sst` field -> real `400`.
- `GET` with the real seeded combination
  (`single-nssai={"sst":1,"sd":"000001"}&dnn=internet&mtc-provider-information=mtc-provider-1`) ->
  real `200` with `{"authorizationData":[{"supi":"imsi-999700000000001"}]}`.
- `GET` with an unseeded `dnn` (`dnn=ims`, same `single-nssai`/`mtc-provider-information`) -> real
  `404`.
- `GET` with `single-nssai={"sst":1}` (no `sd`) against the seeded `sd="000001"` row -> real `404`,
  confirming the composite key's precision (an absent-`sd` lookup does not accidentally match a
  present-`sd` row).
- Direct `psql SELECT` against `udr_nidd_authorization_data` independently confirmed exactly the
  one seeded row, matching the successful curl response exactly.

### Testing and verification

`udr` built clean (zero warnings) both before and after `clang-format-18` (reformat added no diff
beyond what was newly written). Full `conformance_tests` (excluding the two disclosed pre-existing
flaky tests): 331/331 pass, zero regressions -- same count as ADR-0164, since this ADR adds no new
automated test (same disclosed manual-live-verification precedent already established for
GET-only, seeded-at-startup resources).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #72 of free5GC's ~42+ real `Nudr_DataRepository` resources
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open: bare `/subscription-data/{ueId}`
(`QueryUeSubscribedData`) and `group-data`'s own `5g-vn-groups`/`mbs-group-membership` bare
collection GETs remain real candidates genuinely unblocked by ADR-0161's array-parsing infra, not
yet individually closed. `GetSSAuData` remains deliberately deferred per ADR-0160. UDM's own
`Nudm_NIDDAU` service (the real provisioning path for this data) remains out of this project's
current scope.

