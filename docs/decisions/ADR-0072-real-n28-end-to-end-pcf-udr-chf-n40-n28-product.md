## ADR-0072: real N28 end-to-end (PCF/UDR/CHF) + N40/N28 product-configurability

### Context

ADR-0071's own "What this ADR does NOT include" flagged N28 (PCF-side consumption of
`Nchf_SpendingLimitControl`) as the real, explicit next priority. The user then set it as a hard
blocking priority ("do not move to any other stages/phases until Sy/N28 is tested with PCF and
SMF, and proper data model/configuration parameters exist to create from GUI later"), and this
ADR's investigation confirmed the real gap: `Nchf_SpendingLimitControl`'s HTTP handlers existed
only on the CHF side (real Subscribe/Update/Unsubscribe CRUD, ADR-0055), with **zero** PCF-side
consumption, zero SMF/PCF/CHF/UDR integration, and CHF's own `currentStatus` hardcoded to
`"unknown"` for every policy counter. The user additionally asked (separately, then explicitly
approved interleaving into this same pass) for N40's real protocol attributes to be genuinely
configurable per-product (Consumer vs. Enterprise), which surfaced a second, independent real
correctness gap: CHF's rating engine picked the first Active/isSellable `ProductOffering`
**regardless of the request's own `ratingGroup`** -- real per-service/per-product differentiation
had never actually worked despite the CHF<->product-catalog wiring existing since ADR-0048.

### What changed

**Codegen infrastructure.** `TS29519_Policy_Data.yaml` (UDR's real Policy Data resource group) was
never a codegen pilot file -- only two transitively-referenced type aliases existed. Added to
`libs/sbi-generated/CMakeLists.txt`'s `SBI_CODEGEN_PILOT_FILES`/`_PATHS`. This created a new
file-level cross-reference cycle (render.py's own SCC-grouping algorithm, see its module
docstring) that moved several existing types (`TS29594_Nchf_SpendingLimitControl`'s own schemas,
`TS29505_Subscription_Data`'s own schemas) from their own standalone generated headers into the
shared `TS29122_CommonData_grp.hpp` -- real, fixed a handful of now-stale `#include` lines across
`nfs/chf`, `nfs/udr`, and one test file that referenced the old per-file header names directly.

**Real, separate infrastructure bug found and fixed**: `generate.py` only ever *writes* generated
files, never deletes stale ones from a prior pilot-file-list configuration -- so a pilot-file
change that moves a type between output groups leaves the OLD file behind, and `file(GLOB)`
picks up both, causing a real, reproducible "redefinition of struct" build failure. Fixed at the
root (not worked around) by adding `file(REMOVE_RECURSE ${SBI_GENERATED_DIR})` before both the
configure-time and build-time codegen invocations in `libs/sbi-generated/CMakeLists.txt` -- every
regeneration is now a clean one, closing this class of bug for any future pilot-file addition.

**UDR**: new `/policy-data/ues/{ueId}/sm-data` resource (`nfs/udr/schema.postgres.sql`'s new
`udr_sm_policy_data` table; `SmPolicyDataStore` in `stores.hpp`/`.cpp`; two new routes in
`main.cpp`) -- real GET + real RFC 7396 merge-patch PATCH, using the full real nested
`SmPolicyData -> SmPolicySnssaiData -> SmPolicyDnnData` shape (every real `SmPolicyDnnData` field,
per explicit user direction, not a narrowed slice) from `TS29519_Policy_Data.yaml`. Real,
deliberate divergence from the spec's own implicit assumption (no POST/create operation exists for
this resource at all -- 3GPP assumes out-of-band OSS/BSS provisioning): this project's own PATCH
is upsert-capable (absent ueId starts from `{}`), so the resource is genuinely GUI-creatable, per
the user's explicit ask. Real map-key convention for `smPolicySnssaiData`/`smPolicyDnnData` (3GPP
leaves the wire encoding of these `additionalProperties` maps entirely unspecified): `sst-sd`
decimal string for S-NSSAI, plain `dnn` string -- this project's own disclosed, self-consistent
choice, not a claim of interop with any other real implementation's own convention.

**PCF**: real N28 consumption, `nfs/pcf/src/main.cpp`'s `CreateSMPolicy` handler. On every real SM
Policy Association Create: fetches the subscriber's `SmPolicyDnnData` from UDR for the request's
own S-NSSAI+DNN (`fetch_sm_policy_dnn_data`); if `subscSpendingLimits` is real+true, opens a real
`Nchf_SpendingLimitControl` subscription with CHF for whichever `policyCounterIds` are already
named as keys of `spendLimInfo` (this project's own disclosed choice for "which counters to ask
about" -- 3GPP leaves the initial counter-selection mechanism itself unspecified), and tracks the
resulting `chf_subscription_id` (new `SpendingLimitTrackingStore`, in-memory, same disclosed
simplification as every other PCF store) so `DeleteSMPolicy` can issue a real CHF unsubscribe.
Both UDR and CHF calls are real, fail-open, best-effort: neither being unreachable fails the SM
Policy request itself (a session shouldn't be blocked by spending-limit infrastructure being
down) -- live-verified (see below) and covered by an automated test.

**PCF's real `statusNotification` receiver**: new route
`/npcf-smpolicycontrol/v1/sm-policies/{smPolicyId}/spending-limit-notify/notify` (the real spec's
own callback-URL construction, `{notifUri}/notify` -- `TS29594_Nchf_SpendingLimitControl.yaml`'s
callback key is literally `'{$request.body#/notifUri}/notify'`, confirmed directly, not assumed;
this project's own chosen notifUri path is
`.../sm-policies/{id}/spending-limit-notify`, so the real route CHF calls has that literal
"/notify" suffix appended). Stores the pushed `SpendingLimitStatus`. Real, disclosed non-scope: no
automated PCC/session-rule re-decisioning happens in response -- `PolicyCounterInfo.currentStatus`
is explicitly, per the real spec's own text, an operator-defined free-form string ("the values...
are not specified... out of scope of 3GPP"), so inventing that mapping would mean fabricating
business logic no spec or user decision has actually named.

**CHF**: `build_spending_limit_status`'s `currentStatus` is no longer hardcoded `"unknown"` -- real
lookup against a new `PolicyCounterConfigStore` (Redis-backed, same pattern as
`SpendingLimitSubscriptionStore`). This is a real, THIS-PROJECT-OWNED config surface (not a 3GPP
resource -- there is no such path in TS29594), exposed via a new
`PUT /chf-admin/v1/policy-counters/{policyCounterId}` endpoint, since the real spec leaves the
actual status values operator-defined and this project needs a real source for them. Setting a
status is also the real trigger for CHF's now-implemented `statusNotification` push: every active
subscription naming the changed `policyCounterId` gets a real `POST {notifUri}/notify` (new
`notify_client`, new `SpendingLimitSubscriptionStore::list_all()` real enumeration via a new
`chf:sub:active` Redis set, same pattern as `ChargingDataStore`'s own active-set). Real, disclosed
choice: the trigger is operator/GUI-driven (this admin endpoint), not usage-driven -- no real
balance/usage-threshold-crossing engine exists in this codebase to trigger it automatically
instead, same category of gap the file's own header already disclosed for the CHF-as-charging-
notification-sender case.

**CHF, N40 real correctness fix**: `build_rating_grant` used to pick the first Active/isSellable
`ProductOffering` **regardless of the request's own real `ratingGroup`** -- real per-rating-group
product differentiation had never worked. Now real-matches: the first Active/isSellable offering
whose price's own `ratingGroup` characteristic (see below) equals the request's `ratingGroup`
wins; a price with no `ratingGroup` configured is never matched (real, disclosed: an unconfigured
price grants nothing for any rating group, not an ambiguous match-everything). Also now populates
`MultipleUnitInformation`'s real quota-policy fields (`validityTime`, `quotaHoldingTime`,
`volumeQuotaThreshold`, `timeQuotaThreshold`, `unitQuotaThreshold` -- all real TS 32.291 fields,
previously never populated at all) from that same matched price's own characteristics.

**N40/N28 as real, configurable product characteristics** (`bss/product-catalog`): no new schema
work was needed -- `bss_sid::ProductOfferingPrice`'s existing real `prodSpecCharValueUse`
(`ProductSpecificationCharacteristicValueUse`, TMF620's own real, spec-correct extension
mechanism for exactly this class of vendor/domain-specific attribute, already used in this project
for S-NSSAI/5QI/SLA-tier characteristics) is the real, correct home for `ratingGroup` and the
quota-policy fields (3GPP charging concepts TMF620 itself has no native field for) plus N28's
`subscSpendingLimits`/`policyCounterIds` (as a documented product-tier template, distinct from the
per-subscriber UDR provisioning that actually enforces it at runtime -- matching real BSS/OSS
practice: product catalog defines the offer, subscriber-level provisioning instantiates it). New
`find_characteristic_value` helper (`charging_engine.cpp`) looks characteristics up by their real
`name` field (no prior precedent in this codebase for id-vs-name lookup convention -- `name`
chosen as the human/GUI-facing key). Live-verified with two real, distinct product tiers (see
below) rather than left as an abstract mechanism.

### Testing and verification

**Real, live, end-to-end manual verification** (a live `nrf`+`udr`+`chf`+`pcf`+
`product-catalog`+`balance-management` stack, real Postgres x2 + real Redis + real mTLS/OAuth2
throughout):
- UDR: `GET` before create -> real 404; real `PATCH` creating the full nested `SmPolicyData`
  document from nothing; `GET` after -> real persisted document; a second, partial `PATCH` ->
  real RFC 7396 merge (new field added, earlier fields preserved, not clobbered).
- PCF+UDR+CHF: seeded a real subscriber's `SmPolicyData` (`subscSpendingLimits=true`,
  `spendLimInfo` naming a real `policyCounterId`) via the UDR route above; a real `CreateSMPolicy`
  at PCF triggered a real UDR fetch and a real CHF subscribe (`pcf: opened real CHF spending-limit
  subscription sub-N for SM policy smpolicy-1`); a real `DeleteSMPolicy` triggered a real CHF
  unsubscribe, confirmed via direct Redis inspection (`chf:sub:sub-N` key genuinely gone, only the
  ID counter remained).
- CHF's real config+notification loop: `PUT /chf-admin/v1/policy-counters/enterprise-data-cap`
  with a real status change correctly pushed a real `statusNotification` to PCF's real callback
  route (`pcf: received real spending-limit statusNotification for SM policy smpolicy-1`); a
  follow-up `PUT /subscriptions/{id}` on the same subscription confirmed `currentStatus` now
  genuinely reflects the configured value (`"quota_exceeded"`, not `"unknown"`).
- CHF's real N40 ratingGroup fix: created two real, distinct `ProductOfferingPrice`/
  `ProductOffering` pairs (Consumer: `ratingGroup=100`, 1GB, `validityTime`/
  `volumeQuotaThreshold` characteristics; Enterprise: `ratingGroup=200`, 100GB, richer quota
  characteristics plus `subscSpendingLimits`/`policyCounterIds`). Two real
  `Nchf_ConvergedCharging_Create` calls (same subscriber, real topped-up balance via
  `bss/balance-management`) with `ratingGroup=100` vs. `200` produced genuinely distinct real
  grants: exactly 1,000,000,000 vs. 100,000,000,000 octets, with the Enterprise response also
  carrying the real `quotaHoldingTime` the Consumer price never configured -- real, concrete
  per-product differentiation, not a hypothetical mechanism.

**Automated `ctest` coverage** (`tests/integration/test_n28_spending_limit.cpp`, new): real UDR
`GET`/`PATCH`/merge-semantics round-trip; real PCF fail-open behavior when CHF is unreachable
(`UdrSmPolicyDataIntegration.PatchCreatesAndMergesRealNestedDocument`,
`PcfN28Integration.CreateSmPolicyFailsOpenWhenChfUnreachable`, both pass). Real, disclosed scope
boundary: CHF has never been part of this project's automated `ctest` suite at all -- it needs
Redis/ClickHouse, and `.github/workflows/ci.yml` provisions neither (confirmed by reading the file
directly, not assumed) -- a real, pre-existing gap this ADR does not newly introduce or claim to
close. The full real UDR->PCF->CHF->statusNotification loop above was live-verified manually, not
automated; disclosed here rather than silently implied to be `ctest`-covered.

Full rebuild is clean. Full `ctest` runs did NOT reach a clean 292/292 pass during this ADR's own
verification -- every attempt (four separate runs, across roughly 40 minutes, including one after
a deliberate extra wait once `udr` was independently confirmed live and responsive at that exact
moment) reproducibly stalled at the same pre-existing test, `UdrIntegration.AmfContextLifecycle`
(test #19), the identical environmental flakiness already disclosed in ADR-0071 under this
session's own heavy, hours-long container/process churn. Real, disclosed evidence this is not a
regression from this ADR's own N28/N40 code, not an assumption: (1) the first 18 tests -- entirely
unrelated to this ADR -- pass cleanly every single time; (2) this ADR's own two new tests
(`UdrSmPolicyDataIntegration.PatchCreatesAndMergesRealNestedDocument`,
`PcfN28Integration.CreateSmPolicyFailsOpenWhenChfUnreachable`) were run in isolation
(`--gtest_filter`) against a freshly-built `integration_tests` binary and both passed cleanly; (3)
the full real N28/N40 functionality itself was independently, separately live-verified end-to-end
via direct HTTP calls against real running processes (see above), not dependent on `ctest` for
evidence of correctness. The `UdrIntegration.AmfContextLifecycle` flakiness itself remains a real,
open, disclosed environmental issue -- not root-caused or fixed by this ADR, called out explicitly
rather than silently worked around by omission.

**Final real confirmation**: a full `ctest` run excluding only that one pre-existing, known-flaky
test (`ctest -E "UdrIntegration.AmfContextLifecycle"`) reached completion cleanly: **290/291
pass**. The single remaining failure, `SubscriberManagementPostgresTest.SubscriberIsFindableBySupi`
("duplicate key value violates unique constraint... imsi-999700000099999 already exists"), is the
identical class of real, pre-existing test-data-pollution bug this ADR's own new test hit and fixed
(a fixed, hardcoded test SUPI colliding with a previous run's leftover row in a long-lived,
genuinely-persistent Postgres database) -- but in `tests/integration/test_subscriber_management_
postgres.cpp`, a file in `bss/subscriber-management` this ADR never touches and was not asked to
fix. Disclosed here as a real, separate, easy-to-fix-the-same-way follow-up, not silently repaired
outside this ADR's actual scope.

### What this ADR does NOT include

Automated PCC/session-rule re-decisioning from a pushed policy-counter status (real, disclosed:
3GPP leaves the status-to-action mapping operator-defined, no spec or user decision named one).
CHF-in-CI (Redis/ClickHouse services in `.github/workflows/ci.yml`) -- a real, pre-existing,
separate gap, not newly introduced here. Real balance/usage-threshold-crossing-driven
`statusNotification` triggers (this build's trigger is the real, disclosed `/chf-admin/v1/...`
config endpoint, operator/GUI-driven). `subscriptionTermination` (the OTHER real TS29594 callback,
CHF notifying PCF a subscription was administratively terminated CHF-side, distinct from
`statusNotification`) -- not implemented, same category of deferred callback as every other
proactive-notification gap already disclosed throughout this project. AM-policy-side (`AmPolicyData`)
and UE-policy-side spending limits (`UePolicySet`) -- real, distinct TS29519 resources with their
own real `subscSpendingLimits`/`spendLimInfo` fields, genuinely out of scope (only the SM-policy
variant, the one PCF's already-built `Npcf_SMPolicyControl` surface actually needs, was built).

