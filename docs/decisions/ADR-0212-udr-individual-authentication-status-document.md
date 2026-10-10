## ADR-0212: UDR Individual Authentication Status (Document) + `QueryProvisionedData` -- second
Tier-B gap-closure, plus a stale-claim correction in the audit doc itself

### Context

Continuing ADR-0193's Tier-B backlog (`docs/CAPABILITY_GAP_ANALYSIS.md`'s own audit note: UDR has
"~7 further undisclosed missing resources in `Subscription_Data`"), this ADR closes two of those
three real `Subscription_Data` items, confirmed by direct read of
`specs/5G_APIs-REL-19/TS29505_Subscription_Data.yaml`, not assumed:

- **Individual Authentication Status (Document)** (`/subscription-data/{ueId}/authentication-data/
  authentication-status/{servingNetworkName}`): `CreateIndividualAuthenticationStatus` (PUT,
  replace), `QueryIndividualAuthenticationStatus` (GET), `DeleteIndividualAuthenticationStatus`
  (DELETE) -- real, genuinely distinct resource from the bare, already-implemented
  `authentication-status` (ADR-0093/task #106): keyed by `(ueId, servingNetworkName)`, not `ueId`
  alone. Same real `AuthEvent` schema (`TS29503_Nudm_UEAU.yaml`) as the bare resource.
- **`QueryProvisionedData`** (`GET /subscription-data/{ueId}/{servingPlmnId}/provisioned-data`):
  the real `ProvisionedDataSets` aggregate resource -- confirmed field-for-field (all 21 real
  properties) already backed by the same per-field stores `QueryUeSubscribedData` (ADR-0166)
  already composes for its own `ProvisionedDataSets`-typed subset; this route reuses those exact
  store/getter calls rather than re-deriving them.

The audit doc's third claimed item, "`subs-to-notify` bulk-DELETE", does **not** correspond to any
real operation: direct read of `TS29505_Subscription_Data.yaml`'s own `subs-to-notify` collection
(`POST` only) and individual (`GET`/`PATCH`/`DELETE` at `/subs-to-notify/{subsId}`) paths confirms
there is no bulk-DELETE-on-the-collection operation defined anywhere in the real spec, and grep
confirms this entire resource is already fully implemented (`nfs/udr/src/main.cpp`, task #122,
ADR-0166-era work). This is a stale/inaccurate claim in the audit doc itself, corrected below
rather than "implemented" -- there is nothing real to implement.

### Implementation

- **`IndividualAuthenticationStatusStore`** (`nfs/udr/src/stores.hpp`/`.cpp`, new
  `udr_individual_authentication_status` table, `PRIMARY KEY (ue_id, serving_network_name)`) --
  same real composite-key doc-store shape as the already-existing
  `ServiceSpecificAuthorizationInfoStore`. Three new routes in `nfs/udr/src/main.cpp` mirror the
  bare `authentication-status` routes' own real behavior exactly (PUT replaces + notifies
  subscribers via the existing `subs-to-notify` wiring, GET returns `404` if absent, DELETE
  removes + notifies, real `204`/`200`/`404` status codes) -- the only difference is the extra
  `servingNetworkName` path parameter threading through every store call.
- **`QueryProvisionedData`**: new route composing the real `ProvisionedDataSets` response from the
  already-existing `provisioned_data`/`lcs_privacy_data`/`lcs_mo_data`/`lcs_subscription_data`/
  `v2x_data`/`prose_data`/`odb_data`/`ee_profile_data`/`pp_profile_data`/`uc_data`/`mbs_data`/
  `pp_data`/`a2x_data`/`rangingsl_privacy_data` stores, same `wanted(name)` dataset-names-filter
  lambda pattern as `QueryUeSubscribedData`. Genuine improvement over that bare aggregate: here
  `servingPlmnId` is a REQUIRED path parameter (not an optional query param), so all 7 PLMN-keyed
  fields (`amData`/`smfSelData`/`smsSubsData`/`smData`/`traceData`/`smsMngData`/`lcsBcaData`) are
  ALWAYS composed, never skipped for lack of a serving-PLMN hint. `niddAuthData` is still never
  composed here, same real disclosed gap as ADR-0166: its own composite key requires
  `mtc-provider-information`, a query param this resource does not expose. The real, optional
  `adjacent-plmns`/`single-nssai`/`dnn`/`ext-group-ids`/`uc-purpose` query params are accepted but
  not honored, matching the established precedent. The real, optional `3gpp-Sbi-Etags` response
  header is NOT populated -- this store has no per-field ETag/versioning layer, a genuine,
  disclosed gap, not a fabricated header value. Same real `200`-always design as the other
  aggregates.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udr_gap_closure_212.cpp`, 2 tests, real spawned `nrf`+`udr` process
pair over real TLS 1.3 + mTLS:
- `IndividualAuthenticationStatusLifecycle`: real `404` before PUT, real `204` on PUT with a real
  `AuthEvent` body, real `200` + round-tripped body on GET, confirms the bare `authentication-status`
  resource for the same `ueId` stays independently `404` (proving the distinct-key design), real
  `204` on DELETE, real `404` again after DELETE.
- `QueryProvisionedDataComposesAllSeededFields`: against `nfs/udr/src/main.cpp`'s own real startup
  seed data for `imsi-999700000000001`/servingPlmnId `99970`, confirms every seeded field
  (PLMN-keyed and non-PLMN-keyed) is present with no filter, confirms `niddAuthData` is absent,
  confirms the `dataset-names=AM` filter returns only `amData`, confirms an unknown `ueId` returns
  a real `200 {}` (live view, no independent existence).

Both pass. A real, disclosed operational finding along the way, not a code bug: the long-running
dev `docker-postgres-udr-1` container (9 days up) only ever ran `schema.postgres.sql`'s `CREATE
TABLE` statements once, at its own creation -- the new `udr_individual_authentication_status`
table had to be applied to the live container directly (`CREATE TABLE IF NOT EXISTS`, idempotent,
matches the file) before the new test could pass; this is a known category of dev-environment
schema drift, not something this ADR's code changes could or should paper over.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly). Both new tests pass individually.
Full suite re-run clean: 432/432 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion; 6 further tests skipped via the existing
env-var-gated Postgres/product-catalog tests, same as every prior run). No strays before or after
any run (`ps aux` checked explicitly). One transient, unrelated CMake/CTest race caught and
retried, not a code issue: a `gtest_discover_tests` post-build step for `conformance_tests` hit
"text file is busy" against the just-linked binary on the first full rebuild attempt; re-running
`cmake --build build` succeeded cleanly with no code changes.

### What this ADR does NOT include

The audit doc's "`subs-to-notify` bulk-DELETE" claim is corrected in
`docs/CAPABILITY_GAP_ANALYSIS.md` as stale/inaccurate (no such operation exists in the real spec;
the resource is already fully implemented under task #122) rather than implemented -- there was
nothing real to build. UDR's `Policy_Data.yaml` Tier-B items (~12 undisclosed operations: bare
`{ueId}` aggregate, `sm-data/{usageMonId}` family, bare `bdt-data` collection GET, and a
`Policy_Data`-specific `subs-to-notify` family distinct from `Subscription_Data`'s own) remain
open, unchanged by this ADR. UDM's ~58+ undisclosed Tier-B operations remain the largest real item
still open project-wide.

