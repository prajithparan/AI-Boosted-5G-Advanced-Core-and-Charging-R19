## ADR-0382: Customer onboarding reaches the UDR -- a project-owned OAM provisioning API, a resumable saga, and the auth gap it does not close

**Date:** 2026-09-25. **Status:** accepted (user-directed: "UDR provisioning API" and "K/OPc
supplied in the order" chosen explicitly over the alternatives below). Increment 2 of the customer
onboarding module (`bss/provisioning`); increment 1 wrote only the BSS SID chain and recorded the
UDR task as `pending`.

**The spec gap, verified against the frozen R19 YAML, not assumed.** Nudr has **no create
operation** for the data a new subscriber needs. `TS29505_Subscription_Data.yaml`:
`provisioned-data/am-data|sm-data|smf-selection-subscription-data` are GET-only;
`authentication-subscription` is GET + PATCH. `TS29519_Policy_Data.yaml`: `policy-data/ues/{ueId}/
am-data` and `sm-data` are GET + PATCH. 3GPP leaves subscriber creation to OSS/BSS; commercial UDRs
answer it with a vendor OAM provisioning interface.

**Decision.**
1. **UDR: `PUT /oam-provisioning/v1/subscribers/{ueId}`** (`nfs/udr/src/oam_provisioning.*`) --
   explicitly NOT a 3GPP API, deliberately outside every `nudr-*` root so it cannot be mistaken for
   one. The body's documents ARE the 3GPP schemas (AccessAndMobilitySubscriptionData,
   SessionManagementSubscriptionData, SmfSelectionSubscriptionData, AuthenticationSubscription,
   AmPolicyData, SmPolicyData; required members and patterns validated from the YAML); only the
   envelope and path are ours. One PostgreSQL transaction across the four tables the standard Nudr
   GETs read (`SubscriberProvisioningStore`), replace semantics, 201 first time / 204 on replace.
   Readability is proven through the standard Nudr GETs, not by querying tables.
2. **Access control by verified mTLS identity.** Every NF certificate chains to the one lab CA, so
   mTLS alone would let any NF write K/OPc. `sbi_core::http2::Request` now carries the verified peer
   certificate's CN and dNSName SANs (read once per connection after the handshake); the route
   admits only identities on `config/udr.json` `oam_provisioning_allowed_clients` (default
   `["provisioning"]`) and answers 403 ProblemDetails otherwise. No OAuth2 bearer: the provisioning
   service is not an SBI consumer and is not registered with the NRF.
3. **`bss/provisioning` becomes a resumable saga.** One `provisioning_task` per node (`bss-charging`,
   then `udr-subscription`); the order state is derived from the tasks (`completed` only when all are
   done, else `failed` + 502 listing each task's status/attempts/lastError). Resending the same order
   (same `idempotencyKey`) resumes every task not `done` -- increment 1 returned early on any existing
   order row, so a failed task could never be retried. Task ids are keyed by order id, not SUPI.
4. **SIM keys are supplied in the order** (`sim.k`, `sim.opc`, optional `sim.sqn`), as they arrive
   from the SIM vendor's input file in real operations; provisioning validates, never generates.
   They are never logged, never stored in `request_payload`/`response`/`last_error`, never returned
   by GET; parse errors are replaced with a fixed message because `json::parse_error` text can quote
   the input. A resumed order must resend them.
5. **Network content is config, not code** (`config/provisioning.json`): `udr_base_url`,
   `serving_plmn_id`, `authentication` {`method`, `management_field`, `initial_sqn`} and
   `network_profiles` keyed by product offering id with an explicit `default`; the task response
   records which profile applied.

**Rejected.**
- *Provisioning writes straight into the UDR's PostgreSQL* (the free5GC/Open5GS web-console
  pattern) -- couples BSS to the UDR's internal schema and breaks single-owner-per-DB.
- *Using the existing upsert-capable PATCH routes* (authentication-subscription, policy sm/am-data
  already create a document when none exists, ADR-0072/0083) -- there is still no path for
  provisioned-data, four separate calls are not atomic, and those routes are not identity-restricted.
- *A separate UDR listener + dedicated provisioning CA* -- also correct, but needs a second CA in
  `gen-lab-pki.sh`, CI and compose; the per-request identity is ~30 lines in sbi-core and reusable by
  any later route that must admit exactly one client.
- *Provisioning generates K/OPc* -- not how SIMs work (keys come from SIM manufacturing).

**Disclosed -- stubs, simplifications, non-conformance.**
- **An onboarded SUPI still cannot authenticate.** UDM's 5G-AKA/EAP-AKA' vector generation reads
  K/OPc/SQN from its own in-memory store seeded with two TS 35.207 test SUPIs
  (`nfs/udm/src/main.cpp`), not from the UDR over Nudr. Closing that (UDM `QueryAuthSubsData` GET +
  SQN write-back via `ModifyAuthenticationSubscription` PATCH) is the next increment; until then the
  onboarding flow is "provisioned", not "registrable".
- `encPermanentKey`/`encOpcKey` hold K/OPc in clear hex with no `protectionParameterId` -- the lab has
  no key-protection (HSM / transport-key) scheme. Not presented as encryption.
- Only IMSI-form SUPIs are accepted (the YAML's Supi also allows nai-/gci-/gli-).
- sm-data is one SessionManagementSubscriptionData for the profile's first S-NSSAI;
  `dnnConfigurations` and `subscribedSnssaiInfos` are left unset, the same floor the seeded
  subscribers carry. AmPolicyData is `{}` (nothing product-derived yet).
- Network profiles are config-keyed by offering id; their proper home is the TMF620
  ProductSpecification's characteristics.
- No compensation (UDR delete) on a later task failure -- with two tasks and BSS first, a UDR failure
  leaves BSS provisioned and the order `failed`/resumable, which is the intended state. A DELETE on
  the OAM API arrives with the first task that can fail after UDR (CHF/balance, NSSF).
- Existing finding, not fixed here: every UDR Nudr route's `check_bearer` gate is
  `auth.has_value() && !auth->valid`, i.e. a request with **no** token is admitted; combined with the
  upsert-capable PATCH routes, any mTLS peer can create/modify authentication-subscription over
  Nudr today. Recorded for the security backlog (`docs/SECURITY_COMPLIANCE.md` scope), not widened
  into this increment.

**Tests.** `tests/integration/test_udr_oam_provisioning.cpp` (CI): adapter documents pass the UDR
validator (contract); rejection text names fields but never echoes K/OPc; CN/SAN allow-list; real
nrf + udr + PostgreSQL over mTLS -- `hello-nf` identity refused 403, bad body 400, provision 201/204
then resend 204, and the subscriber read back through the standard Nudr GETs (am-data, sm-data,
authentication-subscription, policy-data sm-data/am-data). The saga itself (orchestration + charging
DBs) is not provisioned in CI; it was verified locally 2026-09-25 against the lab databases with real
nrf/udr/provisioning processes: (1) an unknown offering fails the BSS task on the real FK, order
`failed`, UDR task left `pending`; (2) UDR stopped -> BSS `done`, UDR `failed` ("UDR unreachable"),
order `failed`; UDR restarted + same order resent -> BSS not re-run (attempts 1), UDR `done`
(attempts 2), order `completed`, 201, subscriber readable via Nudr am-data and policy sm-data;
(3) invalid SIM -> 400 without echoing the value; (4) the test K appears in no process log and in no
`provisioning_task` column.

**Also fixed in this change (pre-existing, found while building):** the connection-pool commits
(`bss: connection pool ...`) added a required `pool_size` to `ProductOfferingStore`,
`ProductOfferingPriceStore`, `ProductSpecificationStore` and `BalanceStore`, but three integration
tests still used the two-argument constructors, so `integration_tests` no longer compiled. They now
pass `pool_size=1`.

