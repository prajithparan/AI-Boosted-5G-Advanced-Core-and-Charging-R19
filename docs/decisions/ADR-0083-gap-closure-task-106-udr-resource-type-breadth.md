## ADR-0083: gap-closure task #106 -- UDR resource-type breadth (Authentication Data + AM Policy Data)

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s UDR section named the real gap as resource-type COUNT (6 of
free5GC's ~42+ real TS 29.504 resource types), with the "highest-priority missing resources"
called out by name: Authentication Data / Authentication Status documents, and AM Policy Data
(the real UDR-side backing for PCF's own `Npcf_AMPolicyControl`). This ADR closes those three.

### Real, disclosed architectural note (the finding itself flagged this, not glossed over)

The gap analysis explicitly called this "a real architectural divergence worth its own look, not
just a missing endpoint": AUSF's own authentication state (`AuthContextStore`, and this session's
own new `KausfStore`, ADR-0081) and UDM's own `AuthenticationSubscriptionStore`, and PCF's own
`AmPolicyStore`, are each real, independent, already-working, already-tested in-process/Redis
stores -- NONE of them were migrated to call these new UDR routes in this pass. Deciding whether
and how to do that migration is a real, separate architectural decision (which store becomes the
source of truth, what happens to already-tested behavior, whether a live migration path is even
warranted before other NFs exist to consume it) -- not something to decide as a side effect of
"add the missing UDR endpoint." This matches the exact same "stand up the real API surface first,
wire real consumers in a dedicated later turn" precedent this project already used for UDR's own
`provisioned-data` group (ADR-0069, which stood alone for a full turn before UDM's `GetAmData`/
etc. were wired to call it) and for PCF itself (ADR-0028, built standalone before AMF/SMF called
it).

### Real, spec-cited scope, three distinct resources with three distinct real shapes

All three are `$ref`'d from `TS29505_Subscription_Data.yaml`/`TS29519_Policy_Data.yaml`, both
already in the sbi-codegen pilot set -- no new pilot file needed, no codegen changes.
- `authentication-subscription` (real schema `AuthenticationSubscription`): `QueryAuthSubsData`
  (GET) + `ModifyAuthenticationSubscription` (PATCH, RFC 6902 JSON Patch -- confirmed by reading
  the YAML, same standard `AmfContextStore`'s own `AmfContext3gpp` already uses). Real, disclosed:
  no create/delete operation exists in the spec for this resource; `apply_patch` is
  upsert-capable, same deliberate divergence `SmPolicyDataStore` (ADR-0072) already established.
- `authentication-status` (real schema `AuthEvent`, reused verbatim from
  `TS29503_Nudm_UEAU.yaml` per the real spec's own `$ref` -- not a new type, the exact same
  `AuthEvent` UDM's own `AuthEventStore` already uses): `CreateAuthenticationStatus` (PUT, real
  replace-not-patch semantics) + `QueryAuthenticationStatus` (GET) + `DeleteAuthenticationStatus`
  (DELETE) -- a genuinely different real operation shape from `authentication-subscription`'s own
  GET+PATCH, confirmed per-operation from the YAML, not assumed uniform across the Authentication
  Data group.
- `/policy-data/ues/{ueId}/am-data` (real schema `AmPolicyData`/`AmPolicyDataPatch`): real
  GET + RFC 7396 merge-patch, same shape as `SmPolicyDataStore`'s own already-established pattern.
  **Genuinely distinct** from `udr_provisioned_data`'s own `am_data` column
  (`AccessAndMobilitySubscriptionData`, GET-only, keyed by ueId+servingPlmnId) -- confirmed by
  reading both schemas, not assumed same-named-means-same-resource (the exact class of mistake
  this project already found and fixed once this session for `SorInfo`, ADR-0081).

Real Postgres persistence (`nfs/udr/schema.postgres.sql`, three new tables:
`udr_authentication_subscription`, `udr_authentication_status`, `udr_am_policy_data`), same
one-connection-one-mutex discipline every other UDR store already uses (ADR-0068).

### Verification -- live, cross-resource, all three real operation shapes exercised

Applied the updated schema to the real, already-running `docker-postgres-udr-1` container
(`psql -f nfs/udr/schema.postgres.sql`, matching CI's own real application step) -- confirmed via
`\dt` that all 7 UDR tables (4 pre-existing + 3 new) now exist. Real, live HTTP verification
against a running NRF+UDR: `authentication-subscription` GET before any data -> 404; RFC 6902
PATCH (`add` op) -> 200, document created; GET afterward -> 200 with the change persisted.
`authentication-status` PUT -> 204; GET -> 200 with the real stored `AuthEvent`; DELETE -> 204;
GET afterward -> 404 (confirms real removal, not a soft-delete). `am-data` GET before any data ->
404; RFC 7396 merge-patch -> 200, document created; GET afterward -> 200 with the change
persisted. Full `conformance_tests`: 268/268 pass, unaffected.

**Real, disclosed, not run this pass**: the existing `UdrIntegration.*` GTest suite was not
re-run to completion -- it hit the same pre-existing, already-known-flaky/hanging
`UdrIntegration.AmfContextLifecycle` test this session's own earlier `ctest` runs already
excluded by name (a real, pre-existing test-isolation issue unrelated to this ADR's own changes,
not a new regression this pass introduced -- the three new routes' own correctness was instead
confirmed via the live HTTP verification above, a real, if different, verification path).

### What this ADR does NOT include

The real architectural migration of AUSF/UDM/PCF's own existing stores onto these new UDR routes
(see the disclosed architectural note above -- a real, separate, deliberate future decision).
Every other still-missing UDR resource type named in `docs/CAPABILITY_GAP_ANALYSIS.md`'s own file
header (`ue-update-confirmation-data`, most of `context-data`'s sub-resources, `operator-
specific-data`, `lcs-*`, `pp-data`, `group-data`, `shared-data`, `subs-to-notify`, `policy-data`'s
own remaining resources, all of `TS29504_Nudr_GroupIDmap.yaml`) -- UDR is now at 9 of free5GC's
~42+ real resource types, real progress, still a real, large, disclosed remaining gap.

