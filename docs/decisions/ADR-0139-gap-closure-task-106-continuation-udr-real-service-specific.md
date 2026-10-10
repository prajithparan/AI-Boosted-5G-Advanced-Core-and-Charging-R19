## ADR-0139: gap-closure task #106 continuation -- UDR real Service Specific Authorization Info (Document)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (49 of free5GC's ~42+ real
`Nudr_DataRepository` resources closed as of ADR-0137). Real, confirmed-by-YAML-read: surveyed
both real siblings in the same pass:

- `/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}` (real spec
  `operationId` `GetSSAuData`) -- genuinely blocked, not attempted: its real, required query
  parameters include `single-nssai`, which the spec defines with `content: application/json` (a
  JSON-encoded object passed as a query string value) -- the same complex-object query-parameter
  shape already disclosed as blocking `nidd-authorization-data`.
- `/subscription-data/{ueId}/context-data/service-specific-authorizations/{serviceType}` (real
  operations `CreateServiceSpecificAuthorizationInfo`/`GetServiceSpecificAuthorizationInfo`/
  `ModifyServiceSpecificAuthorizationInfo`/`RemoveServiceSpecificAuthorizationInfo` -- real
  PUT+GET+PATCH+DELETE, no complex query parameters at all) -- this is the one implemented by this
  ADR. Real schema `ServiceSpecificAuthorizationInfo` requires `serviceSpecificAuthorizationList`,
  a map of `AuthorizationInfo` (`TS29503_Nudm_NIDDAU.yaml`) keyed by `authId`; each
  `AuthorizationInfo` requires `snssai`/`dnn`/`mtcProviderInformation`/`authUpdateCallbackUri`.
  Confirmed the sbi-codegen pipeline already generates real DTOs for both
  (`sbi_gen::ServiceSpecificAuthorizationInfo`, `sbi_gen::AuthorizationInfo` in
  `TS29122_CommonData_grp.hpp`) matching the YAML exactly -- used directly, no hand-written DTO.
  Real PUT response codes are distinct 201-vs-204 and PATCH is real RFC 6902
  `application/json-patch+json`, same shape as `nidd-authorizations`'s own resource (ADR-0121).
  Genuinely keyed by the composite `(ueId, serviceType)` -- `serviceType` is a real plain-string
  enum (`TS29503_Nudm_SSAU.yaml`), no path-segment encoding ambiguity -- so this follows
  `PpDataEntryStore`'s own composite-key precedent (ADR-0109) rather than any single-key store.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_service_specific_auth_info` table, composite primary
  key `(ue_id, service_type)`.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `ServiceSpecificAuthorizationInfoStore` class
  (`put`/`get`/`apply_patch`/`remove`, all keyed by `(ue_id, service_type)`), combining
  `PpDataEntryStore`'s composite-key SQL shape with `NiddAuthorizationInfoStore`'s
  put/apply_patch/remove operation shape.
- `nfs/udr/src/main.cpp`: store construction, two new OTel counters (write, delete), and four new
  routes (PUT/GET/PATCH/DELETE) at
  `/subscription-data/{ueId}/context-data/service-specific-authorizations/{serviceType}`, using
  `sbi_core::http2::parse_json_body<sbi_gen::ServiceSpecificAuthorizationInfo>` for the PUT body.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl sequence against a running `udr` process backed by a real PostgreSQL database, SUPI
`imsi-999700000000001`, serviceType `AF_GUIDANCE_FOR_URSP` (a real enum value):
- `PUT` with a real-shaped body (`authId` `auth-1`, `snssai.sst: 1`, `dnn: "internet"`,
  `mtcProviderInformation: "mtc-provider-1"`, `authUpdateCallbackUri:
  "https://af.example.com/callback"`) -> real `201` with the created resource echoed back.
- `GET` -> real `200` with the same body.
- `PUT` again (same key) -> real `204` (update, not create).
- `GET` on an unseeded `serviceType` (`AF_REQUESTED_QOS`) for the same SUPI -> real `404`.
- `PATCH` with a real RFC 6902 `add` operation (adding a second `authId`, `auth-2`) -> real `204`;
  subsequent `GET` shows both `auth-1` and `auth-2` entries, confirming the patch applied.
- `DELETE` -> real `204`; subsequent `GET` -> real `404`.
- Composite-key isolation confirmed with a second SUPI/serviceType pair
  (`imsi-999700000000002`/`AF_REQUESTED_QOS`) -> real `201`, independently persisted. Direct
  `psql` query against `udr_service_specific_auth_info` confirmed the row and its composite key.

### Testing and verification

`udr` built clean (including a clean rebuild after `clang-format-18`, no formatting diff beyond
what was written). Full `conformance_tests` (excluding the two disclosed pre-existing flaky
tests): 325/325 pass, zero regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #50 of free5GC's ~42+ real `Nudr_DataRepository` resources. The sibling
`service-specific-authorization-data/{serviceType}` GET-only resource remains a real, disclosed
gap blocked on complex-object query-parameter parsing (see Context above) -- not silently skipped.
Task #106 remains open: the not-yet-surveyed remainder of `TS29505_Subscription_Data.yaml`
(`group-data/*`, bare `/subscription-data/{ueId}`,
`ue-update-confirmation-data/subscribed-snssais`, `ue-update-confirmation-data/subscribed-cag`,
and others) and the genuinely deferred subsystems (`ee-subscriptions`/`sdm-subscriptions`,
`subs-to-notify`, `pdtq-data`, `mbs-session-pol-data`, `nidd-authorization-data`,
`service-specific-authorization-data/{serviceType}`, `Nudr_GroupIDmap`'s own `/nf-group-ids`)
remain real, open, disclosed gaps.

