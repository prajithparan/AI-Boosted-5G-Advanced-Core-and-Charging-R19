## ADR-0028: PCF (seventh NF) -- Npcf_AMPolicyControl + Npcf_SMPolicyControl

**Date:** 2026-08-07
**Status:** Accepted

**Context:** Seventh and, per the originally-agreed Phase 2 build order (NRF -> AMF -> SMF -> UDM
-> UDR -> AUSF -> PCF), final NF in that list. Source: `specs/5G_APIs-REL-19/
TS29507_Npcf_AMPolicyControl.yaml` + `TS29512_Npcf_SMPolicyControl.yaml`, commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`, added as new codegen pilot files this turn.

**Scope agreed with the user before implementation:** the two services CLAUDE.md's stated Phase 2
end goal actually needs -- `Npcf_AMPolicyControl`'s `CreateIndividualAMPolicyAssociation`
(`POST /policies`), `ReadIndividualAMPolicyAssociation` (`GET /policies/{polAssoId}`),
`DeleteIndividualAMPolicyAssociation` (`DELETE /policies/{polAssoId}`),
`ReportObservedEventTriggersForIndividualAMPolicyAssociation`
(`POST /policies/{polAssoId}/update`); and `Npcf_SMPolicyControl`'s `CreateSMPolicy`
(`POST /sm-policies`), `GetSMPolicy` (`GET /sm-policies/{smPolicyId}`), `UpdateSMPolicy`
(`POST /sm-policies/{smPolicyId}/update`), `DeleteSMPolicy` (`POST /sm-policies/{smPolicyId}/
delete`).

**Deliberately deferred, not dropped:** both services' callback notifications (`PolicyUpdate`/
`TerminationNotification`, `SmPolicyNotification`, pushed BY PCF TO the `notificationUri` AMF/SMF
supply) -- neither AMF nor SMF has a receiver for these, same shape as every other proactive/
callback flow this build has deferred (AMF's own N1N2, UDM's SDM subscriptions, etc. never got a
PCF-side push implemented either). `Npcf_PolicyAuthorization` (AF/Rx-style), `Npcf_UEPolicyControl`
(URSP), `Npcf_EventExposure`, `Npcf_BDTPolicyControl`, `Npcf_PDTQPolicyControl`,
`Npcf_AMPolicyAuthorization`, `Npcf_MBSPolicyControl`/`Authorization` -- separate PCF sub-services,
out of scope for the core registration/PDU-session flows. Most importantly: **wiring AMF/SMF to
actually call this PCF** -- this turn stands up PCF's own API surface + tests standalone, same
precedent as UDR's turn (ADR-0025) and UDM's Nudm_UEAU turn (ADR-0026, before AUSF called it in
ADR-0027) -- a deliberate future turn touching already-committed AMF/SMF code, reviewable on its
own, not silently done or silently skipped.

**Disclosed simplification, stated up front:** real PCF policy decisions are computed from
subscriber data UDR would hold (via `Npcf`'s own UDR client for the policy-data group), which
UDR's turn (ADR-0025) never implemented (UDR's `provisioned-data` group is GET-only with nothing to
provision through the API at all). So `CreateSMPolicy`/`UpdateSMPolicy` here return one default
`SessionRule` per SM Policy: the request's own `subsSessAmbr`/`subsDefQos` when the caller supplies
them (so the decision at least reflects what was actually asked for), falling back to a fixed
1 Gbps/1 Gbps session AMBR and 5QI 9 (non-GBR, the one number in this default actually drawn from a
real TS 23.501 5QI table) otherwise. The ARP priority level used in that fallback (8) is an
arbitrary placeholder with **no** spec citation -- disclosed both in `nfs/pcf/src/main.cpp`'s file
header and again inline at the point it's constructed, not just here, since it's the one field with
zero spec backing among otherwise-real defaults. `UpdateSMPolicy` similarly just re-derives the same
decision from the originally-stored context rather than doing any real trigger-driven
re-evaluation of the reported `SmPolicyUpdateContextData` -- there is no policy engine behind this,
by design, for the reason above.

**`SmPolicyUpdateContextData` is an opaque `nlohmann::json` fallback, not hand-modeled:** confirmed
by inspecting the generated header, not assumed -- its schema is an `allOf` with a `not` keyword
tools/sbi-codegen doesn't handle (documented opaque-fallback pattern, same category as
`AuthenticationVector`'s `oneOf`+`discriminator` fallback from ADR-0026's UDM turn). `UpdateSMPolicy`'s
handler accepts it as raw parsed JSON rather than requiring it to match a typed DTO -- and since
this build's decisioning doesn't actually branch on the reported triggers anyway (see the
simplification above), nothing is lost by not having a typed shape for it this turn.

**Cross-file `$ref` closure merged AM/SM Policy Control's types into the same giant
`TS29122_CommonData_grp.hpp` as everything else, confirmed not assumed:** `PolicyAssociation`,
`SmPolicyContextData`, etc. did NOT land in their own `TS29507_*.hpp`/`TS29512_*.hpp` files the way
earlier single-service NFs' types did -- `chfInfo`/`sliceReplReq`/`uePolFailReport` and friends
transitively `$ref` into `TS29512`/`TS29534`/`TS29525`/`TS29522`/`TS29520`/... forming one large
cross-file reference cycle that tools/sbi-codegen's existing cycle-merge logic (ADR-0010) folds
into the same merged group nearly every other NF's common types already live in. Not a new
generator bug -- same documented behavior UDM's Nudm_UEAU turn already exercised (ADR-0026, the
UEAU+imsUEAU merge), just a much larger cycle this time. `nfs/pcf/src/main.cpp` includes
`TS29122_CommonData_grp.hpp` for this reason, same as most other NFs already do.

**Decision:** implemented as `nfs/pcf` (port 7783, metrics 9470, `kAmApiRoot =
"/npcf-am-policy-control/v1"`, `kSmApiRoot = "/npcf-smpolicycontrol/v1"`), following the same
structural pattern as every prior standalone-turn NF: NRF registration/heartbeat on a background
thread, OAuth2 JWT verification against NRF's fixed `kNrfInstanceId` (ADR-0018), TLS 1.3 + mTLS via
the shared lab PKI (new `certs/pcf/` leaf cert), Prometheus metrics, ProblemDetails error handling.
`AmPolicyStore`/`SmPolicyStore` (`nfs/pcf/src/stores.hpp`) hold plain `nlohmann::json` rather than
generated structs -- both `PolicyAssociation` and `SmPolicyControl` are dozens-of-optional-fields
DTOs that every route handler already builds/reads as full JSON, so a typed store would just be a
second, redundant place these fields could drift from the wire format (same reasoning as UDM's
`SdmSubscriptionStore`/`AuthEventStore`). `CreateSMPolicy` returns just the `SmPolicyDecision`
(HTTP 201 body, per the YAML) while `GetSMPolicy` returns the full `SmPolicyControl`
(`{context, policy}`, per the YAML) -- two different response shapes for the same underlying stored
resource, confirmed from the spec, not assumed to be symmetric.

**Verification:** manual `curl` end-to-end (recorded in `docs/TRACEABILITY.md`) plus 4 new real
subprocess-to-subprocess integration tests (`tests/integration/test_pcf_policy_control.cpp`,
spawning real `nrf`+`pcf`, real TLS 1.3 + mTLS, real signed JWT -- PCF plays no client role this
turn per the deferred-AMF/SMF-wiring decision above, so these tests act as the AMF/SMF role
directly, same as every NF's own tests before its real caller existed): a full AM Policy
Association lifecycle (create, get, report-triggers-and-update confirming the update is really
persisted not just echoed, delete, 404 after delete, 404 on re-delete); a full SM Policy lifecycle
where the request supplies its own `subsSessAmbr`/`subsDefQos` and the returned decision reflects
those exact values, not the fixed fallback; a second SM Policy case with neither field supplied,
confirming the decision falls back to the documented 1 Gbps/5QI-9 defaults; and the 404/401 error
paths. All 56 project tests pass, stable across repeated runs. Docker image built and verified in
this environment (`docker build -f deploy/docker/pcf.Dockerfile -t 5gc-pcf:test .`); `deploy/
docker/docker-compose.yml` gained a `pcf` service; `deploy/helm/pcf/` mirrors `deploy/helm/ausf/`,
including the same disclosed cross-chart shared-PKI gap. `docker compose up` and `helm install`/
`helm template` were NOT run this session -- same disclosed-not-silently-assumed gap ADR-0014
already recorded for NRF alone.

**Rejected alternative:** build a small real policy-decision engine (e.g. actually branching SM
policy on `pduSessionType`/`sliceInfo`/whether the UE requested a specific DNN) to make the
defaults feel less arbitrary. Rejected because there is no real subscriber policy data behind any
of it either way (UDR's policy-data group doesn't exist -- see the disclosed simplification above)
-- a more elaborate-looking decision function fed by nothing but the request itself would be
*more* misleading than a small, honestly-labeled fixed default, not less; the honest minimal
version is easier to audit and to replace wholesale once UDR-backed policy data is real.

**Consequence:** this turn completes Phase 2's originally-agreed seven-NF list
(NRF/AMF/SMF/UDM/UDR/AUSF/PCF) with every NF's own API surface standing up standalone and tested.
What remains before "UE registration (TS 23.502 §4.2.2.2.2) and PDU session establishment
(§4.3.2.2.1) end-to-end" (CLAUDE.md's stated Phase 2 finish line) is real, not stubbed, is entirely
inter-NF wiring: AMF calling PCF for AM policy, SMF calling PCF for SM policy (this ADR's deferred
list), AMF calling AUSF for authentication (a real flow, ADR-0027's AUSF already built the
callee side), UDM/UDR wiring (ADR-0025's deferred list), and AMF/SMF's own N2/N4 termination
(NGAP/PFCP, both explicitly out of scope for the SBI-only build so far). None of that is resolved
by this turn -- flagged here as the actual shape of what's left, not assumed away.

---

