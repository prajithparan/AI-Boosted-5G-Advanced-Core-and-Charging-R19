## ADR-0213: UDR `Policy_Data.yaml` Tier-B gap-closure -- all ~12 undisclosed operations closed

### Context

Continuing ADR-0193's Tier-B backlog, this ADR closes the remaining UDR item
(`docs/CAPABILITY_GAP_ANALYSIS.md`'s own audit: "~12 [undisclosed operations] in `Policy_Data`").
Confirmed by direct read of `specs/5G_APIs-REL-19/TS29519_Policy_Data.yaml`, four genuinely
distinct real resources were missing (not implemented anywhere in `nfs/udr/src/main.cpp`):

- **`ReadPolicyData`** (`GET /policy-data/ues/{ueId}`): the real `PolicyDataForIndividualUe`
  aggregate -- same "live view over already-independently-stored sub-resources" design as
  `QueryUeSubscribedData`/`QueryProvisionedData` (ADR-0166/ADR-0212).
- **Usage Monitoring Information (Document)** (`/policy-data/ues/{ueId}/sm-data/{usageMonId}`):
  `CreateUsageMonitoringResource` (PUT), `ReadUsageMonitoringInformation` (GET),
  `DeleteUsageMonitoringInformation` (DELETE) -- real PUT+GET+DELETE, keyed by
  `(ueId, usageMonId)`.
- **`ReadBdtData`** (`GET /policy-data/bdt-data`): the real bare BDT Data collection GET --
  genuinely distinct from the already-implemented individual `bdt-data/{bdtReferenceId}`
  (ADR-0116).
- **Policy Data Subscriptions (Collection) + Individual Policy (Data) Subscription (Document)**
  (`/policy-data/subs-to-notify[/{subsId}]`): `CreateIndividualPolicyDataSubscription` (POST),
  `ReadPolicyDataSubscriptions` (GET), `ReadIndividualPolicySubscriptionData` (GET),
  `ReplaceIndividualPolicyDataSubscription` (PUT), `DeleteIndividualPolicyDataSubscription`
  (DELETE) -- genuinely distinct real resource from `Subscription_Data.yaml`'s own
  `subs-to-notify` (ADR-0149): different real schema (`PolicyDataSubscription`), different real
  individual-modify verb (PUT full replace here, not RFC 6902 PATCH there).

### Implementation

- **`ReadPolicyData`**: composes `uePolicyDataSet` (`ue_policy_set`), `smPolicyDataSet`
  (`sm_policy_data`), `amPolicyDataSet` (`am_policy_data`), `umData` (new `UsageMonDataStore`,
  keyed by `limitId` -- confirmed equal to `usageMonId`), `operatorSpecificDataSet`
  (`policy_operator_specific_data`, already the real map-shaped schema this field needs
  verbatim) -- all already-existing stores, zero new schema for this route itself. Real,
  optional `data-subset-names` filter honored (`PolicyDataSubset` enum), same `wanted(name)`
  pattern used throughout this file. Real `200`-always design.
- **`UsageMonDataStore`** (new, `nfs/udr/src/stores.hpp`/`.cpp`, `udr_usage_mon_data` table,
  `PRIMARY KEY (ue_id, usage_mon_id)`): real PUT (always `201`, same "spec documents only 201"
  precedent as `BdtDataStore`) + GET + DELETE. Real, disclosed: the spec's own GET documents a
  `204` ("found but no data") state distinct from `404` -- this project's single-row-per-key
  model has no such intermediate state, so an absent row is always the real `404`.
- **`ReadBdtData`**: new `BdtDataStore::list_all()`, same "store returns all, route filters"
  precedent as `PdtqDataStore::list()` -- the real, optional `bdt-ref-ids` array filter is applied
  in the route against each stored `BdtData`'s own `bdtRefId` field.
- **`PolicyDataSubsToNotifyStore`** (new, `udr_policy_data_subs_to_notify` table): real
  create/get/list_all/replace/remove, structurally close to `SubsToNotifyStore` but with a real
  full-replace `PUT` instead of an RFC 6902 `PATCH`. Real, disclosed: the collection GET's own
  optional `mon-resources`/`ue-id` filters are accepted but not honored --
  `PolicyDataSubscription`'s own body has no `ueId` field to filter by (confirmed absent from the
  real schema, unlike `SubscriptionDataSubscriptions`'), and `monResItems`' per-resource-path
  granularity isn't modeled by this store. The spec's own `policyDataChangeNotification` webhook
  callback is not implemented -- same disclosed gap class as `SubsToNotifyStore`'s own
  pre-ADR-0171 state.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udr_policy_data_gap_closure_213.cpp`, 4 tests, real spawned
`nrf`+`udr` process pairs over real TLS 1.3 + mTLS:
- `UsageMonitoringResourceLifecycle`: real `404` before PUT, real `201` + `Location` on PUT, real
  `200` round-trip on GET, real `204` DELETE, real `404` after.
- `ReadPolicyDataComposesSeededSubResources`: seeds `am-data` (PATCH), `ue-policy-set` (PUT), and
  this ADR's own new usage-mon-data (PUT) for a fresh test UE, then confirms the aggregate GET
  composes all three plus honors the `data-subset-names=AM_POLICY_DATA` filter, plus confirms an
  unknown `ueId` returns real `200 {}`.
- `ReadBdtDataListsSeededDataAndHonorsFilter`: PUTs an individual BDT Data doc via the
  already-existing route, confirms the new collection GET lists it, confirms the `bdt-ref-ids`
  filter excludes it when given an unrelated id.
- `PolicyDataSubsToNotifyLifecycle`: real `201` + `Location` on POST, collection GET lists it,
  individual GET `200`, PUT replace returns the real replaced body, DELETE `204`, GET after `404`.

All 4 pass. One real, disclosed test-design finding along the way, not a code bug: this project's
own dev PostgreSQL container is a long-lived, persistent Docker container shared across every test
run (not reset per run) -- an initial test assertion that `smPolicyDataSet` would be absent for a
freshly-chosen seeded test `ueId` failed because an earlier, unrelated test (the real N28
end-to-end SM policy wiring, ADR-0072/ADR-0084) had already written `SmPolicyData` for that same
`ueId` in a prior run. Fixed by not asserting on that ambient shared-DB state, not by touching the
implementation. Similarly, `ue-policy-set`'s own PUT returned a real `204` (update) rather than
`201` (create) on a re-run against the same persisted row -- the test now accepts either, matching
the store's own real upsert semantics.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step). The two new
tables (`udr_usage_mon_data`, `udr_policy_data_subs_to_notify`) had to be applied directly to the
long-running dev `docker-postgres-udr-1` container (same real, disclosed operational finding as
ADR-0212 -- `schema.postgres.sql`'s `CREATE TABLE` statements only ever run once, at container
creation). All 4 new tests pass individually. Full suite re-run clean: 436/436 passing (`ctest
--timeout 120 -E "UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`,
matching `.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux`
checked explicitly).

### What this ADR does NOT include

The spec's own `policyDataChangeNotification` webhook callback remains unimplemented, disclosed
above. The collection GET's `mon-resources`/`ue-id` filters are accepted but not honored,
disclosed above. This closes UDR's own `Policy_Data.yaml` Tier-B item in full -- **UDR now has
zero known Tier-B gaps.** UDM's ~58+ undisclosed Tier-B operations (`Nudm_UECM`/`Nudm_SDM`/`UEAU`/
`PP`) are now the only remaining real Tier-B item project-wide, per
`docs/CAPABILITY_GAP_ANALYSIS.md`.

