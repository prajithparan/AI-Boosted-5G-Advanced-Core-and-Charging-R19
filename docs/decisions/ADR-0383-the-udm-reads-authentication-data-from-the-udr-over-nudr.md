## ADR-0383: The UDM reads authentication data from the UDR over Nudr -- onboarded subscribers can authenticate

**Date:** 2026-09-25. **Status:** accepted (user-directed: "proceed" with the next increment named in
ADR-0382). Closes ADR-0382's headline gap: a subscriber onboarded through `bss/provisioning` was
provisioned in the UDR but could not pass AKA, because the UDM generated vectors from its own
in-memory store holding two hardcoded TS 35.207 subscribers.

**Decision.** `udm::UdrAuthSubscriptionSource` (`nfs/udm/src/udr_auth_source.*`) replaces the
in-memory `AuthenticationSubscriptionStore`, keeping the same two operations and the same crypto:
- **Read:** `QueryAuthSubsData` -- GET `/nudr-dr/v2/subscription-data/{ueId}/authentication-data/
  authentication-subscription` (TS29505_Subscription_Data.yaml), on every vector request. K/OPc/AMF/
  SQN/method come from `encPermanentKey`/`encOpcKey`/`authenticationManagementField`/
  `sequenceNumber.sqn`/`authenticationMethod`. Nothing is cached in the UDM; neither the GET nor the
  PATCH response body (both carry K/OPc) is ever logged.
- **SQN write-back:** `ModifyAuthenticationSubscription` -- the resource's only write operation,
  RFC 6902 PATCH -- as a **compare-and-swap**: `[{op:test, path:/sequenceNumber/sqn, value:<as
  read>}, {op:replace, ...}]`. A lost race makes the UDR reject the whole patch (400); the UDM
  re-reads and retries (bounded, 8). The UDR's `apply_patch` now reads the row `FOR UPDATE`, so the
  swap is atomic across UDR replicas, not only within one process's mutex.
- **Multi-vector** (`hss-security-information/{hssAuthType}/generate-av`): one read + one swap that
  reserves N consecutive SQNs (vector i uses base+i -- the sequence N separate advances produced),
  instead of N round trips on the synchronous client.
- **Resync:** AUTS is verified in the UDM against the K/OPc just read; a genuine resync stores
  SQN_MS + 2^16 through the same swap (rule and rationale unchanged, ADR-0037).
- **Errors** now distinguish what they used to collapse into 404, using only statuses the
  Nudm_UEAU YAML documents for all four vector operations: no document -> 404; UDR unreachable /
  token failure / retries exhausted -> 503; a document without usable AKA material -> 500. No
  `cause` value is invented.
- **Seed moved, values unchanged:** the two TS 35.207 Test Set 1 subscribers (K, OPc *derived* from
  OP, AMF `b9b9`, SQN `ff9bb4d0b607` / `000000000000`, 5G_AKA / EAP_AKA_PRIME) are seeded by the UDR
  at startup (`udr` now links `aka_crypto` to derive OPc). SQNs reset on each UDR start (previously
  each UDM start).

**Rejected.**
- *Keep the in-memory store and have provisioning also push keys into the UDM* -- two sources of
  truth for K/OPc, and the UDM would need its own write API; TS 29.505 puts this data in the UDR.
- *Cache K/OPc in the UDM* -- key material held in two places; per-request reads are the simpler
  correct baseline (disclosed cost below).
- *Plain `replace` without `test`* -- two concurrent vector requests (or two UDM replicas) could hand
  out the same SQN, which a USIM rejects as a replay.
- *N round trips for N vectors* -- same result, N times the latency on the synchronous client.

**Disclosed.**
- Every vector request now costs a UDR GET + PATCH (plus an OAuth2 token, cached by the client).
  Correct, not yet measured; relevant to the ADR-0049 performance mandate.
- The UDR's Nudr routes still admit token-less requests (`check_bearer`, recorded in ADR-0382) --
  the SQN CAS relies on the PATCH route, which is not identity-restricted. Unchanged here.
- The CAS treats any 400 from the two-op patch as "lost the race"; a UDR that rejected the patch
  for another reason would be retried 8 times and then reported as 503.
- Still no full TS 33.102 Annex C windowing (ADR-0026/0037 rules kept as they were).

**Tests.** New `tests/integration/test_udm_ueau_from_udr.cpp` (real nrf + udr + udm, PostgreSQL,
mTLS): (1) a subscriber onboarded through `bss/provisioning`'s `UdrAdapter` gets a 5G-AKA vector
whose MAC-A, AUTN-embedded SQN, AMF, XRES* and KAUSF are recomputed UE-side from the ORDER's K/OPc,
and the advanced SQN is read back through Nudr; (2) 8 concurrent requests get 8 distinct,
consecutive SQNs; (3) unknown SUPI -> 404, no UDR -> 503. `test_udm_ueau`, `test_udm_ueau_gap_
closure_214`, `test_ausf_ue_authentication`, `test_ausf_upu_protection` now start a UDR (they reach
AKA); `spawn_guard.hpp` gained `wait_tcp_listening`. Verified locally 2026-09-25: the new suite
3/3; Udm*/Ausf*/Udr* 71/71 (after starting local Valkey and clearing the known stale
`udr_amf_context` row); AmfNgapTestGnb 10/10, including real UE registration and PDU session
establishment through AMF -> AUSF -> UDM -> UDR.

