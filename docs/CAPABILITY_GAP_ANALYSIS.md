# Capability gap analysis vs free5GC and open5GS

**Status: first pass COMPLETE for all 9 built NFs, code-free review gate** (same pattern as
`docs/CHARGING_MAPPING.md`) -- user-directed, full sweep of every built NF against both real
reference implementations, procedure/behavior-level (not literal line-by-line -- free5GC is Go,
open5GS is C, this project is C++, so "line by line" is interpreted as exhaustive
capability/behavior comparison, the real cross-language equivalent). Governed by ADR-0075's
capability-completeness mandate: every real finding here is tracked to eventual implementation,
none silently dropped. Nothing has been implemented yet -- this file is the evidence base an
implementation plan gets built from. `nssf`/`nef`/`scp`/`bsf` (Tier-1 NFs that don't exist in this
project at all yet) are a separate, larger "whole NF missing" gap, out of this sweep's scope, not
blended into the per-NF findings below. See the Summary section at the end for the
cross-NF priority picture.

## Method

For each of this project's built NFs (`nrf, amf, ausf, chf, pcf, smf, udm, udr, upf` --
`nssf/nef/scp/bsf` don't exist yet in this project at all, a separate "whole NF missing" gap
tracked at the end, not blended into per-NF findings below), real source was pulled directly:
- **free5GC**: cloned live from `github.com/free5gc/<nf>.git` (the `free5gc-main.zip` umbrella
  repo only contains empty git-submodule pointers, no real source -- confirmed by inspection,
  each real NF repo cloned individually as needed).
- **open5GS**: extracted from the user-supplied `open5gs-main.zip`, a real, full monorepo
  (`src/<nf>/`).

Every finding below cites the real file/line evidence it came from. Nothing is asserted without
having actually read the cited source.

## NRF

### Real endpoint/service-surface comparison

| Service | free5GC | open5GS | This project |
|---|---|---|---|
| `Nnrf_NFManagement` (Register/Get/Update/Deregister/GetInstances) | Real (`internal/sbi/api_nfmanagement.go`) | Real (`src/nrf/nnrf-handler.c`) | Real, matches route-for-route (`nfs/nrf/src/main.cpp`) |
| `Nnrf_NFDiscovery` (SearchNFInstances) | Real | Real | Real |
| `Nnrf_AccessToken` (OAuth2 token) | Real | Real | Real |
| `Nnrf_NFManagement` Subscriptions (Create/Update/Remove) | Real | Real | Real |
| `Nnrf_Bootstrapping` | **Stub only** -- free5GC's own `HTTPBootstrappingInfoRequest` returns HTTP 501 Not Implemented (`internal/sbi/api_bootstrapping.go`) | Not found | Not implemented | Not a real gap -- free5GC itself doesn't implement this either. |

### Real gaps found (this project is missing real behavior both/one reference has)

1. **`NFProfile` semantic validation was missing -- closed, docs/DECISIONS.md ADR-0079.** free5GC
   has ~290 lines of real validation (`internal/sbi/processor/nf_profile_validation.go`):
   `nfInstanceId` must be a real UUID v4, `heartBeatTimer` bounds-checked, `nfType`/`nfStatus`/
   `nfServiceStatus`/`uriScheme` checked against real enum sets, `ipv4Addresses`/`ipv6Addresses`
   format-validated, `ipEndPoint.transport` must be TCP, port range-checked. This project's NRF
   previously only checked that `nfInstanceId`/`nfType`/`nfStatus` keys were **present** -- no
   format or enum validation at all, so a malformed `nfType` or an out-of-range `heartBeatTimer`
   was silently accepted. Now real: every constraint above is enforced, each independently
   grounded in the actual OpenAPI YAML (cited per-field in `nfs/nrf/src/main.cpp`'s own comment,
   not copied from free5GC's own choices) -- live-verified via real HTTP 400 rejections for each
   violation class.

2. **No active heartbeat-expiry timer -- closed, docs/DECISIONS.md ADR-0079.** open5GS runs a
   real per-NF-instance timer (`src/nrf/nf-sm.c:187-216`, `t_no_heartbeat`, started on entry to
   the `registered` state for `heartbeat_interval + no_heartbeat_margin`) that proactively
   deregisters an NF instance if it stops sending heartbeats, firing a real de-registration
   notification to subscribers. This project's NRF previously accepted `PATCH` heartbeats but had
   no background expiry sweep at all -- a crashed NF that never sent `DELETE` stayed registered
   forever. free5GC does not appear to have this either (only stores the `HeartBeatTimer` value,
   `internal/context/management_data.go:62`) -- this was specifically an open5GS capability this
   project lacked, not a universal gap. Now real: `NfRegistry::sweep_expired`, a periodic
   background sweep (this project's own design, not a byte-for-byte port of open5GS's per-NF
   timer object) -- live-verified end-to-end, including that a `PATCH` heartbeat genuinely resets
   the expiry window, not just that expiry fires at all.

3. **Subscription `subscrCond` filtering is ignored.** open5GS validates and applies
   `subscrCond` (an `nfType`-or-`serviceName` XOR filter, `src/nrf/nnrf-handler.c:481-603`, citing
   its own real upstream bug fix, issue #2630: "must be `oneOf`"). This project's own code already
   self-discloses this exact gap (`nfs/nrf/src/main.cpp:18`: "Subscription notification fan-out
   ignores `subscrCond` (delivers to every active subscriber...)"). **Already known, now
   independently corroborated, not a new finding** -- listed here for completeness of the record.

### Not yet checked for NRF (deferred to a follow-up pass if worth it)

NRF-to-NRF federation/forwarding (multi-NRF deployments), `searchOptions` completeness in
discovery (free5GC/open5GS support a large set of optional discovery query filters -- this
project's own discovery filter completeness was not compared field-by-field this pass), rate
limiting / TPS protection (already known-absent everywhere per ADR-0049, not re-derived here).

## AMF

**Real scale context, checked before diving in**: this project's AMF is 3,116 lines
(`nfs/amf/src/*.cpp/.hpp`); free5GC's is 43,058 lines (Go, `internal/{gmm,ngap,sbi}/`); open5GS's
is 32,982 lines (C, `src/amf/`). Roughly a 10-14x difference -- and the findings below show why:
this project's AMF covers a real but narrow slice of the full NGAP/GMM procedure set, not a
roughly-equivalent implementation with a few missing edges.

### Namf_* SBI services

| Service | free5GC | This project |
|---|---|---|
| `Namf_Communication` (UEContext CRUD, N1N2MessageTransfer(+subscribe), non-UE-N2-messages(+subscribe)) | Real (`internal/sbi/processor/{ue_context,n1n2message}.go`) | Real, route-for-route match (`nfs/amf/src/main.cpp`, 16 routes) |
| `Namf_EventExposure` (Create/Delete/Modify subscription -- mobility, reachability, comm-failure, location-report events, TS 29.518) | Real (`processor/event_exposure.go`, 3 real handlers) | **Real, all 6 real operations** (individual + AMF-Set-level bulk families, ADR-0199) -- real subscription CRUD, real RFC 6902 `PATCH`; notification delivery not implemented (disclosed, same gap class as this NF's other subscription types) |
| `Namf_Location` (ProvideLocationInfo -- used by LMF/GMLC for positioning) | Real (`processor/location_info.go`) | **Real, all 3 real operations** (ADR-0199) -- `ProvidePositioningInfo` real `501` (no LPP/GNSS/PRU capability, disclosed), `ProvideLocationInfo` real 404/honestly-empty-200, `CancelLocation` real `404` |
| `Namf_MT` (ProvideDomainSelectionInfo -- CS/PS domain selection for MT SMS/call) | Real (`processor/mt.go`) | **Real, all 3 real operations** (ADR-0200) -- `ProvideDomainSelectionInfo` real 404/honestly-empty-200, `EnableUeReachability` real 404/real-ack, `EnableGroupReachability` real 200 with honestly-empty `ueConnectedList` |
| `Namf_OAM` (RegisteredUEContext query) | Real (`processor/oam.go`) | **Missing entirely** |

Grep-confirmed: no occurrence of `namf-oam` or its real operation names anywhere in this project's
AMF source. `Namf_EventExposure`/`Namf_Location` closed real, ADR-0199; `Namf_MT` (plus
`Namf_AIoT`/`Namf_MBSBroadcast`/`Namf_MBSCommunication`, real services this project's own R19
archive carries that aren't in free5GC's comparison set at all) closed real, ADR-0200. **1 of the
5 real Namf_* services free5GC implements remains entirely unimplemented**: `Namf_OAM` does NOT
exist as a file in this project's `specs/5G_APIs-REL-19/` at all (checked directly, `ls`+`grep`,
ADR-0199's pass) -- either free5GC's `Namf_OAM` maps to a TS 29.518 operation this project's own
R19 archive genuinely doesn't carry, or it's a 3GPP internal/non-published interface; not yet
root-caused which, so not silently assumed out of scope.

### Real NAS (GMM) procedure coverage

free5GC's `internal/gmm/handler.go` implements 17 real GMM procedure handlers (grep-confirmed,
`func Handle*`): `ULNASTransport`, `RegistrationRequest` (dispatching to real
`InitialRegistration`/`MobilityAndPeriodicRegistrationUpdating` sub-procedures),
`IdentityResponse`, `NotificationResponse`, `ConfigurationUpdateComplete`, **`ServiceRequest`**,
`AuthenticationResponse`, `AuthenticationError`, `AuthenticationFailure`,
`RegistrationComplete`, `SecurityModeComplete`, `SecurityModeReject`, `DeregistrationRequest`
(UE-initiated), `DeregistrationAccept` (network-initiated ack), `Status5GMM`. Backed by a real,
explicit GMM state machine (`internal/gmm/sm.go`: `DeRegistered`, `Registered`,
`DeregisteredInitiated`, plus `Authentication`/`SecurityMode` sub-states, `internal/gmm/init.go`).

This project's `nas_codec.hpp` decodes/handles: `RegistrationRequest`, `AuthenticationOutcome`,
`SecurityModeComplete`, `RegistrationComplete`, `ULNASTransport`, and (closed, docs/DECISIONS.md
ADR-0076) **`ServiceRequest`/`ServiceAccept`/`ServiceReject`**, backed by a real, persistent
(Redis-backed) NAS security context (`UeSecurityContextStore`) keyed by 5G-GUTI/5G-TMSI, so it
survives across NG associations rather than living only in per-association memory as every other
procedure here still does. Grep-confirmed still absent, entirely: `IdentityResponse`,
`NotificationResponse`, `ConfigurationUpdateComplete`, `AuthenticationError` (distinct from
`AuthenticationFailure` -- resync vs. abort), `SecurityModeReject`,
`DeregistrationRequest`/`DeregistrationAccept`, `Status5GMM`. No explicit GMM state machine
exists in this project -- registration/mobility state is tracked, but not as a named, real RM/CM
state machine matching TS 24.501's own model.

**`ServiceRequest`'s absence was the single highest-impact finding in this whole analysis --
closed as of ADR-0076.** It is the dominant real NAS procedure for CM-IDLE -> CM-CONNECTED
transitions (a UE resuming from idle after paging, or UE-triggered uplink data resume) -- in real
network traffic it fires far more often than full Registration. Real-interop-verified for the
RegistrationAccept/GUTI-assignment/RegistrationComplete side (a full UERANSIM run through PDU
Session Establishment); the decode path itself (peek-TMSI-then-verify-MAC) is verified by 6 new
hand-built-genuine-message unit tests, not by interop, since no reachable trigger for a real
`ServiceRequest` from UERANSIM was found this pass (see the NGAP `UEContextRelease` finding
below). What `ServiceRequest` does NOT yet do: drive real N2 PDU Session Resource Setup for any
PDU session its own `uplinkDataStatus` reports pending -- that requires SMF's own `UpdateSMContext`
N2SmInfo dispatch, still a gap (see next section).

### Real NGAP (N2) procedure coverage

free5GC's `internal/ngap/handler.go` implements ~39 real NGAP procedure handlers (grep-confirmed,
`func handle*Main`), including the full real N2 handover suite (`HandoverRequired`,
`HandoverRequestAcknowledge`, `HandoverFailure`, `HandoverNotify`, `HandoverCancel`,
`PathSwitchRequest`), `NGReset`/`NGResetAcknowledge`, `UEContextRelease{Request,Complete}`,
`InitialContextSetup{Response,Failure}`, `UEContextModification{Response,Failure}`,
`PDUSessionResource{Setup,Modify,Release}Response`, `PDUSessionResourceNotify`,
`RRCInactiveTransitionReport`, `RANConfigurationUpdate`, `ErrorIndication`, `CellTrafficTrace`,
NRPPa transport (positioning) relay, and more.

This project's `ngap_task.cpp` handles 7 real procedures (grep-confirmed, `void handle_*`):
`NGSetupRequest`, `InitialUEMessage`, `UplinkNASTransport` (+ 2 internal sub-variants for
SMC-complete and PDU-session-establishment relay), and (closed, docs/DECISIONS.md ADR-0078)
`UEContextReleaseRequest`/`UEContextReleaseComplete` (RAN-initiated direction only). Grep-confirmed
still absent from `ngap_codec.hpp` entirely -- not even encodable/decodable, not just unhandled:
`Handover*`, `PathSwitchRequest`, `NGReset`, `InitialContextSetup*`.

**This project's AMF has zero N2 handover support** -- no `HandoverRequired`, no
`PathSwitchRequest`, nothing. A UE cannot be handed over between gNBs in this implementation
today; only initial attach + PDU session establishment on a single gNB is real. This is a
structural gap, not a missing edge case. **Not addressed by ADR-0076 or ADR-0078** (those passes
closed `ServiceRequest`/GMM and `UEContextRelease` specifically, not this NGAP-side gap) -- still
mostly open, tracked as the remaining scope of task #100/#101.

**Partial closure, ADR-0090**: `PathSwitchRequest`/`PathSwitchRequestAcknowledge`/
`PathSwitchRequestFailure` (the AMF-facing tail of Xn-based handover, TS 38.413 §8.4.4) are now
real and live-verified -- a genuine, previously-missing `amf_ue_ngap_id -> tmsi` cross-association
index and real TS 33.501 Annex A.9/A.10 vertical key derivation (KgNB/NH) were built along the
way. See ADR-0090 for the full real, disclosed scope (including a real, found-in-passing correction
to `ngap_task.hpp`'s own pre-existing "one thread per association" claim -- confirmed at the time
to be single-association-at-a-time, sequential; that specific limitation is what ADR-0095 below
fixes for real).

**Closed, ADR-0095/ADR-0096**: the real N2-based handover chain (`HandoverRequired`/
`HandoverRequest`/`HandoverRequestAcknowledge`/`HandoverCommand`/`HandoverPreparationFailure`/
`HandoverFailure`/`HandoverNotify`) is now real and live-verified with two genuinely,
simultaneously-open gNB associations -- the real architectural prerequisite this remaining scope
needed (`run_ngap_lifecycle`'s accept loop was strictly sequential, confirmed above; ADR-0095
rearchitected it to one real `std::thread` per association plus a new `GnbAssociationRegistry` for
real cross-thread relay/correlation). Live-verified against a real, unmodified UERANSIM
registration plus two hand-crafted scratch gNB clients, including a genuine, unplanned bonus
confirmation: UERANSIM's own real gNB correctly processed a real, new AMF-initiated
`UEContextReleaseCommand` this same pass added (closing the exact gap ADR-0078 disclosed as the
"AMF-INITIATED direction... not implemented" below). Real, disclosed remaining gap:
`HandoverCancel`/`HandoverCancelAcknowledge` (a separate elementary procedure, not part of the
closed chain), and `HandoverRequest`'s own per-session PDU transfer content is real but carries a
placeholder N3 address (no real AMF->SMF relay built for handover, same disclosed class as
`PathSwitchRequest`'s own still-open "AMF doesn't call SMF yet" gap two rows below).

**Real NGAP gap found via live interop, closed the same way it was found**: attempting to
trigger a real `ServiceRequest` naturally (gNB-initiated idle-mode re-entry via UERANSIM's
`nr-cli <gnb> --exec 'ue-release <id>'`) sent a real NGAP `UEContextReleaseRequest` this AMF could
not decode at all -- `amf-ngap: failed to decode NGAP PDU (25 bytes), ignoring: 002a4015...`.
Closed as of ADR-0078: real `UEContextReleaseRequest`/`UEContextReleaseCommand`/
`UEContextReleaseComplete` (RAN-initiated direction), live-verified via the identical interop
scenario -- gNB's own `ue-list` went from one entry to empty, UE transitioned to CM-IDLE, real
NGAP bytes exchanged both directions. The AMF-initiated `UEContextRelease` direction (an AMF
deciding on its own to release, e.g. after Deregistration) remains unimplemented, disclosed as a
real, deliberate scope boundary in ADR-0078, not silently dropped.

### Real finding, not yet checked further

Whether this project's simpler mobility tracking (no explicit RM/CM state machine) produces
behaviorally-correct results for the procedures it DOES implement was not re-verified in this
pass -- this section is about coverage (what's present vs. absent), not re-auditing correctness
of what's already built and already has its own tests.

---

## AUSF

**Scale context**: this project's AUSF is 676 lines vs free5GC's 3,317 (Go) vs open5GS's 2,173
(C) -- a smaller ratio than AMF's, and the findings match: the core service is fully covered.

### `Nausf_UEAuthentication` -- real, full parity

free5GC's real routes (`internal/sbi/api_ueauthentication.go`): `POST /ue-authentications`
(initiate), `PUT .../5g-aka-confirmation`, `DELETE .../5g-aka-confirmation`, `POST
.../eap-session`, `DELETE .../eap-session`. This project's AUSF (`nfs/ausf/src/main.cpp`)
implements all five, route-for-route, plus one additional real route,
`POST /ue-authentications/deregister`. **No gap on the core service** -- both 5G-AKA and
EAP-AKA' paths, confirmed already live-tested in this project's own integration tests
(`AusfIntegration.FiveGAkaSuccessfulAuthentication...`,
`AusfIntegration.EapAkaPrimeSuccessfulAuthentication...`).

### Real gaps found -- both free5GC-only, not shared with open5GS

1. **`Nausf_SoRProtection`** (`POST /{supi}/ue-sor`, real TS 33.501 Annex A.17/A.18 (originally
   miscited as "Annex E.2" -- corrected and independently verified against a real local copy of
   TS 33.501 v19.6.0 before implementation) -- protects Steering-of-Roaming (SoR) list/CMCI data
   against tampering by a compromised VPLMN). Real in free5GC (`internal/sbi/api_sorprotection.go`).
   **Grep-confirmed absent from open5GS's AUSF too** -- free5GC-only capability. **Closed, docs/
   DECISIONS.md ADR-0081**: real SoR-MAC-IAUSF/SoR-MAC-IUE crypto (`libs/aka-crypto`), a new
   persistent per-SUPI `KausfStore` (the real architectural prerequisite this needed), and the
   real CounterSoR freshness-counter state machine including wrap-around suspension -- all
   live-verified cross-process. Real, disclosed scope narrowing: only the "SOR header supplied by
   requester" branch is implemented (AUSF constructing its own header per TS 24.501 §9.11.3.51 is
   a different spec section not in hand); the structured-array form of the optional steering-info
   parameter is also out of scope for the same reason.
2. **ProSe (Proximity Services / D2D) authentication** (`POST /prose-authentications`, `DELETE
   .../prose-auth`, a real R17+ extension). Real in free5GC. **Grep-confirmed absent from
   open5GS's AUSF** as well -- same free5GC-only status as SoR protection. **Closed, docs/
   DECISIONS.md ADR-0091**: real TS 33.503 Annex A.2/A.3/A.4 CP-PRUK/CP-PRUK-ID*/KNR_ProSe
   derivations (`libs/aka-crypto`), a new UDM `GenerateProseAV` route reusing the existing
   EAP-AKA' Milenage path, a new AUSF `ProSeAuthContext` store, all live-verified cross-process
   (a hand-crafted UE-role client independently recomputed RES/K_aut from real TS 35.207 Test Set
   1 public vectors). Real, disclosed narrower scope than free5GC's: only the first-time/new-
   CP-PRUK path is built -- the `5gPrukId`-based returning-UE path and CP-PRUK's own real
   cross-session persistence both need a live PAnF (ProSe Anchor Function) this project doesn't
   have (a whole separate, unbuilt NF), and return a real, disclosed `501 Not Implemented`.

Per ADR-0075's capability-completeness mandate, both are real gaps to close eventually regardless
of being free5GC-only -- "superior to both, never behind either" doesn't stop at whichever
reference happens to have less.

---

## SMF

**Scale context**: this project's SMF is 2,320 lines vs free5GC's 23,422 (Go) vs open5GS's 38,138
(C) -- roughly 10-16x, the same order as AMF, and for the same underlying reason: the Create path
is real, the Update path is largely a stub.

### `Nsmf_PDUSession` -- Create and Release real, Update is the gap

free5GC's real top-level procedures (`internal/sbi/processor/pdu_session.go`):
`HandlePDUSessionSMContextCreate` (334 lines), `HandlePDUSessionSMContextUpdate` (852 lines --
by far the largest single procedure in the file), `HandlePDUSessionSMContextRelease`,
`HandlePDUSessionSMContextLocalRelease`. This project's SMF (`nfs/smf/src/main.cpp`) implements
all four route-for-route, and **its own header comments already self-disclose** the real gap:
"`UpdateSMContext` still does NOT call PCF's `UpdateSMPolicy`... acknowledges (204) without
fabricating `SmContextUpdatedData` content" (`main.cpp:25,46`). Confirmed by reading free5GC's
852-line `HandlePDUSessionSMContextUpdate`: it's a real dispatcher over a large, genuine set of N2
SM info sub-procedures (grep-confirmed, `models.N2SmInfoType_*`/`HoState_*` cases):
`PDU_RES_SETUP_{REQ,RSP,FAIL}`, `PDU_RES_MOD_{REQ,RSP,IND,CFM}`, `PDU_RES_REL_{CMD,RSP}`,
`PATH_SWITCH_{REQ,REQ_ACK,SETUP_FAIL}`, `HANDOVER_REQUIRED`. This project's `UpdateSMContext` is,
today, effectively a no-op ack -- **none of these sub-procedures exist**.

This directly couples to AMF's own finding above (zero N2 handover NGAP support): even if SMF
computed real updated N2 SM info for a handover, AMF has no NGAP handler to carry
`PATH_SWITCH_REQ`/`HANDOVER_REQUIRED` in the first place. The two gaps are the same real
capability (N2 handover) viewed from each NF's own side, not two independent gaps.

**Partial closure, ADR-0092**: `PATH_SWITCH_REQ`/`PATH_SWITCH_REQ_ACK` are now real -- `UpdateSMContext`
gained real multipart parsing, decodes the real NGAP `PathSwitchRequestTransfer`, issues a real
PFCP Session Modification creating this project's first-ever real downlink PDR/FAR (with a real
TS 29.244 §8.2.56 Outer Header Creation), and returns a real `PathSwitchRequestAcknowledgeTransfer`
carrying UPF's own real N3 uplink F-TEID. A real, previously-undiscovered UPF bug (false-positive
`RequestAccepted` on an unrecognized `CreatePDR`/`CreateFAR`) was found and fixed along the way.
Live-verified, three-way corroborated (SMF log, UPF log, and independent decode of the real HTTP
response). Real, disclosed scope: AMF's own `PathSwitchRequest` handler does not call this
endpoint yet (deferred, separate relay-wiring piece); the downlink PDR's match criteria is
`SourceInterface`-only (this project has no real UE IP allocation anywhere, a real, deeper,
separate gap found while scoping this ADR); the new FAR's `OuterHeaderCreation` is real at the
PFCP control-plane level only, not wired into the real eBPF/XDP datapath (no downlink GTP-U
encapsulation path exists there). ~~The other 20 real `N2SmInfoType` values (`PDU_RES_*`,
`HANDOVER_*`, ...) remain unreal~~ -- **CLOSED, ADR-0259 + ADR-0260**; ADR-0092's own
disclosures (downlink PDR matches `SourceInterface=Core` only, no UE IP allocation anywhere, no
real eBPF/XDP downlink encapsulation) are unchanged and now live in the shared
`install_downlink_far` helper.

### Real, further, not-yet-fully-characterized findings (flagged, not yet fully drilled down)

- **ULCL (Uplink Classifier) / multi-homed PDU sessions** (R16 local-breakout/edge-computing
  feature): free5GC has a dedicated `internal/sbi/processor/ulcl_procedure.go`. Grep-confirmed no
  equivalent concept anywhere in this project's SMF. Real gap, not yet measured for size/depth.
  Whether open5GS has an equivalent was not yet checked in this pass.
- **PFCP/N4 procedure breadth**: free5GC's `internal/pfcp/handler/handler.go` +
  `internal/context/pfcp_{rules,reports,session_context}.go` suggest a real, fuller N4 session
  report/rule surface than this project's own UPF-side implementation (ADR from gap-closure Tier
  1d covers Create/Modify/Delete + QER/BAR, but a direct procedure-by-procedure PFCP diff against
  both references was not done in this pass -- flagged as owed, not yet delivered).
- Charging trigger integration (`internal/sbi/processor/charging_trigger.go`) -- this project's
  own N40/CHF wiring is real and already tested (unlike free5GC's, this project's charging system
  is comparatively the more built-out side per the CHF brief), so this is flagged for a
  behavior-level diff, not assumed to be a gap in either direction.

---

## PCF

**Scale context**: this project's PCF is 961 lines vs free5GC's 9,557 (Go) vs open5GS's 6,472
(C).

### Real service surface comparison

free5GC's real PCF processors (`internal/sbi/processor/`): `ampolicy.go`
(`Npcf_AMPolicyControl`), `smpolicy.go` (`Npcf_SMPolicyControl`), `bdtpolicy.go`
(`Npcf_BDTPolicyControl`), `policyauthorization.go` (`Npcf_PolicyAuthorization`), plus a real
`api_uepolicy.go` (`Npcf_UEPolicyControl`). This project's PCF (`nfs/pcf/src/main.cpp`) implements
`Npcf_AMPolicyControl` and `Npcf_SMPolicyControl` in full (including the real N28 spending-limit
loop from ADR-0072/-0073) -- **this project's own code already self-discloses the rest as
deferred** (`main.cpp:17-18`): "`Npcf_PolicyAuthorization` (AF/Rx-style), `Npcf_UEPolicyControl`
(URSP), `Npcf_EventExposure`, `Npcf_BDTPolicyControl`, `Npcf_PDTQPolicyControl`,
`Npcf_AMPolicyAuthorization`..." -- these are not new findings, but this pass adds real,
verified priority evidence for them:

| Service | free5GC | open5GS | Priority signal |
|---|---|---|---|
| `Npcf_PolicyAuthorization` | Real (`policyauthorization.go`) | Real -- confirmed via `OGS_SBI_RESOURCE_NAME_APP_SESSIONS` in `npcf-handler.c` (1,686 lines), the real TS 29.514 resource name | **Closed, docs/DECISIONS.md ADR-0080.** Both references implement this; this project's own `nfs/pcf/src/main.cpp` now does too (`PostAppSessions`/`GetAppSession`/`ModAppSession`/`DeleteAppSession`/`updateEventsSubsc`/`DeleteEventsSubsc`/`PcscfRestoration`, all live-verified). Real, disclosed gap remaining: no PCC-rule engine to authorize a requested media flow against -- returns a schema-correct, non-fabricated "authorized" outcome (no `ServAuthInfo` failure code), not real subscriber-specific decisioning. |
| `Npcf_UEPolicyControl` (URSP) | Real (`api_uepolicy.go`) | Grep-confirmed absent (no URSP/UE-policy resource name found in open5GS's PCF source) | free5GC-only. Still owed per ADR-0075, lower relative priority than PolicyAuthorization. |
| `Npcf_BDTPolicyControl` | Real (`bdtpolicy.go`) | Grep-confirmed absent | free5GC-only. |
| `Npcf_EventExposure` | Not found in free5GC's own processor directory either | Not checked in depth | Neither reference clearly implements this -- lowest priority of the self-disclosed list, pending a closer look. |

### Not yet checked for PCF

Real PCC rule richness (QoS flow granularity, charging-rule interaction depth) within the SM
policy path that IS implemented -- this pass compared service-surface presence, not full
behavioral depth of the already-built `Npcf_SMPolicyControl`/`Npcf_AMPolicyControl` paths.

---

## UDM

**Scale context**: this project's UDM is 1,520 lines vs free5GC's 8,409 (Go) vs open5GS's 4,409
(C).

### Real service surface comparison

free5GC's real UDM processors: `subscriber_data_management.go` (`Nudm_SDM`),
`ue_context_management.go` (`Nudm_UECM`), `generate_auth_data.go` (`Nudm_UEAU`),
`event_exposure.go` (`Nudm_EE`), `parameter_provision.go` (`Nudm_PP`). This project's UDM
(`nfs/udm/src/main.cpp`) implements the three core services in real, solid depth: `Nudm_UECM`
(AMF 3GPP-access registration + deregistration, SMF registration full CRUD), `Nudm_SDM`
(am-data/smf-select-data/sm-data + subscription CRUD), `Nudm_UEAU` (generate-auth-data,
auth-events) -- route-for-route matching free5GC's own core surface, already covered by this
project's own live-verified integration tests.

**Real gap, entirely missing, both references -- closed, docs/DECISIONS.md ADR-0082**: `Nudm_EE`
(Event Exposure -- `CreateEeSubscription`/`UpdateEeSubscription`/`DeleteEeSubscription`, real
subscription-based UDM event notifications) and `Nudm_PP` (Parameter Provisioning -- the real
`Get`/`Update` `pp-data` operation the analysis named). Both free5GC-only (grep-confirmed absent
from open5GS's UDM too), still real gaps per ADR-0075, now closed and live-verified. Real,
newly-discovered additional `Nudm_PP` scope found while implementing this (not in the original
finding above): three larger, more specialized resource groups -- `/5g-vn-groups/{extGroupId}`
(5G LAN/VN group CRUD), `/{ueId}/pp-data-store/{afInstanceId}` (PP Data Entry CRUD),
`/mbs-group-membership/{extGroupId}` (5G MBS group CRUD) -- flagged for a future turn, not built
or silently dropped.

---

## UDR

**Scale context**: this project's UDR is 963 lines vs free5GC's 9,547 (Go) vs open5GS's 2,400 (C).
UDR is architecturally a wide-but-shallow data repository (TS 29.504) -- the real gap here is
resource-type COUNT, not per-resource complexity.

### Real resource-type coverage

free5GC's real UDR (`internal/sbi/processor/`) has **42 distinct resource-document/collection
processor files**, the real TS 29.504 `Nudr_DR` data model in full: Application Data (influence
data + subscriptions, PFD data), Exposure Data (event exposure group subscriptions), Policy Data
(AM policy data, SM policy data), Subscription Data (AM data, SMF-selection data, SM data, SMS
management/subscription data, PDU-session-management data, session-management-subscription data,
AMF 3GPP/non-3GPP access registration, SMF registration(s), SMSF 3GPP/non-3GPP registration,
authentication data/status/SoR, trace data, query-identity-by-supi-or-gpsi, query-ODB-data,
operator-specific-data-container, shared-data retrieval), and PP (Parameter Provisioning) data.

This project's UDR (`nfs/udr/src/main.cpp`) implements 86 real resource endpoints (78 real
`Nudr_DataRepository` resources -- including, as of ADR-0167, the bare `5g-vn-groups`/
`mbs-group-membership` collection GETs, as of ADR-0168, their own `/internal` variants, and as of
ADR-0169, their own `/pp-profile-data` singleton variants -- plus, as of ADR-0120/ADR-0164, two
real `Nudr_GroupIDmap` resources (`GetRoutingIDs` and `GetNfGroupIDs` -- a genuinely distinct Nudr
API), plus, as of ADR-0170, the real `nf-group-ids/subscriptions`
POST/GET/PATCH/DELETE subscription-management family (four more real `Nudr_GroupIDmap`
endpoints), plus, as of ADR-0165/ADR-0166, `GetNiddAuData` and the bare `QueryUeSubscribedData`
aggregate -- none of the `Nudr_GroupIDmap` or aggregate resources counted in the
`Nudr_DataRepository`-vs-free5GC comparison below, the former
because it's a real distinct API and the latter because it composes entirely from resources
already individually
counted): AMF 3GPP-access
context-data, AMF non-3GPP-access context-data (docs/DECISIONS.md ADR-0093), SMSF 3GPP-access and
non-3GPP-access context-data (ADR-0097 -- see below), IP-SM-GW Registration context-data
(ADR-0098 -- see below), Message Waiting Data (Document) context-data (ADR-0099 -- see below),
Roaming Information (Document) context-data (ADR-0100 -- see below), PEI Information (Document)
context-data (ADR-0101 -- see below), Enhanced Coverage Restriction Data (ADR-0102 -- see below),
LCS Privacy Subscription Data (ADR-0103 -- see below), LCS Subscription Data (ADR-0104 -- see
below), LCS Mobile Originated Subscription Data (ADR-0105 -- see below), Parameter Provision
(Document) (ADR-0107 -- see below), Parameter Provision profile Data (Document) (ADR-0108 -- see
below), Provisioned Parameter Data Entry / pp-data-store (ADR-0109 -- see below), individual
Shared Data (ADR-0110 -- see below, first non-per-UE UDR resource), Operator-Specific Data
Container (Document) (ADR-0111 -- see below), Event Exposure Data (Document) (ADR-0112 -- see
below), UE Policy Set (ADR-0113 -- see below), policy-data Operator-Specific Data (ADR-0114 --
see below), Sponsor Connectivity Data (ADR-0115 -- see below, second non-per-UE UDR resource),
individual BDT Data (ADR-0116 -- see below, richest policy-data resource yet), PLMN UE Policy Set
(ADR-0117 -- see below, keyed by plmnId rather than ueId), Slice-specific Policy Control Data
(ADR-0118 -- see below, real GET+PATCH-only, upsert-capable merge-patch), group-specific Policy
Control Data (ADR-0119 -- see below, same GET+PATCH-only upsert-capable shape, plain-string key),
NIDD Authorization Info context-data (ADR-0121 -- see below, real PUT+GET+PATCH+DELETE, a
self-correction of an earlier bundled deferral), Identity Data by SUPI or GPSI (ADR-0122 -- see
below, real GET+PATCH, RFC 6902, upsert-capable), ODB Data (ADR-0123 -- see below, real GET-only,
seeded at startup), V2X Subscription Data (ADR-0128 -- see below, real GET-only, genuinely NOT
part of the provisioned-data group, keyed by ueId alone), ProSe Service Subscription Data
(ADR-0129 -- see below, real GET-only, genuinely NOT part of the provisioned-data group, keyed by
ueId alone), User Consent Subscription Data (ADR-0130 -- see below, real GET-only, genuinely NOT
part of the provisioned-data group, keyed by ueId alone), Time Synchronization Subscription Data
(ADR-0131 -- see below, real GET-only, genuinely NOT part of the provisioned-data group, keyed by
ueId alone, real required fields unlike most other GET-only resources closed), UE's Location
Information (Document) (ADR-0133 -- see below, real GET-only, genuinely NOT part of the
provisioned-data group, keyed by ueId alone), A2X Subscription Data (ADR-0134 -- see below, real
GET-only, genuinely NOT part of the provisioned-data group, keyed by ueId alone), Ranging and
Sidelink Positioning Privacy Subscription Data (ADR-0135 -- see below, real GET-only, genuinely
NOT part of the provisioned-data group, keyed by ueId alone, optional field-selection query
parameter not honored), Ranging and Sidelink Positioning Service Subscription Data (ADR-0136 --
see below, real GET-only, genuinely NOT part of the provisioned-data group, keyed by ueId alone),
5MBS Subscription Data (Document) (ADR-0137 -- see below, real GET-only, genuinely NOT part of
the provisioned-data group, keyed by ueId alone), Service Specific Authorization Info (Document)
context-data (ADR-0139 -- see below, real PUT+GET+PATCH+DELETE, composite (ueId, serviceType)
key), Group Identifiers mapping resource (ADR-0140 -- see below, real GET-only, genuinely NOT
per-UE, first real group-data sub-resource closed), NSSAI update ack (Document) resource
(ADR-0141 -- see below, real PUT+GET, no create-vs-update distinction, first real
ue-update-confirmation-data sub-resource closed), CAG update ack (Document) resource (ADR-0142
-- see below, real PUT+GET, identical shape to NSSAI update ack), Authentication SoR (Document)
and Authentication UPU (Document) resources (ADR-0143 -- see below, sor-data real PUT+GET+PATCH
including a real RFC 6902 PATCH, upu-data real PUT+GET only -- a genuine asymmetry between two
otherwise same-shaped siblings, closes all four ue-update-confirmation-data sub-resources
surveyed to date), group-data individual 5G VN Group Configuration resource (ADR-0144 -- see
below, real GET+PUT+PATCH+DELETE, PUT documents only 201, PATCH real RFC 6902, second real
group-data sub-resource closed after group-identifiers), group-data individual 5G MBS Group
Membership resource (ADR-0145 -- see below, structurally identical to the 5G VN Group
Configuration resource, third real group-data sub-resource closed), group-data Event Exposure
Data for a group resource (ADR-0146 -- see below, real GET-only, seeded, distinct sibling of the
per-UE ee-profile-data resource, fourth real group-data sub-resource closed), aggregate UE Update
Confirmation Data resource (ADR-0147 -- see below, real GET-only, composed live from the four
already-closed individual sub-resources rather than a new table, always 200 not 404), Event
Exposure Subscriptions collection + individual document (ADR-0148 -- see below, real collection
GET+POST plus individual GET+PUT+PATCH+DELETE, server-generated subsId, update-only PUT,
corrects ADR-0122's own blanket "deeply-nested" characterization of this specific resource), Subs
To Notify collection + individual document (ADR-0149 -- see below, real collection GET+POST
filtered by a real non-array ue-id query param, individual GET+PATCH+DELETE with genuinely no
PUT, webhook delivery deliberately not built), SDM Subscriptions collection + individual document
(ADR-0151 -- see below, structurally identical to Event Exposure Subscriptions, completes
correcting ADR-0122's original bundled deferral of both resources), AMF Subscription Info
(Document) nested under an individual ee-subscription (ADR-0152 -- see below, real
GET+PUT+PATCH+DELETE, array-valued document, real distinct 201-vs-204 PUT, first of
ee-subscriptions' own nested sub-collections closed), SMF Event Subscription Info (Document)
nested under an individual ee-subscription (ADR-0153 -- see below, real GET+PUT+PATCH+DELETE,
single-object document unlike its array-valued sibling, second of ee-subscriptions' own nested
sub-collections closed), HSS Subscription Info (Document) nested under an individual
ee-subscription (ADR-0154 -- see below, real GET+PUT+PATCH+DELETE, single-object document,
real disclosed spec inconsistency resolved per user confirmation, third and final of
ee-subscriptions' own nested sub-collections closed), HSS SDM Subscription Info (Document)
nested under an individual sdm-subscription (ADR-0155 -- see below, real GET+PUT+PATCH+DELETE,
single-object document, real 204-only PUT unlike its ee-subscriptions-nested siblings, same
disclosed spec inconsistency resolved via the ADR-0154 precedent, sdm-subscriptions' own final
deferred nested sub-collection closed), Event Exposure Group Subscriptions collection +
individual document, group-data-scoped (ADR-0156 -- see below, the group-data-scoped sibling of
ee-subscriptions keyed by ueGroupId instead of ueId, same real fix for a disclosed
Location-header bug also found and fixed in ee-subscriptions/sdm-subscriptions), AMF Group
Subscription Info (Document), group-data-scoped (ADR-0157 -- see below, the group-data-scoped
sibling of ee-subscriptions/{subsId}/amf-subscriptions, array-valued document, real distinct
201-vs-204 PUT; this ADR also fixes the same Location-header bug project-wide across ~20 routes,
not just the 3 already fixed in ADR-0156), SMF Event Group Subscription Info (Document),
group-data-scoped (ADR-0158 -- see below, the group-data-scoped sibling of
ee-subscriptions/{subsId}/smf-subscriptions, single-object document, real distinct 201-vs-204
PUT, second of group-data's own nested sub-collections closed), HSS Event Group Subscription
Info (Document), group-data-scoped (ADR-0159 -- see below, the group-data-scoped sibling of
ee-subscriptions/{subsId}/hss-subscriptions, single-object document, real distinct 201-vs-204
PUT, real disclosed spec inconsistency resolved with no genuine ambiguity, third and final of
group-data's own nested sub-collections closed -- completes the whole group-data
nested-subscription tree), Context Data (Document), aggregate resource (ADR-0161 -- see below,
real GET-only, unblocked by this project's own first real `style: form, explode: false`
array-query-param parsing infra, `sbi_core::http2::split_form_array()` -- a live-composed view
over 11 already-existing sub-resource stores, same design as `ue-update-confirmation-data`
[ADR-0147]), PDTQ Data collection + individual document (ADR-0162 -- see below,
`TS29519_Policy_Data.yaml`, the first UDR resource confirmed genuinely unblocked -- not merely a
candidate -- by ADR-0161's infra, real GET/PUT/PATCH/DELETE, `pdtqReferenceId` client-supplied,
`CreateIndividualPdtqData` documents only 201 matching the pre-existing `bdt-data` precedent,
real RFC 7396 merge-patch),
SMF-registrations context-data (full CRUD,
`{pduSessionId}`-scoped), provisioned-data (`am-data`, `smf-selection-subscription-data`,
`sm-data`, -- ADR-0106 -- `lcs-bca-data`, -- ADR-0125 -- `sms-mng-data`, -- ADR-0126 -- `sms-data`, and -- ADR-0127 -- `trace-data`), and the real nested `policy-data/ues/{ueId}/sm-data` resource from ADR-0072
(`SmPolicyData` with full `SmPolicySnssaiData -> SmPolicyDnnData` nesting and RFC 7396 merge-patch
semantics -- genuinely more complete for THIS one resource than a bare CRUD document, per that
ADR's own real, deliberate design). What's covered is solid; the real gap is breadth -- roughly 53
of free5GC's ~42+ real resource types (9 as of ADR-0083, 10 as of ADR-0093, 12 as of ADR-0097, 13
as of ADR-0098, 14 as of ADR-0099, 15 as of ADR-0100, 16 as of ADR-0101, 17 as of ADR-0102, 18 as
of ADR-0103, 19 as of ADR-0104, 20 as of ADR-0105, 21 as of ADR-0106, 22 as of ADR-0107, 23 as of
ADR-0108, 24 as of ADR-0109, 25 as of ADR-0110, 26 as of ADR-0111, 27 as of ADR-0112, 28 as of
ADR-0113, 29 as of ADR-0114, 30 as of ADR-0115, 31 as of ADR-0116, 32 as of ADR-0117, 33 as of
ADR-0118, 34 as of ADR-0119, 35 as of ADR-0121, 36 as of ADR-0122, 37 as of ADR-0123, 38 as of
ADR-0125, 39 as of ADR-0126, 40 as of ADR-0127, 41 as of ADR-0128, 42 as of ADR-0129, 43 as of
ADR-0130, 44 as of ADR-0131, 45 as of ADR-0133, 46 as of ADR-0134, 47 as of ADR-0135, 48 as of
ADR-0136, 49 as of ADR-0137, 50 as of ADR-0139, 51 as of ADR-0140, 52 as of ADR-0141, 53 as of
ADR-0142, 54 as of ADR-0143, 55 as of ADR-0144, 56 as of ADR-0145, 57 as of ADR-0146, 58 as of
ADR-0147, 59 as of ADR-0148, 60 as of ADR-0149, 61 as of ADR-0151 (ADR-0150 was a CHF-only CI fix,
no UDR resource change), 62 as of ADR-0152, 63 as of ADR-0153, 64 as of ADR-0154, 65 as of
ADR-0155, 66 as of ADR-0156, 67 as of ADR-0157, 68 as of ADR-0158, 69 as of ADR-0159 (ADR-0160 was
a research-only survey, no UDR resource change), 70 as of ADR-0161, 71 as of ADR-0162 (ADR-0164
was a `Nudr_GroupIDmap` resource, no `Nudr_DataRepository` count change), 72 as of ADR-0165
(ADR-0166 composed entirely from already-counted resources, no count change), 74 as of ADR-0167,
76 as of ADR-0168, now 78 as of ADR-0169 -- see below). This is well past free5GC's own ~42+
figure for real
`Nudr_DataRepository` resource types; the real, still-open gap from here is
the not-yet-surveyed remainder of `TS29505_Subscription_Data.yaml` itself (`group-data/*`,
`a2x-data`, `rangingsl-privacy-data`, `ranging-slpos-data`, `5mbs-data`, and others) plus the
genuinely deferred subsystems below, not a shrinking free5GC-comparison count.

**Highest-priority missing resources** (the ones with real, direct behavioral impact elsewhere in
this project, not just data-model completeness): Authentication Data / Authentication Status
documents (UDR-side persistence for AUSF's own authentication vectors and result status), AM
Policy Data (UDR-side backing for PCF's `Npcf_AMPolicyControl`). **Closed, docs/DECISIONS.md
ADR-0083**: all three now real, live-verified endpoints (`authentication-subscription` GET+PATCH,
`authentication-status` PUT+GET+DELETE, `/policy-data/ues/{ueId}/am-data` GET+PATCH), taking UDR
from 6 to 9 of free5GC's ~42+ real resource types. Real, disclosed architectural note (flagged in
the original finding, not glossed over): AUSF/UDM/PCF's own existing stores were NOT migrated to
call these new routes -- that's a real, separate, deliberate future decision, same "stand up the
surface first, wire consumers later" precedent already used for UDR's own `provisioned-data`
group (ADR-0069) and for PCF itself (ADR-0028). **Closed, docs/DECISIONS.md ADR-0093**: AMF
non-3GPP-access context-data (`QueryAmfContextNon3gpp`/`CreateAmfContextNon3gpp`, real GET+PUT,
schema `AmfNon3GppAccessRegistration`, a real, distinct resource from the 3GPP-access one, not a
rename) -- taking UDR from 9 to 10 of free5GC's ~42+ real resource types. Same "surface first,
consumer wiring later" disclosed precedent -- AMF's own registration path does not yet call this
endpoint. **Closed, docs/DECISIONS.md ADR-0097**: SMSF Registration context-data, 3GPP-access and
non-3GPP-access (`CreateSmsfContext3gpp`/`QuerySmsfContext3gpp`/`DeleteSmsfContext3gpp` and their
non-3GPP counterparts, real GET+PUT+DELETE, schema `SmsfRegistration` shared by both real, distinct
resources) -- taking UDR from 10 to 12 of free5GC's ~42+ real resource types. Same "surface first,
consumer wiring later" precedent -- SMSF itself doesn't exist as a built NF in this project yet
(Tier 2), so nothing calls these new routes. **Closed, docs/DECISIONS.md ADR-0098**: IP-SM-GW
Registration context-data (`CreateIpSmGwContext`/`QueryIpSmGwContext`/`ModifyIpSmGwContext`/
`DeleteIpSmGwContext`, real PUT+GET+PATCH+DELETE, schema `IpSmGwRegistration`, real RFC 6902
`application/json-patch+json` patch) -- taking UDR from 12 to 13 of free5GC's ~42+ real resource
types, and the first UDR context-data resource with all four real operations together. Same
"surface first, consumer wiring later" precedent -- no NF currently calls this new endpoint.
**Closed, docs/DECISIONS.md ADR-0099**: Message Waiting Data (Document) context-data
(`CreateMessageWaitingData`/`QueryMessageWaitingData`/`ModifyMessageWaitingData`/
`DeleteMessageWaitingData`, real PUT+GET+PATCH+DELETE, schema `MessageWaitingData`, real
distinct `201`-vs-`204` PUT response codes unlike `ip-sm-gw`'s own always-`204` PUT) -- taking UDR
from 13 to 14 of free5GC's ~42+ real resource types. Same "surface first, consumer wiring later"
precedent -- SMSF, the real originator of MWD data, doesn't exist as a built NF in this project
yet (Tier 2). **Closed, docs/DECISIONS.md ADR-0100**: Roaming Information (Document) context-data
(`UpdateRoamingInformation`/`QueryRoamingInformation`, real GET+PUT only, schema
`RoamingInfoUpdate`, real distinct `201`-vs-`204` PUT response codes) -- taking UDR from 14 to 15
of free5GC's ~42+ real resource types. Same "surface first, consumer wiring later" precedent --
no NF currently calls this new endpoint. **Closed, docs/DECISIONS.md ADR-0101**: PEI Information
(Document) context-data (`CreateOrUpdatePeiInformation`/`QueryPeiInformation`, real GET+PUT only,
real `allOf`-composed schema `PeiUpdateInfo` correctly flattened by sbi-codegen into
`PeiUpdateInfo_Subscription_Data`, real distinct `201`-vs-`204` PUT response codes) -- taking UDR
from 15 to 16 of free5GC's ~42+ real resource types. Same "surface first, consumer wiring later"
precedent -- no NF currently calls this new endpoint. **Closed, docs/DECISIONS.md ADR-0102**:
Enhanced Coverage Restriction Data (`QueryCoverageRestrictionData`, real GET-only, seeded at
startup for the same two real test SUPIs every other GET-only UDR resource seeds -- same shape as
`provisioned-data`) -- taking UDR from 16 to 17 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md
ADR-0103**: LCS Privacy Subscription Data (`QueryLcsPrivacyData`, real GET-only, seeded at
startup) -- taking UDR from 17 to 18 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md
ADR-0104**: LCS Subscription Data (`QueryLcsSubscriptionData`, real GET-only, seeded at startup)
-- taking UDR from 18 to 19 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md
ADR-0105**: LCS Mobile Originated Subscription Data (`QueryLcsMoData`, real GET-only, seeded at
startup) -- taking UDR from 19 to 20 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md
ADR-0106**: LCS Broadcast Assistance Data (`QueryLcsBcaData`, real GET-only, added as a 4th column
on the existing `provisioned-data` group/`ProvisionedDataStore` rather than a new store) -- taking
UDR from 20 to 21 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md ADR-0107**:
Parameter Provision (Document) (`GetppData`/`ModifyPpData`, real GET+PATCH, RFC 6902,
upsert-capable) -- taking UDR from 21 to 22 of free5GC's ~42+ real resource types. **Closed,
docs/DECISIONS.md ADR-0108**: Parameter Provision profile Data (Document) (`QueryPPData`, real
GET-only, seeded at startup) -- taking UDR from 22 to 23 of free5GC's ~42+ real resource types.
**Closed, docs/DECISIONS.md ADR-0109**: Provisioned Parameter Data Entry / `pp-data-store`
(`Create`/`Get`/`Delete PP Data Entry` plus `Get Multiple PP Data Entries`, real PUT+GET+DELETE
plus a real sibling collection GET, composite `(ueId, afInstanceId)` key) -- taking UDR from 23 to
24 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md ADR-0110**: individual
Shared Data (`GetIndividualSharedData`, real GET-only, keyed by `sharedDataId` alone -- the first
UDR resource in this project genuinely not keyed per-UE) -- taking UDR from 24 to 25 of free5GC's
~42+ real resource types; its real sibling collection resource (`GetSharedData`, array query
parameter) remains deferred, needing array-query-param parsing this project has no precedent for
yet. **Closed, docs/DECISIONS.md ADR-0111**: Operator-Specific Data Container (Document)
(`QueryOperSpecData`/`ModifyOperSpecData`, real GET+PATCH, RFC 6902, upsert-capable, same
"no PUT/DELETE" shape as pp-data) -- taking UDR from 25 to 26 of free5GC's ~42+ real resource
types. **Closed, docs/DECISIONS.md ADR-0112**: Event Exposure Data (Document) (`QueryEEData`,
real GET-only, seeded at startup, distinct from this project's own UDM-side `Nudm_EE` work) --
taking UDR from 26 to 27 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md
ADR-0113**: UE Policy Set (`ReadUEPolicySet`/`CreateOrReplaceUEPolicySet`/`UpdateUEPolicySet`,
real GET+PUT+PATCH RFC 7396, the first `policy-data` group resource combining both a real
create-or-replace PUT and a real merge-patch PATCH) -- taking UDR from 27 to 28 of free5GC's ~42+
real resource types. **Closed, docs/DECISIONS.md ADR-0114**: `policy-data` group's
Operator-Specific Data (`ReadOperatorSpecificData`/`UpdateOperatorSpecificData`, real GET+PATCH
RFC 6902, genuinely distinct real resource from the `subscription-data`-scoped one, ADR-0111,
confirmed live) -- taking UDR from 28 to 29 of free5GC's ~42+ real resource types. **Closed,
docs/DECISIONS.md ADR-0115**: Sponsor Connectivity Data (`ReadSponsorConnectivityData`, real
GET-only, keyed by `sponsorId` alone -- the second UDR resource genuinely not keyed per-UE) --
taking UDR from 29 to 30 of free5GC's ~42+ real resource types. **Closed, docs/DECISIONS.md
ADR-0116**: individual BDT Data (`ReadIndividualBdtData`/`CreateIndividualBdtData`/
`UpdateIndividualBdtData`/`DeleteIndividualBdtData`, real GET+PUT+PATCH+DELETE, the richest real
`policy-data` operation set closed so far -- real PUT documents only `201`, no update-via-PUT
status, and real PATCH is NOT upsert-capable, both genuine divergences from every prior resource
of the same shape) -- taking UDR from 30 to 31 of free5GC's ~42+ real resource types. **Closed,
docs/DECISIONS.md ADR-0117**: PLMN UE Policy Set (`ReadPlmnUePolicySet`, real GET-only, reuses the
`UePolicySet` schema but keyed by `plmnId` rather than `ueId` -- a genuinely distinct,
H-PLMN-scoped resource, not a UE-scoped alias) -- taking UDR from 31 to 32 of free5GC's ~42+ real
resource types. **Closed, docs/DECISIONS.md ADR-0118**: Slice-specific Policy Control Data
(`ReadSlicePolicyControlData`/`UpdateSlicePolicyControlData`, real GET+PATCH-only, no `PUT`/`POST`
create operation exists at all, so the real RFC 7396 merge-patch is upsert-capable, same
disclosed precedent as `am-data`; keyed by `snssai`, this project's own disclosed
`sst + '-' + sd` string convention reused from ADR-0072 since the YAML itself documents no bare
path-segment encoding for the `Snssai` object schema) -- taking UDR from 32 to 33 of free5GC's
~42+ real resource types. **Closed, docs/DECISIONS.md ADR-0119**: group-specific Policy Control
Data (`ReadGroupPolCtrlData`/`ModifyGroupPolCtrlData`, real GET+PATCH-only, no `PUT`/`POST` create
operation exists at all, same upsert-capable merge-patch precedent as `slice-control-data`; keyed
by `intGroupId`, the real `GroupId` schema -- a plain string with no encoding ambiguity) -- taking
UDR from 33 to 34 of free5GC's ~42+ real resource types. Real, disclosed while surveying this
resource's siblings: `mbs-session-pol-data`'s own key (`MbsSessPolDataId`) is a deeply nested
`oneOf`/`anyOf` object with no documented bare-path-segment encoding and no existing project
precedent to reuse (genuinely worse than `snssai`'s own flat shape) -- left deferred rather than
inventing a serialization. With both remaining real `Nudr_DataRepository` list-siblings blocked,
**Closed, docs/DECISIONS.md ADR-0120** (asked and confirmed, not silently added): `GetRoutingIDs`
(`/routing-ids`) -- real, but from a genuinely **different** real Nudr API,
`TS29504_Nudr_GroupIDmap.yaml`'s `Nudr_GroupIDmap` service (`/nudr-group-id-map/v1`), not
`Nudr_DataRepository` (`/nudr-dr/v2`) -- hosted by the same UDR binary per TS 29.504, but does
**NOT** count toward this section's own "N of free5GC's ~42+ `Nudr_DataRepository` resources"
metric, still 34. **Closed, docs/DECISIONS.md ADR-0164**: `GetNfGroupIDs` (`/nf-group-ids`), the
second real `Nudr_GroupIDmap` resource, genuinely unblocked once ADR-0161's array-parsing infra
landed -- also does **NOT** count toward the `Nudr_DataRepository` metric. `Nudr_GroupIDmap`'s own
remaining resources (the `/nf-group-ids/subscriptions` change-notification family, surveyed in
ADR-0164 but deliberately deferred, same "no real webhook delivery" gap class as
`subs-to-notify`) remain open.
**Closed, docs/DECISIONS.md ADR-0121** (self-correction): NIDD Authorization Info context-data
(`CreateNIDDAuthorizationInfo`/`GetNiddAuthorizationInfo`/`ModifyNiddAuthorizationInfo`/
`RemoveNiddAuthorizationInfo`, real PUT+GET+PATCH+DELETE, same shape as `amf-3gpp-access`'s own
resource plus a real DELETE) -- taking UDR from 34 to 35 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. This resource had previously been lumped into the deferred
"ee-subscriptions/sdm-subscriptions/nidd-authorizations" bundle without an individual real YAML
read; it is genuinely a flat per-UE document, not a nested sub-subscription resource.
`ee-subscriptions`/`sdm-subscriptions` were NOT re-verified and remain genuinely deferred.
**Closed, docs/DECISIONS.md ADR-0122**: Identity Data by SUPI or GPSI
(`GetIdentityData`/`ModifyIdentityData`, real GET+PATCH, real RFC 6902 JSON Patch (not
merge-patch), no `PUT`/`POST` create operation exists at all so `apply_patch` is upsert-capable,
same precedent as `pp-data`/`operator-specific-data`) -- taking UDR from 35 to 36 of free5GC's
~42+ real `Nudr_DataRepository` resource types. This ADR also individually re-verified (not
re-bundled) the three items still named as deferred: `ee-subscriptions`/`sdm-subscriptions` are
confirmed genuinely deeply-nested subscription-lifecycle resources (server-generated `subsId` via
`POST`, further nested `amf-`/`smf-`/`hss-subscriptions` sub-collections, plus a parallel
`group-data`-scoped tree), and `/policy-data/subs-to-notify` is confirmed a real `POST`-based
collection with a server-generated `Location` and real webhook callback registration -- all three
remain genuine, disclosed gaps, not silently re-deferred. **Closed, docs/DECISIONS.md ADR-0123**:
ODB Data (`GetOdbData`, real GET-only, seeded at startup, same shape as
`coverage-restriction-data`) -- taking UDR from 36 to 37 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0125**: SMS Management
Subscription Data (`QuerySmsMngData`, real GET-only, added as a new `sms_mng_data` column on the
existing `udr_provisioned_data` table -- same real precedent ADR-0106 established for
`lcs-bca-data`) -- taking UDR from 37 to 38 of free5GC's ~42+ real `Nudr_DataRepository` resource
types. The real sibling `.../provisioned-data/sms-data` resource (schema `SmsSubscriptionData`)
was found during the same survey and is a strong next candidate, deliberately left for its own
turn. **Closed, docs/DECISIONS.md ADR-0126**: SMS Subscription Data (`QuerySmsData`, real
GET-only, added as a new `sms_data` column on the existing `udr_provisioned_data` table, same
precedent -- genuinely distinct from `sms-mng-data` above, confirmed by live side-by-side
verification) -- taking UDR from 38 to 39 of free5GC's ~42+ real `Nudr_DataRepository` resource
types. **Closed, docs/DECISIONS.md ADR-0127**: Trace Data (`QueryTraceData`, real GET-only, added
as a new `trace_data` column on the existing `udr_provisioned_data` table, same precedent; real
response schema is a `oneOf` of the full `TraceData` object or a bare `SharedDataId` string,
handled as opaque JSON) -- taking UDR from 39 to 40 of free5GC's ~42+ real `Nudr_DataRepository`
resource types. **Closed, docs/DECISIONS.md ADR-0128**: V2X Subscription Data (`QueryV2xData`,
real GET-only, genuinely NOT part of the `provisioned-data` group -- keyed by `ueId` alone, so a
new `udr_v2x_data` table/store rather than another column) -- taking UDR from 40 to 41 of
free5GC's ~42+ real `Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0129**:
ProSe Service Subscription Data (spec `operationId` literally `QueryPorseData`, a real typo in
`TS29505_Subscription_Data.yaml` itself, cited as-is; real GET-only, genuinely NOT part of the
`provisioned-data` group -- keyed by `ueId` alone, so a new `udr_prose_data` table/store) -- taking
UDR from 41 to 42 of free5GC's ~42+ real `Nudr_DataRepository` resource types. **Closed,
docs/DECISIONS.md ADR-0130**: User Consent Subscription Data (`QueryUserConsentData`, schema
`UcSubscriptionData` -- a single optional `userConsentPerPurposeList` map, no `required` fields at
all; real GET-only, genuinely NOT part of the `provisioned-data` group -- keyed by `ueId` alone, so
a new `udr_uc_data` table/store) -- taking UDR from 42 to 43 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0131**: Time
Synchronization Subscription Data (`QueryTimeSyncSubscriptionData`, schema
`TimeSyncSubscriptionData` -- unlike the last several GET-only resources closed, this one has real
`required` fields, `afReqAuthorizations` and `serviceIds`; real GET-only, genuinely NOT part of
the `provisioned-data` group -- keyed by `ueId` alone, so a new `udr_time_sync_data` table/store)
-- taking UDR from 43 to 44 of free5GC's ~42+ real `Nudr_DataRepository` resource types. **Closed,
docs/DECISIONS.md ADR-0133**: UE's Location Information (Document) (`QueryUeLocation`, schema
`LocationInfo` -- requires a non-empty `registrationLocationInfoList`; real GET-only, genuinely
NOT part of the `provisioned-data` group -- keyed by `ueId` alone, so a new `udr_location_data`
table/store) -- taking UDR from 44 to 45 of free5GC's ~42+ real `Nudr_DataRepository` resource
types. Its real sibling `nidd-authorization-data` was surveyed in the same pass and confirmed
genuinely blocked (not silently skipped): real required complex-object query parameters this
project has no parsing precedent for, the same class of gap already disclosed for `pdtq-data` --
moved to the genuinely-deferred list below. **Closed, docs/DECISIONS.md ADR-0134**: A2X
Subscription Data (`QueryA2xData`, schema `A2xSubscriptionData` -- every field optional, same
shape as `v2x-data`/`prose-data`; real GET-only, genuinely NOT part of the `provisioned-data`
group -- keyed by `ueId` alone, so a new `udr_a2x_data` table/store) -- taking UDR from 45 to 46 of
free5GC's ~42+ real `Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0135**:
Ranging and Sidelink Positioning Privacy Subscription Data (`QueryRangingSlPrivacyData`, schema
`RangingSlPrivacyData` -- every top-level field optional; optional `fields` query parameter for
field-selection filtering not honored, disclosed simplification; real GET-only, genuinely NOT
part of the `provisioned-data` group -- keyed by `ueId` alone, so a new
`udr_rangingsl_privacy_data` table/store) -- taking UDR from 46 to 47 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0136**: Ranging and
Sidelink Positioning Service Subscription Data (`QueryRangingSlPosData`, schema
`RangingSlPosSubscriptionData` -- every top-level field optional, no complex or required query
parameters at all; real GET-only, genuinely NOT part of the `provisioned-data` group -- keyed by
`ueId` alone, so a new `udr_ranging_slpos_data` table/store) -- taking UDR from 47 to 48 of
free5GC's ~42+ real `Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0137**:
5MBS Subscription Data (Document) (`Query5mbsData`, schema `MbsSubscriptionData` -- every field
optional, no complex or required query parameters at all; real GET-only, genuinely NOT part of
the `provisioned-data` group -- keyed by `ueId` alone, so a new `udr_5mbs_data` table/store) --
taking UDR from 48 to 49 of free5GC's ~42+ real `Nudr_DataRepository` resource types. **Closed,
docs/DECISIONS.md ADR-0139**: Service Specific Authorization Info (Document) context-data
(`CreateServiceSpecificAuthorizationInfo`/`GetServiceSpecificAuthorizationInfo`/
`ModifyServiceSpecificAuthorizationInfo`/`RemoveServiceSpecificAuthorizationInfo` -- real
PUT+GET+PATCH+DELETE, real distinct 201-vs-204 PUT, real RFC 6902 PATCH, composite `(ueId,
serviceType)` key -- taking UDR from 49 to 50 of free5GC's ~42+ real `Nudr_DataRepository`
resource types. Its real sibling `service-specific-authorization-data/{serviceType}` was surveyed
in the same pass and confirmed genuinely blocked (not silently skipped): real required
complex-object query parameters this project has no parsing precedent for, same class of gap
already disclosed for `nidd-authorization-data`. **Closed, docs/DECISIONS.md ADR-0140**: Group
Identifiers mapping resource (`GetGroupIdentifiers`, schema `GroupIdentifiers` -- every field
optional, no path parameters, genuinely NOT per-UE; two real optional query parameters
(`ext-group-id`/`int-group-id`) are alternate lookup keys for the same seeded record, at least one
required by this implementation, disclosed simplification) -- taking UDR from 50 to 51 of
free5GC's ~42+ real `Nudr_DataRepository` resource types. First real `group-data` sub-resource
closed -- the remainder of `group-data` remains genuinely deferred. **Closed, docs/DECISIONS.md
ADR-0141**: NSSAI update ack (Document) resource (`CreateOrUpdateNssaiAck`/`QueryNssaiAck`,
schema `NssaiAckData` -- real PUT+GET, no PATCH/DELETE; real, disclosed: the spec documents only a
single `204` PUT response, no `201`, so genuinely no create-vs-update distinction exists) -- taking
UDR from 51 to 52 of free5GC's ~42+ real `Nudr_DataRepository` resource types. First real
`ue-update-confirmation-data` sub-resource closed -- its `sor-data`/`upu-data`/`subscribed-cag`
siblings remain genuinely deferred. Bare `/subscription-data/{ueId}` was surveyed in the same pass
and confirmed genuinely blocked: it combines both the array-query-param (`dataset-names`) and
complex-object-query-param (`single-nssai`) gaps already disclosed elsewhere. **Closed,
docs/DECISIONS.md ADR-0142**: CAG update ack (Document) resource (`CreateCagUpdateAck`/
`QueryCagAck`, schema `CagAckData` -- identical shape to `NssaiAckData`, same real 204-only-PUT,
no create-vs-update distinction) -- taking UDR from 52 to 53 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0143**: Authentication SoR
(Document) and Authentication UPU (Document) resources (`CreateAuthenticationSoR`/`QueryAuthSoR`/
`UpdateAuthenticationSoR` for `sor-data`, schema `SorData`, real PUT+GET+PATCH including a real
RFC 6902 PATCH; `CreateAuthenticationUPU`/`QueryAuthUPU` for `upu-data`, schema `UpuData`, real
PUT+GET only, no PATCH/DELETE at all -- a genuine, disclosed asymmetry between two otherwise
same-shaped siblings) -- taking UDR from 53 to 54 of free5GC's ~42+ real `Nudr_DataRepository`
resource types. This closes all four `ue-update-confirmation-data` sub-resources surveyed to date.
**Closed, docs/DECISIONS.md ADR-0144**: group-data individual 5G VN Group Configuration resource
(`Create5GVnGroup`/`Get5GVnGroupConfiguration`/`Modify5GVnGroup`/`Delete5GVnGroup`, schema
`5GVnGroupConfiguration` -- real GET+PUT+PATCH+DELETE, PUT documents only `201` (same precedent
as `bdt-data`), PATCH real RFC 6902) -- taking UDR from 54 to 55 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. Second real `group-data` sub-resource closed, after
`group-identifiers` (ADR-0140); the sibling bare collection GET (`Query5GVnGroup`) was surveyed
and confirmed genuinely blocked on a real `style: form, explode: false` array query parameter,
same class already disclosed for `pdtq-data`/`nf-group-ids`. **Closed, docs/DECISIONS.md
ADR-0145**: group-data individual 5G MBS Group Membership resource
(`Create5GmbsGroup`/`GetMulticastMbsGroupMemb`/`Modify5GmbsGroup`/`Delete5GmbsGroup`, schema
`MulticastMbsGroupMemb` -- structurally an exact twin of the 5G VN Group Configuration resource)
-- taking UDR from 55 to 56 of free5GC's ~42+ real `Nudr_DataRepository` resource types. Third
real `group-data` sub-resource closed; its own sibling bare collection GET (`Query5GmbsGroup`)
was surveyed and confirmed the same genuinely blocked array-query-param shape. **Closed,
docs/DECISIONS.md ADR-0146**: group-data Event Exposure Data for a group resource
(`QueryGroupEEData`, schema `EeGroupProfileData` -- every field optional, real GET-only, seeded,
genuinely distinct sibling of the per-UE `ee-profile-data` resource, keyed by `ueGroupId`) --
taking UDR from 56 to 57 of free5GC's ~42+ real `Nudr_DataRepository` resource types. Fourth real
`group-data` sub-resource closed. With `group-data` now exhausted of unblocked candidates,
**closed, docs/DECISIONS.md ADR-0147**: aggregate UE Update Confirmation Data resource
(`QueryUeUpdConf`, schema `UeUpdConfData` -- every field optional, real GET-only, composed live
from the four already-closed `sor-data`/`upu-data`/`subscribed-snssais`/`subscribed-cag` stores
rather than a fifth, duplicate table; always returns `200`, not the spec's documented `404`, since
the aggregate itself has no real create/update path of its own) -- taking UDR from 57 to 58 of
free5GC's ~42+ real `Nudr_DataRepository` resource types. **Closed, docs/DECISIONS.md ADR-0148**:
Event Exposure Subscriptions collection + individual document
(`Queryeesubscriptions`/`CreateEeSubscriptions`/`QueryeeSubscription`/`UpdateEesubscriptions`/
`ModifyEesubscription`/`RemoveeeSubscriptions`, schema `EeSubscription` -- real collection
GET+POST, individual GET+PUT+PATCH+DELETE, server-generated `subsId` (real UUID v4), genuinely
update-only PUT) -- taking UDR from 58 to 59 of free5GC's ~42+ real `Nudr_DataRepository` resource
types. This corrects ADR-0122's own earlier blanket "genuinely deeply-nested" characterization of
`ee-subscriptions`/`sdm-subscriptions`: on direct read, `ee-subscriptions`'s own collection-GET
array filters are genuinely optional, not the required-array-param class that blocks other
resources -- `sdm-subscriptions` itself was not re-surveyed and remains deferred, as do
`ee-subscriptions`'s own deeper `amf-`/`smf-`/`hss-subscriptions` nested sub-collections.
**Closed, docs/DECISIONS.md ADR-0149**: Subs To Notify collection + individual document
(`SubscriptionDataSubscriptions`/`QuerySubsToNotify`/`QuerySubscriptionDataSubscriptions`/
`ModifysubscriptionDataSubscription`/`RemovesubscriptionDataSubscriptions`, schema
`SubscriptionDataSubscriptions` -- real collection GET+POST filtered by a real non-array,
required `ue-id` query param, individual GET+PATCH+DELETE with genuinely no PUT) -- taking UDR
from 59 to 60 of free5GC's ~42+ real `Nudr_DataRepository` resource types. Resolves half of this
resource's own original ADR-0122-era deferral (server-generated-ID precedent, now established by
`ee-subscriptions`/ADR-0148); the real `onDataChange` webhook-callback delivery mechanism remains
deliberately not built. **Closed, docs/DECISIONS.md ADR-0151**: SDM Subscriptions collection +
individual document (`Querysdmsubscriptions`/`CreateSdmSubscriptions`/`QuerysdmSubscription`/
`Updatesdmsubscriptions`/`ModifysdmSubscription`/`RemovesdmSubscriptions`, schema
`SdmSubscription` -- structurally identical to `ee-subscriptions`, server-generated `subsId`,
genuinely update-only PUT) -- taking UDR from 60 to 61 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. Completes correcting ADR-0122's original bundled
"genuinely deeply-nested" deferral of `ee-subscriptions`/`sdm-subscriptions` -- both now
individually surveyed and closed; each resource's own deeper nested sub-collection
(`amf-`/`smf-`/`hss-subscriptions`, `hss-sdm-subscriptions`) remains genuinely deferred.
**Closed, docs/DECISIONS.md ADR-0152**: AMF Subscription Info (Document), nested under an
individual ee-subscription ("Create AMF Subscriptions"/`GetAmfSubscriptionInfo`/
`ModifyAmfSubscriptionInfo`/`RemoveAmfSubscriptionsInfo`, schema `AmfSubscriptionInfo` -- real
GET+PUT+PATCH+DELETE, document body is a JSON array not a single object, real distinct
201-vs-204 PUT) -- taking UDR from 61 to 62 of free5GC's ~42+ real `Nudr_DataRepository` resource
types. First of `ee-subscriptions`' own nested sub-collections surveyed directly and closed;
`smf-subscriptions`/`hss-subscriptions` (siblings) and `sdm-subscriptions`' own
`hss-sdm-subscriptions` remain deferred. **Closed, docs/DECISIONS.md ADR-0153**: SMF Event
Subscription Info (Document), nested under an individual ee-subscription ("Create SMF
Subscriptions"/`GetSmfSubscriptionInfo`/`ModifySmfSubscriptionInfo`/`RemoveSmfSubscriptionsInfo`,
schema `SmfSubscriptionInfo` -- real GET+PUT+PATCH+DELETE, single-object document body unlike its
array-valued sibling, real distinct 201-vs-204 PUT) -- taking UDR from 62 to 63 of free5GC's
~42+ real `Nudr_DataRepository` resource types. Second of `ee-subscriptions`' own nested
sub-collections closed; `hss-subscriptions` (sibling) and `sdm-subscriptions`' own
`hss-sdm-subscriptions` remain deferred. **Closed, docs/DECISIONS.md ADR-0154**: HSS
Subscription Info (Document), nested under an individual ee-subscription ("Create HSS
Subscriptions"/`GetHssSubscriptionInfo`/`ModifyHssSubscriptionInfo`/`RemoveHssSubscriptionsInfo`,
schema `HssSubscriptionInfo` -- real GET+PUT+PATCH+DELETE, single-object document body, real
disclosed spec inconsistency (the real spec's own `GetHssSubscriptionInfo` response literally
cites `SmfSubscriptionInfo`; treated as a real typo per explicit user confirmation, same
precedent as ADR-0129's `QueryPorseData` typo), real distinct 201-vs-204 PUT) -- taking UDR from
63 to 64 of free5GC's ~42+ real `Nudr_DataRepository` resource types. Third and final of
`ee-subscriptions`' own nested sub-collections closed; `sdm-subscriptions`' own
`hss-sdm-subscriptions` remains deferred. **Closed, docs/DECISIONS.md ADR-0155**: HSS SDM
Subscription Info (Document), nested under an individual sdm-subscription ("Create HSS SDM
Subscriptions"/`GetHssSDMSubscriptionInfo`/`ModifyHssSDMSubscriptionInfo`/
`RemoveHssSDMSubscriptionsInfo`, schema `HssSubscriptionInfo` -- real GET+PUT+PATCH+DELETE,
single-object document body, real 204-only PUT confirmed by direct read (no `201`, matching the
existing `sor-data`/`upu-data` precedent, unlike its `ee-subscriptions`-nested siblings), same
disclosed `SmfSubscriptionInfo`-citation spec inconsistency as ADR-0154 resolved via that same
precedent without re-asking) -- taking UDR from 64 to 65 of free5GC's ~42+ real
`Nudr_DataRepository` resource types. This is `sdm-subscriptions`' own final deferred nested
sub-collection, now closed. **Closed, docs/DECISIONS.md ADR-0156**: Event Exposure Group
Subscriptions collection + individual document, group-data-scoped
(`QueryEeGroupSubscriptions`/`CreateEeGroupSubscriptions`/`QueryEeGroupSubscription`/
`UpdateEeGroupSubscriptions`/`ModifyEeGroupSubscription`/`RemoveEeGroupSubscriptions` -- real
GET+POST collection, GET+PUT+PATCH+DELETE individual document, schema `EeSubscription`), the
group-data-scoped sibling of `ee-subscriptions` (ADR-0148) keyed by `ueGroupId` instead of
`ueId` -- taking UDR from 65 to 66 of free5GC's ~42+ real `Nudr_DataRepository` resource types.
Also fixes a real, disclosed pre-existing bug found while live-verifying this new resource: the
`Location` header on `ee-subscriptions`' and `sdm-subscriptions`' own `POST`-create handlers was
returning the unsubstituted route-pattern placeholder (e.g. literal `{ueId}` text) instead of the
real path-parameter value; confirmed pre-existing on both already-shipped siblings and fixed in
all three resources in the same ADR. **Closed, docs/DECISIONS.md ADR-0157**: AMF Group
Subscription Info (Document), group-data-scoped (`CreateAmfGroupSubscriptions`/
`GetAmfGroupSubscriptions`/`ModifyAmfGroupSubscriptions`/`RemoveAmfGroupSubscriptions` -- real
GET+PUT+PATCH+DELETE, array-valued `AmfSubscriptionInfo[]` document, real distinct 201-vs-204
PUT), the group-data-scoped sibling of `ee-subscriptions/{subsId}/amf-subscriptions` (ADR-0152)
-- taking UDR from 66 to 67 of free5GC's ~42+ real `Nudr_DataRepository` resource types. This
ADR also found the ADR-0156 Location-header bug was far more widespread than its own 3-occurrence
fix: ~20 routes project-wide, including foundational Tier 1a resources predating any gap-closure
ADR. Presented to the user via `AskUserQuestion`; user explicitly chose to fix all ~20 occurrences
in this same turn via one new shared `resolved_location()` helper, rather than deferring or
narrowing scope. **Closed, docs/DECISIONS.md ADR-0158**: SMF Event Group Subscription Info
(Document), group-data-scoped (`CreateSmfGroupSubscriptions`/`GetSmfGroupSubscriptions`/
`ModifySmfGroupSubscriptions`/`RemoveSmfGroupSubscriptions` -- real GET+PUT+PATCH+DELETE,
single-object `SmfSubscriptionInfo` document, real distinct 201-vs-204 PUT), the
group-data-scoped sibling of `ee-subscriptions/{subsId}/smf-subscriptions` (ADR-0153) -- taking
UDR from 67 to 68 of free5GC's ~42+ real `Nudr_DataRepository` resource types. Second of
`group-data`'s own `ee-subscriptions/{subsId}/...` nested sub-collections closed;
`hss-subscriptions` (the third and final sibling) remains deferred. **Closed,
docs/DECISIONS.md ADR-0159**: HSS Event Group Subscription Info (Document), group-data-scoped
(`CreateHssGroupSubscriptions`/`GetHssGroupSubscriptions`/`ModifyHssGroupSubscriptions`/
`RemoveHssGroupSubscriptions` -- real GET+PUT+PATCH+DELETE, single-object `HssSubscriptionInfo`
document, real distinct 201-vs-204 PUT, real disclosed spec inconsistency -- DELETE/PATCH/GET
declare a non-bindable `externalGroupId` parameter with no matching path placeholder, resolved
using the real path template's own `ueGroupId` with no genuine ambiguity), the group-data-scoped
sibling of `ee-subscriptions/{subsId}/hss-subscriptions` (ADR-0154) -- taking UDR from 68 to 69
of free5GC's ~42+ real `Nudr_DataRepository` resource types. Third and final of `group-data`'s
own nested sub-collections closed -- **completes the whole group-data nested-subscription tree**
(mirroring the `ueId`-scoped `ee-subscriptions` family's own closure, ADR-0152/ADR-0153/ADR-0154).
**Surveyed, docs/DECISIONS.md ADR-0160** (no resource count change): bare
`/subscription-data/{ueId}`/`{ueId}/context-data` confirmed genuinely blocked on real array
query params (same class as `pdtq-data`/`nf-group-ids`); `GetSSAuData` investigated in depth
(real schema-citation mismatch resolved, then a deeper structural mismatch found against the
already-implemented CRUD sibling's own storage shape) and deliberately left deferred per explicit
user decision, rather than fabricate an undocumented field mapping or ship a permanently-empty
store. **Closed, docs/DECISIONS.md ADR-0161**: real `style: form, explode: false` array-
query-param parsing infra (`sbi_core::http2::split_form_array()`, six new real unit tests),
unblocking `pdtq-data`/`nidd-authorization-data`/`Nudr_GroupIDmap`'s `/nf-group-ids`/bare
`/subscription-data/{ueId}` for future turns, plus its first real consumer: `QueryContextData`
(`/subscription-data/{ueId}/context-data`, real GET-only, live-composed aggregate over 11
already-existing sub-resource stores, same design as `ue-update-confirmation-data` [ADR-0147])
-- taking UDR from 69 to 70 of free5GC's ~42+ real `Nudr_DataRepository` resource types.
**Closed, docs/DECISIONS.md ADR-0162**: PDTQ Data collection + individual document
(`ReadPdtqData`/`ReadIndividualPdtqData`/`CreateIndividualPdtqData`/`UpdateIndividualPdtqData`/
`DeleteIndividualPdtqData`, `TS29519_Policy_Data.yaml`) -- the first real UDR resource confirmed
genuinely unblocked (not merely a candidate) by ADR-0161's infra. Real, disclosed:
`pdtqReferenceId` client-supplied; `CreateIndividualPdtqData` documents only `201` matching the
pre-existing `bdt-data` precedent; real RFC 7396 merge-patch; the collection GET's own optional
`pdtq-ref-ids` array filter deliberately not honored, matching the established "optional filter
not honored" precedent -- taking UDR from 70 to 71 of free5GC's ~42+ real
`Nudr_DataRepository` resource types.
**Closed, docs/DECISIONS.md ADR-0164**: `GetNfGroupIDs` (`/nf-group-ids`,
`TS29504_Nudr_GroupIDmap.yaml`), the second real `Nudr_GroupIDmap` resource, genuinely unblocked
by ADR-0161's infra (real REQUIRED `nf-type` array query param) -- does **NOT** count toward the
`Nudr_DataRepository` metric, still 71, same non-increment precedent as `GetRoutingIDs`
(ADR-0120). GET-only, seeded at startup (no write path exists in the spec for the mapping data);
real `404` honored on an empty composed result (unlike the aggregate live-view resources' own
always-`200`), since this resource's own schema requires `minProperties: 1` and documents a real
`404`. The sibling `/nf-group-ids/subscriptions` change-notification family was surveyed but
deliberately deferred to its own future turn.
**Closed, docs/DECISIONS.md ADR-0165**: `GetNiddAuData`
(`/subscription-data/{ueId}/nidd-authorization-data`, `TS29505_Subscription_Data.yaml`) --
genuinely distinct from the already-closed `context-data/nidd-authorizations` (ADR-0121). Real
finding: its actual blocker was **not** ADR-0161's array-query-param gap -- its real REQUIRED
`single-nssai` uses `content: application/json` (a JSON-encoded query value), a genuinely
different shape, handled with a direct `json::parse()` rather than new shared infra. GET-only, no
write path exists in this project's in-scope APIs (real provisioning is UDM's own `Nudm_NIDDAU`
service, out of scope); seeded at startup -- taking UDR from 71 to 72 of free5GC's ~42+ real
`Nudr_DataRepository` resource types.
**Closed, docs/DECISIONS.md ADR-0166**: bare `/subscription-data/{ueId}` (`QueryUeSubscribedData`)
-- a real 32-field aggregate (`UeSubscribedDataSets = ProvisionedDataSets & ContextDataSets &
UeUpdConfData`) composed **entirely** from already-closed sub-resources; needed zero new stores.
Does **NOT** increment the `Nudr_DataRepository` count -- still 72, unchanged from ADR-0165, same
non-double-counting precedent as `QueryContextData`/`ue-update-confirmation-data`. Real, disclosed
gaps: `serving-plmn` (optional here, required by `ProvisionedDataStore`'s own composite key) gates
7 fields when absent (recoverable once supplied); `niddAuthData` is never composed (a permanent
gap -- its own key needs `mtc-provider-information`, a param this resource doesn't expose at all).
**Closed, docs/DECISIONS.md ADR-0167**: bare `group-data/5g-vn-groups` and
`group-data/mbs-group-membership` collection GETs (`Query5GVnGroup`/`Query5GmbsGroup`) -- real map
responses composed from the same tables their own individual-resource siblings already write to
(ADR-0144/ADR-0145), no new schema. Real optional `gpsis` array filter accepted but not honored
(honoring it needs per-group member-list inspection, deferred). Taking UDR from 72 to 74 of
free5GC's ~42+ real `Nudr_DataRepository` resource types -- fully closes this series' own
array-parsing-infra-unblocked candidate list from ADR-0161.
**Closed, docs/DECISIONS.md ADR-0168**: `5g-vn-groups/internal` and `mbs-group-membership/internal`
(`Query5GVnGroupInternal`/`Query5GMbsGroupInternal`) -- real filter-by-`internalGroupIdentifier`
lookups over the same `list_all()` (ADR-0167), real `404`-on-no-match. **Found and fixed a real
router-ordering hazard** in the process: this router (`libs/sbi-core/src/http2_server.cpp`) has no
literal-vs-wildcard route priority, so this literal 4-segment `/internal` path had to be registered
*before* the same-segment-count `{externalGroupId}` wildcard route or it would have been
permanently shadowed -- fixed and verified live, not just reasoned about. Taking UDR from 74 to 76
of free5GC's ~42+ real `Nudr_DataRepository` resource types.
**Closed, docs/DECISIONS.md ADR-0169**: `5g-vn-groups/pp-profile-data` and
`mbs-group-membership/pp-profile-data` (`Query5GVnGroupPPData`/`Query5GMbsGroupPPData`) -- this
project's first genuinely keyless singleton resources (real, disclosed: their own response
schemas are NOT per-group documents, unlike every other `group-data` sub-resource), modeled as
fixed single-row tables. Same route-ordering fix as ADR-0168, re-applied and re-verified. Closes
out `5g-vn-groups`/`mbs-group-membership`'s own entire real, in-scope resource set. Taking UDR
from 76 to 78 of free5GC's ~42+ real `Nudr_DataRepository` resource types.
**Closed, docs/DECISIONS.md ADR-0170**: `Nudr_GroupIDmap`'s own `/nf-group-ids/subscriptions`
subscription-management family (`CreateGroupIdSubscription`/`QueryGroupIdSubscription`/
`ModifyGroupIdSubscription`/`RemoveGroupIdSubscription`) -- real CRUD, structurally the same shape
as `ee-subscriptions`/`subs-to-notify`, contrary to ADR-0164's own earlier "genuinely more
complex" characterization (made without a detailed read at the time). Real, disclosed: the spec's
own `onGroupIdMapChange` webhook callback is not implemented -- same disclosed gap class as
`subs-to-notify`'s own lack of real webhook delivery. Does **NOT** increment the
`Nudr_DataRepository` count -- still 78, same non-counting precedent as `GetRoutingIDs`/
`GetNfGroupIDs`.
**Built and live-verified, docs/DECISIONS.md ADR-0171** (user-directed, "full sweep" chosen from 3
offered scopes): real `onDataChange` webhook delivery infrastructure for `subs-to-notify` -- a
shared, mutex-guarded HTTP/2+mTLS client, real `DataChangeNotify`/`ChangeItem` construction for
all three real write shapes (PUT/PATCH/DELETE), and real, live, end-to-end delivery verified
against a separate receiver process. Real, disclosed: wired into exactly 3 of ~85 real per-UE
write routes (`amf-3gpp-access`, `amf-non-3gpp-access`, `smf-registrations`) -- the remaining ~80
are **not yet wired**, a real, tracked follow-up, not silently claimed complete. Real, disclosed
blocker found while implementing: `udr_subs_to_notify`'s own pre-existing `ue_id NOT NULL` schema
(ADR-0149) cannot represent UE-less subscriptions, so non-per-UE resources (group-data and others)
need a separate schema fix before they can be wired at all. `Nudr_GroupIDmap`'s own, separate
`onGroupIdMapChange` callback remains entirely unimplemented (and currently unfireable regardless,
since `nf-group-ids` has no write path).
**Continued, docs/DECISIONS.md ADR-0172**: 10 more real per-UE resources wired to real
`onDataChange` delivery using the identical infrastructure and mechanical pattern (`sm-data`/
`am-data`, `authentication-subscription`, `authentication-status`, `smsf-3gpp-access`/
`smsf-non-3gpp-access`, `ip-sm-gw`, `mwd`, `roaming-information`, `pei-info`) -- **13 of ~40 real
per-UE `Nudr_DataRepository` resources now have real `onDataChange` delivery, 24 real write-route
call sites**. Real, disclosed: this pass live-verified one new representative resource
end-to-end rather than re-proving all 10 individually, since `notify_subscribers()` is identical,
already-proven code at every site.
**Continued, docs/DECISIONS.md ADR-0173**: 5 more real resources wired (`pp-data`,
`pp-data-store`, `subscription-data`'s own `operator-specific-data`, `ue-policy-set`,
`policy-data`'s own `operator-specific-data`) -- **18 of ~40 real per-UE `Nudr_DataRepository`
resources now have real `onDataChange` delivery, 31 real write-route call sites**. Real fix
applied while wiring: `UePolicySetStore::merge_patch()`'s own already-returned patched document
was previously discarded by its route (only ever needed `204`) -- now captured and threaded into
the notification payload, live-verified to carry the real merged result, not the raw patch body.
**Continued, docs/DECISIONS.md ADR-0174**: 7 more real resources wired (`nidd-authorizations`,
`identity-data`, `service-specific-authorizations`, `subscribed-snssais`, `subscribed-cag`,
`sor-data`, `upu-data`) -- **25 of ~40 real per-UE `Nudr_DataRepository` resources now have real
`onDataChange` delivery, 43 real write-route call sites**. Real, disclosed: `bdt-data` checked and
correctly skipped (keyed by `bdtReferenceId`, not `ueId` -- same non-per-UE exclusion as the
group-data family); live-verified `sor-data`'s `PUT`+RFC 6902 `PATCH` end-to-end via the real
HTTPS receiver, including self-correcting an initial subscription-matching mistake (the real match
semantics require the subscription's own `monitoredResourceUris` entry to *contain* the resolved
path as a substring, not the reverse) before concluding the delivery worked.
**Continued, docs/DECISIONS.md ADR-0175**: 6 more real resources wired (`ee-subscriptions`
individual document, `sdm-subscriptions` individual document, and their four nested per-`subsId`
sub-resources: `amf-subscriptions`, `smf-subscriptions`, `hss-subscriptions`,
`hss-sdm-subscriptions`) -- **31 of ~40 real per-UE `Nudr_DataRepository` resources now have real
`onDataChange` delivery, 61 real write-route call sites**. Real, disclosed: the `POST` create
routes on the `ee-subscriptions`/`sdm-subscriptions` collections were deliberately left unwired
(a subscriber cannot plausibly already be watching a `monitoredResourceUris` entry for a
server-generated `subsId` that doesn't exist yet); live-verified `ee-subscriptions`'s own
`POST`-create -> `subs-to-notify` subscribe -> RFC 6902 `PATCH` chain end-to-end via the real
HTTPS receiver.
**Continued, docs/DECISIONS.md ADR-0176**: a comprehensive sweep of every remaining write route
confirmed all real per-UE resources were already wired -- everything left was structurally
non-per-UE (keyed by `bdtReferenceId`/`pdtqReferenceId`/`snssai`/`intGroupId`/`externalGroupId`/
`ueGroupId`) or otherwise out of scope (subscription-management resources themselves,
`Nudr_GroupIDmap`'s own separate callback, `POST`-create routes). User chose to fix the real
prerequisite this blocked on: `udr_subs_to_notify.ue_id` is now a real nullable column (was
`NOT NULL` with an empty-string sentinel) backing a new `list_ue_less()` store method, unblocking
non-per-UE resources for `onDataChange` delivery. First two wired with it: `bdt-data` and
`pdtq-data` (both `PUT`+RFC 7396 `PATCH`+`DELETE`) -- **61 real per-UE call sites + 6 real
non-per-UE call sites = 67 real write-route call sites total, 33 distinct resource types wired**.
Live-verified the schema fix end-to-end: a subscription with no `ueId` field is stored as real SQL
`NULL` (confirmed via direct `psql` query) and its delivered `DataChangeNotify` correctly omits
the `ueId` key entirely (schema-conformant, matching the real, genuinely optional
`DataChangeNotify.ueId` field).
**Continued, docs/DECISIONS.md ADR-0177**: 4 more non-per-UE resources wired using ADR-0176's own
`notify_subscribers_ue_less()`: `slice-control-data`, `group-control-data` (both RFC 7396
`PATCH`-only), `5g-vn-groups`, `mbs-group-membership` (both `PUT`+RFC 6902 `PATCH`+`DELETE`) --
**61 real per-UE call sites + 14 real non-per-UE call sites = 75 real write-route call sites
total, 37 distinct resource types wired**. Live-verified `5g-vn-groups`'s own `PUT`+`DELETE`
end-to-end via the real HTTPS receiver, both notifications correctly omitting the `ueId` field.
**Continued, docs/DECISIONS.md ADR-0178**: the group-data `ee-subscriptions` family wired --
individual document (`PUT`+RFC 6902 `PATCH`+`DELETE`) and its 3 nested `amf-subscriptions`/
`smf-subscriptions`/`hss-subscriptions` sub-resources, the group-data-scoped structural twins of
the already-wired per-UE `ee-subscriptions` family -- **61 real per-UE call sites + 26 real
non-per-UE call sites = 87 real write-route call sites total, 41 distinct resource types wired**.
This closes ADR-0176's own disclosed non-per-UE candidate list in full. Live-verified the
create->subscribe->PATCH chain end-to-end via the real HTTPS receiver. The only remaining real gap
in `onDataChange`-adjacent delivery is `Nudr_GroupIDmap`'s own separate `onGroupIdMapChange`
callback, a genuinely different API, currently unfireable regardless since `nf-group-ids` itself
has no write path.
**Continued, docs/DECISIONS.md ADR-0179**: closed the real, previously-disclosed testing gap named
in every prior `onDataChange` ADR -- delivery had only ever been verified manually. New automated
test `UdrIntegration.OnDataChangeWebhookDeliveredOnPutPatchDelete`
(`tests/integration/test_udr_ondatachange_webhook.cpp`) drives a real `POST subs-to-notify` +
`PUT`+`PATCH`+`DELETE` flow against `smf-registrations` inside `ctest`, using a real in-process
`sbi_core::http2::Server` (the same TLS 1.3 + mTLS implementation every NF runs, not a stub) as the
subscriber's callback endpoint. Found and fixed three real bugs while writing it: a test-design
mistake (assumed `amf-3gpp-access` supports `DELETE`; it genuinely doesn't), a real crash
(`std::terminate()` from a still-joinable thread on early ASSERT return, fixed with RAII), and a
real test-isolation bug (fixed `ue_id` colliding with leftover PostgreSQL state from an
interrupted prior run, fixed via this project's own existing `getpid()`-suffix precedent). 332/332
tests pass.
**Continued, docs/DECISIONS.md ADR-0180**: the last remaining real gap, `Nudr_GroupIDmap`'s own
separate `onGroupIdMapChange` callback, is now wired. Direct read of
`TS29504_Nudr_GroupIDmap.yaml` confirmed `/nf-group-ids` genuinely has no write operation at all
to trigger it from -- rather than inventing a non-spec write endpoint, the callback now fires from
the one real mutation this mapping data undergoes, `nf_group_ids.seed(...)` at `udr` startup.
Live-verified by pre-seeding a subscription row directly in PostgreSQL (the only way to exercise
this real code path, since the callback fires before the server accepts any live connections) --
the receiver correctly logged a real `GroupIdMapNotify`. Explicitly disclosed: in every real run
of this project's own binaries, zero deliveries actually occur, since no subscription can exist
before the one trigger point (startup) has already passed. This closes real `onDataChange`/
`onGroupIdMapChange` delivery coverage across both of UDR's real Nudr APIs in full.
**Continued, docs/DECISIONS.md ADR-0181**: the real `gpsis`/`ext-group-ids` filtering backlog
across 4 already-closed `group-data` GET resources is now closed --
`Query5GVnGroup`/`Query5GmbsGroup` (bare collections, filter by real `members`/
`multicastGroupMemb`) and `Query5GVNGroupPPData`/`Query5GMbsGroupPPData` (keyless singletons,
filter `allowedMtcProviders`/`allowedMbsInfos` map keys, always keeping the real spec-documented
`"ALL"` wildcard). Direct schema read found these were never actually blocked on missing infra by
the time this project's own ADR-0167/ADR-0169 called them that -- each targets one concrete field
already present in data these routes already return. `QueryUeSubscribedData`'s own unrelated
`ext-group-ids` filter was re-checked and correctly remains unhonored (no store to filter its 32
flat per-UE fields against). Also fixed a stale top-of-file code comment that still described the
bare-collection filters as blocked.
**Continued, docs/DECISIONS.md ADR-0182**: `mbs-session-pol-data` (`GetMBSSessPolCtrlData`)
implemented for its own unambiguous `afAppId` `oneOf` branch (`polSessionId`'s own real schema is
`{mbsSessionId}`/`{afAppId}`, and `afAppId` is just `type: string` on its own -- no encoding
decision needed). Real, disclosed: this is a partial closure, not a full resolution -- the
`mbsSessionId`/`tmgi`/`ssm` branch remains exactly as unaddressed as ADR-0119's own original
deferral left it; that ADR already explicitly declined inventing a serialization for a
multi-level nested object (calling it fabrication), and this pass re-checked and did not reverse
that decision. Live-verified `GET` for both a seeded and an unseeded key.
This is well past free5GC's own ~42+ figure; the real, still-open work from here is the
`mbsSessionId`/`tmgi`/`ssm` branch (deliberately still unaddressed) and anything else surfaced by a
future survey, not chasing a shrinking
comparison count.
Influence Data (AF traffic-steering, needed once NEF
exists) remains open, out of scope until NEF is built.

**Closed, docs/DECISIONS.md ADR-0212**: Individual Authentication Status (Document)
(`authentication-status/{servingNetworkName}`, keyed by `(ueId, servingNetworkName)`, genuinely
distinct from the bare `authentication-status` document) -- a real new resource type. Also
**closed, same ADR**: `QueryProvisionedData` (the real `ProvisionedDataSets` aggregate) -- does
**NOT** increment the resource-type count, same non-double-counting precedent as
`QueryUeSubscribedData`/`QueryContextData`, since it composes entirely from already-closed
sub-resources. Same ADR corrected a stale claim in this doc's own earlier Tier-B audit: the
previously-claimed "`subs-to-notify` bulk-DELETE" gap does not correspond to any real operation in
the spec (the collection is `POST`-only) and that resource is already fully implemented
(task #122) -- no code fix needed, documentation only.

**Closed, docs/DECISIONS.md ADR-0213**: UDR's own `Policy_Data.yaml` Tier-B backlog, in full --
`ReadPolicyData` (bare `{ueId}` aggregate, does **NOT** increment the resource-type count, same
non-double-counting precedent as the aggregates above), Usage Monitoring Information (Document)
(`sm-data/{usageMonId}`, a real new resource type), `ReadBdtData` (bare `bdt-data` collection GET,
composed entirely from the already-closed individual `bdt-data/{bdtReferenceId}` resource, does
**NOT** increment the count), and the `Policy_Data`-specific Policy Data Subscriptions
(Collection) + Individual Policy (Data) Subscription (Document) (a real new resource type,
genuinely distinct from `Subscription_Data`'s own `subs-to-notify`). **UDR now has zero known
Tier-B gaps -- every real operation across both `Subscription_Data.yaml` and `Policy_Data.yaml`
is either implemented or an explicitly disclosed, out-of-scope gap.**

---

## UPF

**Scale context**: this project's UPF is 1,743 lines vs free5GC's go-upf 8,606 (Go) vs open5GS's
4,423 (C).

### Real PFCP (N4) message coverage

free5GC's `internal/pfcp/pfcp.go` dispatches the full real PFCP message set (grep-confirmed,
request+response pairs): `Heartbeat`, `PFDManagement`, `AssociationSetup`, `AssociationUpdate`,
`AssociationRelease`, `NodeReport`, `SessionSetDeletion`, `SessionEstablishment`,
`SessionModification`, `SessionDeletion`, `SessionReport`. This project's UPF now handles every one
of these except `SessionSetDeletion` -- `Heartbeat`, `AssociationSetup`, `AssociationUpdate`,
`AssociationRelease`, `SessionEstablishment`/`Modification`/`Deletion`, `PFDManagement`, sends and
now (real, both directions) receives `NodeReport`. `AssociationUpdate`/`AssociationRelease` closed
by gap-closure task #107 part 1 (ADR-0084, live-verified over real raw UDP); `PFDManagement` closed
by task #107 part 2 (ADR-0086, live-verified over real raw UDP -- real, disclosed gap: no
Application Detection Filter engine yet consumes the provisioned PFDs); `NodeReport` closed by task
#107's final slice (ADR-0087, live-verified between two real, independently-built processes --
UPF's own send-side encoder and SMF's real `PfcpPeer` receive-side handler).

**`SessionSetDeletion` real, disclosed correction to this section's own original finding** (ADR-
0087): this project's vendored spec text (`specs/PFCP/29244-e30.pdf`, Table 7.3-1's own
"Applicability" columns) marks Session Set Deletion (message types 14/15) applicable to Sxa/Sxb
only -- **not Sxc**, and N4 (this project's own real interface) is Sxc. Its own IE table (Table
7.4.6.1-1) independently confirms this: every conditional IE is an EPC-only FQ-CSID concept
(SGW-C/PGW-C/SGW-U/PGW-U/TWAN/ePDG/MME) this project's 5GC-only scope has no real analogue for.
free5GC's own dispatch of this message type is real, but does not by itself mean the message
applies to a 5GC-only N4 deployment such as this one's -- a genuine "not applicable" per the spec
text actually read, not a deferred stub. **Task #107 is now closed in full**: 4 of its 5 originally-
named gaps are real and closed, the 5th is a real non-gap.

### Datapath: this project is already ahead here, worth recording per ADR-0075's "superior, not
just parity" framing, not just gaps

free5GC's UPF forwards via **`gtp5g`**, a real, separate Linux kernel module the free5GC project
maintains and drives via netlink (`internal/forwarder/gtp5g.go`/`gtp5glink.go`) -- a real,
reasonably fast kernel-module datapath, but a custom out-of-tree kernel module dependency.
open5GS's UPF (`src/upf/gtp-path.c`) uses plain userspace socket-based GTP-U forwarding -- no
kernel module, no DPDK, no XDP -- simpler to deploy but the least performant of the three. This
project's own UPF uses **eBPF/XDP** (ADR-0043, "fully live-verified end to end") -- a real,
in-kernel, no-custom-module, generally higher-throughput datapath approach than either reference's
own choice. **Real, disclosed, not yet benchmarked**: ADR-0049 already recorded "zero
benchmarking of any kind... performed against free5GC or anything else" -- this is architectural
superiority on paper, not yet a proven-faster claim, and P4.12/ADR-0049's own benchmarking-harness
gap is what would actually prove it.

---

## CHF

**Correction to an earlier, wrong statement made in this same session**: when first asked about
CHF's status, this assistant said free5GC has no charging function at all. That was wrong and has
now been checked directly, not just retracted on faith: free5GC has a real `chf` repo
(`github.com/free5gc/chf`, 10,931 lines Go). open5GS, by contrast, genuinely has **no** `chf`
directory in its real monorepo (`find` over the full extracted source confirms it) -- the
original claim was right for open5GS, wrong for free5GC.

### `Nchf_*` SBI service surface -- this project is ahead here

free5GC's CHF processor surface (`internal/sbi/processor/`) is narrow: `converged_charging.go`
(`HandleChargingdataInitial/Update/Release` -- `Nchf_ConvergedCharging` only) and `cdr.go`. No
`Nchf_SpendingLimitControl`, no `Nchf_OfflineOnlyCharging` found anywhere in free5GC's real CHF
source. This project's CHF implements all three real 5G-native services (`Nchf_ConvergedCharging`,
`Nchf_SpendingLimitControl` with the real, live-verified full N28 loop from ADR-0072/-0073,
`Nchf_OfflineOnlyCharging`), plus the legacy Diameter Gy/Rf/Sy and CAP/CAMEL protocol-translation
layer (P4.5), plus AI-native predictive quota sizing (P4.8, ADR-0074) -- none of which free5GC's
CHF has any equivalent of. **On 5G-native service breadth and legacy-protocol translation, this
project is already ahead of free5GC**, consistent with ADR-0075's "superior, not just parity"
mandate already being true in this specific area, not just aspirational.

### Real gap, now CLOSED (task #108, ADR-0089): TS 32.298 CDR encoding

free5GC's CHF has a real, substantial `cdr/` module (4,746 lines) implementing genuine TS 32.298
ASN.1 BER CDR encoding -- confirmed by real, specific 3GPP IE-matching type names
(`cdr/cdrType/`: `CHFRecord` -- the actual real TS 32.298 top-level 5G CDR record type name --
`AllocationRetentionPriority`, `CauseForRecClosing`, `AMFID`, `ChargingID`,
`AgeOfLocationInformation`, dozens more), plus its own hand-written BER marshal/unmarshal
(`cdr/asn/ber_{marshal,unmarshal}.go` -- not asn1c-generated from a vendored `.asn` file, but a
real, faithful transcription of the real TS 32.298 field layout into Go structs).

This project's own `nfs/chf/schema.clickhouse.sql` used to self-disclose the matching gap: "this
is NOT a conformant TS 32.298 CDR... TS 32.298 is not vendored in this repo." The user supplied
the real spec (`specs/TS_32_298.pdf`, ETSI TS 132 298 V18.8.0/Release 18, verified genuine) and
`nfs/chf/src/cdr_asn1.{hpp,cpp}` now implements a real, BER-encoded `[200] chargingFunctionRecord`
transcribed independently from the spec itself -- never from free5GC's own Go structs, per
ADR-0001's "reference reading only, never copy" rule. Real, disclosed, narrower scope than
free5GC's: 10 of 46 real `ChargingRecord` fields are populated (the ones this project's own CHF
has genuine data for); the rest need unvendored specs (TS 32.255, TS 32.260) or don't apply to
this project's current scope. Live-verified via real curl + direct ClickHouse hex-decode of the
stored `asn1_cdr` column against a real running CHF instance. Disclosed version gap: v18.8.0/
Release 18, not yet re-verified against REL-19. Full detail in ADR-0089/`docs/TRACEABILITY.md`.

### `ccs_diameter/` -- roughly parallel to this project's own Gy/Rf, not a new gap

free5GC's `ccs_diameter/` (Diameter CCS -- Credit Control Service, its own Ro/Gy-class dictionary
and codec) is architecturally the same real capability this project's own Diameter Gy/Rf/Sy
implementation (P4.5, ADR-0059/-0060) already covers. Not re-derived line-by-line in this pass
since this project's own Diameter work is already real, tested, and live-verified -- flagged for
a closer behavioral diff only if a specific discrepancy surfaces later, not assumed to be a gap.

---

## Summary and priority signal (9 of 17 built NFs covered by the original sweep; NSSF/BSF/NEF/SCP/5G-EIR/SMSF/GMLC/LMF added later, ADR-0183 through ADR-0191)

| NF | Scale ratio (ref/ours) | Highest-priority real gap |
|---|---|---|
| NRF | ~1-1.3x | **STALE ROW CORRECTED (ADR-0251)** -- both listed gaps are CLOSED: NFProfile semantic validation is real (`validate_nf_profile`, `nfs/nrf/src/main.cpp`, called on register), and the active heartbeat-expiry sweep landed in ADR-0079 (task #102). No known open gap from the original sweep. |
| AMF | ~10-14x | `ServiceRequest`: CLOSED (ADR-0076). N2 handover: CLOSED in full (task #100, ADR-0090 `PathSwitchRequest` slice + ADR-0095/ADR-0096 real concurrent associations + the full `HandoverRequired`...`HandoverNotify` chain); real AMF->SMF PDU-session-transfer depth **CLOSED, ADR-0258** (AMF now calls `Nsmf_PDUSession_UpdateSMContext` with `n2SmInfoType=HANDOVER_REQUIRED` per PDU session and relays SMF's real transfer; the fabricated TEID=0/0.0.0.0 transfer is deleted). `HandoverCancel` **CLOSED, ADR-0261** (real HandoverCancelAcknowledge to the source, `hoState=CANCELLED` to SMF per PDU session, `UEContextReleaseCommand` with `Cause=handover-cancelled` to the target gNB whose identity AMF now persists at preparation time). Disclosed: covers an already-PREPARED handover, not one racing an in-flight preparation (ADR-0009 synchronous-relay debt); AMF half's acknowledge path is now covered end-to-end over real SCTP by ADR-0264's test gNB; the **full relay is now tested end to end (ADR-0269)** -- a registered UE with a real PDU session, two simultaneously open gNB associations, HandoverRequired -> SMF -> HandoverRequest -> target gNB -> HandoverRequestAcknowledge -> HandoverCommand, verified against AMF's own log. Real gap that closing it exposed and ADR-0269 discloses rather than leaves implicit: AMF never calls SMF with the target's `HANDOVER_REQ_ACK` N2 SM info (TS 23.502 §4.9.1.3.2), the exact mirror of the gap ADR-0258 closed for HANDOVER_REQUIRED -- so UPF's downlink still points at the source gNB after a completed handover. **CLOSED, ADR-0270**: AMF now calls SMF with `HANDOVER_REQ_ACK` per admitted session and puts SMF's returned `HandoverCommandTransfer` in the `HandoverCommand`'s own `PDUSessionResourceHandoverList` (an IE it previously omitted) -- verified by UPF's own log naming the target gNB's TEID, four processes deep |
| AUSF | ~3-5x | **STALE ROW CORRECTED (ADR-0251)** -- both CLOSED: `Nausf_SoRProtection` and real TS 33.503 5G ProSe authentication are implemented (ADR-0091, task #104). |
| SMF | ~10-16x | `UpdateSMContext`: `PATH_SWITCH_REQ`/`_ACK` slice CLOSED (task #101, ADR-0092, real downlink FAR/GTP-U control-plane); ~~**COUNT CORRECTED (ADR-0251)**: 22 of the 26 real N2SmInfoType values remain a stub -- SMF now implements 4 (PATH_SWITCH_REQ/_ACK, plus HANDOVER_REQ_ACK in ADR-0248 and HANDOVER_REQUIRED in ADR-0249).~~ **CLOSED, ADR-0259 + ADR-0260**: all 26 values are now accounted for -- 16 decoded and handled inbound, 10 rejected with 400 as SMF-originated transfers rather than swallowed by the old blanket 204. Depth still varies and ADR-0260 names which values record rather than act; enum coverage is not the same claim as behavioural parity and neither ADR makes the second one. AMF's own N2 handover NGAP side is now closed (ADR-0095/ADR-0096), and the AMF->SMF relay wiring for handover-triggered PDU session resource re-setup is **CLOSED, ADR-0258** -- AMF really calls this endpoint now, pinned by `tests/integration/test_smf_handover_n2sminfo.cpp` |
| PCF | ~7-10x | **STALE ROW CORRECTED (ADR-0251)** -- `Npcf_PolicyAuthorization` (AF/IMS-facing QoS) was CLOSED by ADR-0080 (task #103); it stayed listed as the top open gap long afterwards and nearly caused it to be reimplemented. PCF's 7 remaining YAML files closed in ADR-0204/0205/0206 (task #163). |
| UDM | ~3-6x | **STALE ROW CORRECTED (ADR-0251)** -- both CLOSED: `Nudm_EE` in ADR-0082 (task #105), `Nudm_PP`'s 11 ops in ADR-0237. ~~Genuinely open: UECM's non-3GPP-AMF/SMSF/IP-SM-GW/NWDAF registration groups and UEAU's GetRgAuthData/GenerateAv/GenerateGbaAv.~~ **BOTH CLOSED, re-checked ADR-0258**: UEAU's three ops in ADR-0214; UECM's entire Tier-B backlog across ADR-0215/0216/0217/0218/0219/0220/0221/0227. This row was still asserting them open. |
| UDR | ~2.5-10x | Resource-type breadth (78 of 42+ real TS 29.504 resources closed, past parity -- both ee-subscriptions nested-subscription trees, bare `{ueId}/context-data`/`{ueId}` (32-field aggregate)/`pdtq-data`, `Nudr_GroupIDmap`'s `GetNfGroupIDs` + real `nf-group-ids/subscriptions` CRUD family, `GetNiddAuData`, and `group-data`'s entire `5g-vn-groups`/`mbs-group-membership` resource set (bare collection, individual, `/internal`, `/pp-profile-data` singleton) now fully closed (ADR-0161 through ADR-0170 -- fully closes this series' own array-parsing-infra-unblocked candidate list; ADR-0168 also found and fixed a real router literal-vs-wildcard ordering hazard, re-applied in ADR-0169; ADR-0169 is this project's first genuinely keyless singleton resource); real `onDataChange` webhook delivery infrastructure built and live-verified end-to-end (ADR-0171 through ADR-0180), wired into all 31 of ~40 real per-UE write resources this project has identified plus all 10 non-per-UE resources unblocked by the ADR-0176 schema fix (87 real write-route call sites total, 41 distinct resource types), backed by a real automated `ctest` integration test (ADR-0179), plus `Nudr_GroupIDmap`'s own separate `onGroupIdMapChange` callback now fired from the one real mutation its mapping data has -- startup seed, since the real spec genuinely has no live write path for it (ADR-0180) -- closing real `onDataChange`/`onGroupIdMapChange` coverage across both of UDR's real Nudr APIs; the real `gpsis`/`ext-group-ids` filtering backlog across `Query5GVnGroup`/`Query5GmbsGroup`/`Query5GVNGroupPPData`/`Query5GMbsGroupPPData` is now closed too (ADR-0181) -- `QueryUeSubscribedData`'s own unrelated `ext-group-ids` filter remains correctly unhonored (no store to filter its 32 flat per-UE fields against), `niddAuthData`'s permanent gap in the aggregate (needs `mtc-provider-information`, not exposed by that resource), and `GetSSAuData` (deliberately deferred, ADR-0160) remain real, disclosed gaps, see UDR section above) |
| UPF | ~1x (task #107 fully closed: Association Update/Release, ADR-0084; PFD Management, ADR-0086; Node Report, ADR-0087; Session Set Deletion correctly found not applicable to this project's own N4/Sxc interface) | datapath (XDP) already ahead of both references on paper, unbenchmarked |
| CHF | ~2.2x (free5GC), N/A (open5GS has none) | TS 32.298 real CDR encoding: CLOSED (task #108, ADR-0089, narrower disclosed scope than free5GC's); already ahead on 5G-native service breadth + AI-native charging |
| NSSF | Not yet swept against free5GC/open5GS source (built after this table, ADR-0183, not part of the original comparison pass) | Both real `Nnssf_NSSelection`/`Nnssf_NSSAIAvailability` services present, all 8 real operations implemented; real gap: the slice-selection decision itself is catalog-membership filtering against a fixed seed, not real subscriber-entitlement/NRF-discovery/NSAG-mapping logic -- see ADR-0183 |
| BSF | Not yet swept against free5GC/open5GS source (built after this table, ADR-0184, not part of the original comparison pass) | Real `Nbsf_Management`, all 15 real operations across 4 resource families implemented, including the spec's own real duplicate-combination 403-with-existing-info logic and real `BsfEvent` notification coverage; no known real gap identified yet since no reference-source diff has been done -- see ADR-0184 |
| NEF | Not yet swept against free5GC/open5GS source (built after this table, ADR-0185, not part of the original comparison pass) | All 14 real Nnef YAML files built (`Nnef_PFDmanagement` ADR-0185; `Nnef_SMService`, `Nnef_UEId`, `Nnef_DNAIMapping`, `Nnef_EASDeployment` ADR-0207; `Nnef_SMContext`, `Nnef_Authentication`, `Nnef_ECSAddress` ADR-0208; `Nnef_EventExposure` ADR-0209; `Nnef_TrafficInfluenceData`, `Nnef_Inference`, `Nnef_Training`, `Nnef_VFLInference`, `Nnef_VFLTraining` ADR-0210) -- task #164 complete, no Tier-A gaps remain. Real Tier-B breadth (individual stubbed/missing operations within these files) not separately audited -- see ADR-0210 |
| SCP | Not yet swept against free5GC/open5GS source (built after this table, ADR-0186, not part of the original comparison pass) | Only `Nscp_EventExposure` built (all 3 real ops) -- SCP's real defining role (TS 29.500 §§6.10-6.11 inline HTTP/2 message-forwarding proxy for indirect communication) remains entirely undesigned and unbuilt, a real architectural departure from every other NF this project has, explicitly scoped out via a user-directed `AskUserQuestion` decision, not silently skipped -- see ADR-0186 |
| 5G-EIR | Not yet swept against free5GC/open5GS source (built after this table, ADR-0187, not part of the original comparison pass; free5GC has no 5G-EIR implementation at all as of this project's own knowledge, so no reference exists to sweep against for this NF specifically) | This project's first Tier 2 NF. Real, complete: the entire real `N5g-eir_EquipmentIdentityCheck` API is 1 operation (`GetEquipmentStatus`), fully implemented and live-verified; real, disclosed gap is structural, not a shortfall -- the YAML has no write/provisioning operation anywhere (real IMEI database provisioning is OAM/GSMA scope, out of 3GPP's own SBI framework here), and AMF does not yet call this NF during Registration (TS 23.502 §4.2.2.2.2) -- see ADR-0187 |
| SMSF | Not yet swept against free5GC/open5GS source (built after this table, ADR-0188, not part of the original comparison pass) | This project's second Tier 2 NF. Real, complete: all 5 real `Nsmsf_SMService` operations implemented and live-verified, including real `multipart/related` handling and a real cross-file schema dependency on IP-SM-GW's own YAML; real, disclosed gap is structural -- no real downstream SMS-GMSC/IWMSC or TS 24.011 SMS-over-NAS CP-DATA relay exists in this project (genuinely out of this session's spec material), so `SendSMS`/`SendMtSMS` report SMSF-level acceptance only, never a fabricated delivery outcome -- see ADR-0188 |
| GMLC | Not yet swept against free5GC/open5GS source (built after this table, ADR-0189, not part of the original comparison pass) | This project's third Tier 2 NF. Real scope split, not a partial slice: 2 of 5 real `Ngmlc_Location` operations (`RequestLocation`/`CancelLocation`) genuinely require this project's own LMF (unbuilt at the time, now built ADR-0191 -- but not wired) and honestly report `501`/`404` rather than fabricating positioning data; the other 3 (`UpdateLocation`/`LocationUpdateSubcribe`/`PrivacyCheckIdMapping`) have no LMF dependency and are real, complete, live-verified implementations -- see ADR-0189 |
| LMF | Not yet swept against free5GC/open5GS source (built after this table, ADR-0191, not part of the original comparison pass) | This project's fourth Tier 2 NF. Real scope split: 4 of 7 real `Nlmf_Location` operations (`UpSubscriptions`/`DeleteSubscription`/`LocationContextTransfer`/`UpConfig`) are real, complete, RF-independent implementations; the other 3 (`DetermineLocation`/`LocationMeasure`/`CancelLocation`) genuinely require real LPP (TS 37.355) UE positioning or PRU/NRPPa measurement (TS 38.305/38.455), neither implemented in this project, and honestly report `501`/`404` -- see ADR-0191. Also fixed 2 real sbi-codegen/spec bugs found building this NF -- see ADR-0190 |

**Real, honest pattern across the sweep**: this project's "happy path" (initial attach, PDU
session establishment, core CRUD on each NF's primary resource) is consistently real and already
tested. The consistent gap is **procedure/service BREADTH** -- mobility/handover, the less-common
NAS/NGAP procedures, the wider data-repository resource surface, and a handful of
optional-but-real 3GPP services each NF has. AMF/SMF's shared N2-handover gap is the single
highest-impact, highest-effort item found. CHF is the one NF where this project is already
ahead of at least one reference on real service breadth.

**Still not done, per ADR-0075's own scope**: this closes the original `nssf`/`nef`/`scp`/`bsf`
"still not done" list -- all four now have at least one real, live-verified slice built (ADR-0183
through ADR-0186). None of the four have been swept against free5GC/open5GS source the way the
other 9 NFs above were, so their own real capability-parity gaps, if any, aren't characterized here
yet. Two real, large, still-open items within this group: NEF had 13 of its own 14 real YAML files
entirely unbuilt as of this table's own original writing (6 remain as of ADR-0208, see the NEF row
above), and SCP's own real message-forwarding/indirect-communication role (TS 29.500
§§6.10-6.11) remains entirely undesigned -- both explicitly scoped out this session, not silently
incomplete. Per ADR-0184's own process decision, this project moves to the next NF/subsystem
continuously; **Tier 2 NFs have now begun** (5G-EIR ADR-0187, SMSF ADR-0188, GMLC ADR-0189, LMF ADR-0191 -- the
first two chosen as the smallest/cleanest real candidates, genuinely closable in full rather than
a disclosed partial slice, unlike NEF/SCP above; GMLC's and LMF's own real scopes each split
between RF/GNSS/LPP-independent operations built complete and operations that honestly report
`501`/`404` rather than fabricate positioning/measurement data). The real per-file survey
performed before picking the first candidate (file counts from direct reads of
`specs/5G_APIs-REL-19/`, not estimated): NWDAF 10 files, DCCF 3, ADRF 3, MFAF 3, NSACF 2, TSCTSF 3,
EASDF 2, UCMF 2, **SMSF 1 file/5 ops (built, ADR-0188)**, **5G-EIR 1 file/1 op (built, ADR-0187)**,
**LMF 3 files/7 ops (built, ADR-0191)**, **GMLC 1 file/5 ops (built, ADR-0189)**, NSSAAF 2, AAnF 2,
UDSF 2, SEPP 2 -- 12 Tier 2 NFs (per CLAUDE.md's own scope list) remain unbuilt. Real, disclosed,
deferred wiring: GMLC's own `RequestLocation`/`CancelLocation` still don't call LMF -- LMF now
exists and its own `DetermineLocation` is real code, but GMLC->LMF HTTP wiring wasn't done this
turn, and LMF's own `DetermineLocation`/`LocationMeasure` remain honest `501`s regardless (the real
LPP/PRU capability gap is unaffected by wiring). Also, this pass found and fixed 2 real defects
while building LMF (ADR-0190): a vendored-spec transcription typo, and the third real
sbi-codegen generator bug found this way (after ADR-0022/ADR-0024).

## ADR-0193 full-project YAML coverage audit (2026-08-24)

Different axis from the free5GC/open5GS behavioral sweep above: this is a literal cross-reference
of every real `N<nf>_*` YAML in `specs/5G_APIs-REL-19/` against `libs/sbi-generated/CMakeLists.txt`'s
pilot set (wired vs. never-wired -- "Tier-A") and, for wired files, every real `operationId` against
that NF's actual routed handlers (real vs. stub vs. missing -- "Tier-B"). Triggered by the user
asking why `Nnrf_Bootstrapping` isn't implemented -- confirmed a real, undisclosed gap (the YAML
exists, was simply never added to the pilot set) -- then extended project-wide per ADR-0193's own
mandatory, ASAP, one-by-one audit-and-close directive. Performed by a single-pass background
agent, real grep/read evidence, not guessed; "needs closer look" reserved for anything a quick
check couldn't resolve (none surfaced this pass).

**Confirmed Tier-A gaps (whole API file never wired into codegen) by NF:**

| NF | Missing YAML file(s) |
|---|---|
| NRF | ~~`Nnrf_Bootstrapping`~~ CLOSED, ADR-0194 (the original finding that triggered this audit) |
| AMF | ~~`Namf_AIoT`, `Namf_MBSBroadcast`, `Namf_MBSCommunication`, `Namf_MT`~~ CLOSED, ADR-0200 |
| SMF | ~~`Nsmf_EventExposure`, `Nsmf_NIDD`~~ CLOSED, ADR-0201 |
| UDM | ~~`Nudm_MT`, `Nudm_NIDDAU`, `Nudm_RSDS`, `Nudm_SSAU`, `Nudm_UEID`~~ CLOSED, ADR-0202 |
| UDR | ~~`Nudr_GroupIDmap`~~ CLOSED, ADR-0198 -- was a different failure mode: implemented, but with hand-written `nlohmann::json` validation bypassing sbi-codegen entirely, now replaced with the real generated DTO |
| AUSF | ~~`Nausf_UPUProtection`~~ CLOSED, ADR-0195 |
| PCF | ~~`Npcf_EventExposure`, `Npcf_UEPolicyControl`, `Npcf_AMPolicyAuthorization`, `Npcf_MBSPolicyAuthorization`, `Npcf_MBSPolicyControl`, `Npcf_PDTQPolicyControl`, `Npcf_BDTPolicyControl`~~ CLOSED, ADR-0204/ADR-0205/ADR-0206 (task #163, all 7 files, three slices) |
| NEF | ~~`Nnef_SMService`, `Nnef_UEId`, `Nnef_DNAIMapping`, `Nnef_EASDeployment`~~ CLOSED, ADR-0207 (first slice); ~~`Nnef_SMContext`, `Nnef_Authentication`, `Nnef_ECSAddress`~~ CLOSED, ADR-0208 (second slice); ~~`Nnef_EventExposure`~~ CLOSED, ADR-0209 (third slice); ~~`Nnef_TrafficInfluenceData`, `Nnef_Inference`, `Nnef_Training`, `Nnef_VFLInference`, `Nnef_VFLTraining`~~ CLOSED, ADR-0210 (fourth slice) -- **all 13 of 13 closed, task #164 complete** |
| LMF | ~~`Nlmf_Broadcast`, `Nlmf_DataExposure`~~ CLOSED, ADR-0196 |
| UPF | ~~`Nupf_EventExposure`, `Nupf_GetUEPrivateIPaddrAndIdentifiers`~~ CLOSED, ADR-0203 -- was lower severity (UPF's real primary control interface is N4/PFCP, TS 23.501, not SBI) but a real gap: `nfs/upf/src/main.cpp`'s own file header had incorrectly claimed no `Nupf_*` API existed at all |

NSSF, BSF, SCP, CHF, EIR, SMSF, GMLC: **zero** Tier-A gaps found (single-file NFs already fully
wired, or CHF's deliberately narrow 3-file scope matching CLAUDE.md exactly).

**Related but distinct: wired-and-silently-empty ("looks real, isn't").** ~~AMF's `Namf_Location`
and `Namf_EventExposure` were both in the pilot set and generated real DTOs, but
`nfs/amf/src/main.cpp` routed zero operations for either~~ **CLOSED, ADR-0199**: all 9 real
operations across both now routed, with the real capability gaps that remain (no LPP/GNSS/PRU
positioning, no event notification delivery) disclosed explicitly in code and in ADR-0199, not
silently absent.

**Related but distinct: undisclosed Tier-B gaps inside NFs already marked "real."** Two concrete
factual errors, not just missing coverage -- **CLOSED, ADR-0197**: UDR's own ADR-0111/ADR-0114
comments asserted `operator-specific-data` has no PUT/DELETE in the YAML -- it does (both
`Subscription_Data.yaml` and `Policy_Data.yaml`, confirmed by direct read), and both now have real,
live-verified PUT/DELETE routes. Beyond that, UDR (the most mature NF by route count) had ~7 further
undisclosed missing resources in `Subscription_Data`: ~~`authentication-status/{servingNetworkName}`
(3 ops), bare `provisioned-data` GET (`QueryProvisionedData`)~~ **CLOSED, ADR-0212**. The third
claimed item, "`subs-to-notify` bulk-DELETE", was a stale/inaccurate claim in this audit doc
itself, corrected (not implemented) by ADR-0212: direct read of `TS29505_Subscription_Data.yaml`
confirms no such operation exists in the real spec (the collection is `POST`-only; the individual
resource is `GET`/`PATCH`/`DELETE`), and the entire `subs-to-notify` resource is already fully
implemented (task #122). `Subscription_Data`'s own Tier-B item is now fully closed. ~~~12 in
`Policy_Data` (bare `{ueId}` aggregate, `sm-data/{usageMonId}` family, bare `bdt-data` collection
GET, and an entire `subs-to-notify` subscription family distinct from `Subscription_Data`'s
own)~~ **CLOSED, ADR-0213 -- UDR now has zero known Tier-B gaps.**
~~NRF's 4 undisclosed missing operations (`OptionsNFInstances`, `RetrieveStoredSearch`,
`RetrieveCompleteSearch`, `RetrieveKeyRequest`)~~ **CLOSED, ADR-0211** (the audit's separately-flagged
"stale `ScpDomainRoutingInfo*` disclosure comment" was checked directly and found already accurate
in the current source -- not the "SCP isn't built yet" phrasing this audit's own original snapshot
had flagged, no fix needed).
UDM has the largest raw undisclosed-op count -- **UDM is now the only NF with a known real Tier-B
item project-wide.** A background audit (fork) produced the first real per-operation breakdown
(replacing the earlier rough counts): ~~`Nudm_UEAU` (3 ops: `GetRgAuthData`, `GenerateAv`,
`GenerateGbaAv`)~~ **CLOSED, ADR-0214** (first slice, smallest/clearest); `Nudm_UECM` (~24 ops:
`GetRegistrations` bare aggregate, `SendRoutingInfoSm`, ~~`PeiUpdate`/`UpdateRoamingInformation`~~
**CLOSED, ADR-0215** (second slice -- the two confirmed independent of the other still-missing
sub-resources below), ~~the full non-3gpp-access triple~~ **CLOSED, ADR-0216** (third slice --
`Non3GppRegistration`/`GetNon3GppRegistration`/`UpdateNon3GppRegistration`, mirrors the
already-built `amf-3gpp-access` group's own real shape), ~~smsf-3gpp/non-3gpp-access groups~~
**CLOSED, ADR-0217** (fourth slice -- both groups share the identical real
`SmsfRegistration`/`SmsfRegistrationModification` schemas), ~~ip-sm-gw-registration group~~
**CLOSED, ADR-0218** (fifth slice -- `IpSmGwRegistration`/`GetIpSmGwRegistration`/
`IpSmGwDeregistration`, genuinely no PATCH exists for this resource in the real spec at all),
~~nwdaf-registrations group~~ **CLOSED, ADR-0219**
(sixth slice -- `NwdafRegistration`/`GetNwdafRegistration`/`NwdafDeregistration`/
`UpdateNwdafRegistration`; real finding: no individual GET exists for a single NWDAF registration
in the real spec at all, only the collection GET), ~~`GetRegistrations`~~
**CLOSED, ADR-0220** (seventh slice -- composes `RegistrationDataSets` from the six real per-group
stores, gated by the required `registration-dataset-names` query param, real spec `minItems: 2`
enforced as a real `400`; disclosed: `single-nssai`/`dnn` filtering of the `SMF_PDU_SESSIONS`
dataset not honored), ~~`SendRoutingInfoSm`~~ **CLOSED, ADR-0221** (eighth CRUD/
composition slice -- composes `RoutingInfoSmResponse` from the real SMSF/IP-SM-GW stores;
disclosed: `smsRouter`/`ipSmGwGuidance` never populated, no data source for either), ~~`Trigger
P-CSCF Restoration`, `GetLocationInfo`, `authTrigger`~~ **CLOSED, ADR-0227** (ninth and final
slice -- `GetLocationInfo` is a real, complete local composition from the already-stored AMF
registration records; the other two are real accept-and-validate operations with a disclosed
non-relay to the serving AMF/AUSF, same class as ADR-0207's own SendSMS disclosure) --
**`Nudm_UECM`'s entire Tier-B backlog is now fully closed**, confirmed by direct read of every
operation's own real response schema, not assumed);
`Nudm_SDM` (37 real ops, real-scoped in detail against TS29503_Nudm_SDM.yaml directly rather than
the earlier rough "~33" estimate: 3 already wired -- `GetAmData`/`GetSmfSelData`/`GetSmData`;
~~4 more individual-resource GETs backed by UDR's own existing individual routes
(`GetSmsData`/`GetSmsMngtData`/`GetTraceConfigData`/`GetLcsBcaData`) plus 9 individual-resource
GETs backed only by UDR's bulk `ProvisionedDataSets` aggregate, no individual UDR route of their
own (`GetLcsPrivacyData`/`GetLcsMoData`/`GetLcsSubscriptionData`/`GetV2xData`/`GetProseData`/
`GetMbsData`/`GetUcData`/`GetA2xData`/`GetRangingSlPrivacyData`)~~ **CLOSED, ADR-0228** (13-op
combined slice -- confirmed real, disclosed: none of the 9 bulk-backed ops expose a `plmn-id`
query param in the real spec, so UDM's existing `kDefaultServingPlmnId` single-PLMN-lab fallback
is reused to satisfy UDR's bulk-route's own required `servingPlmnId` path param; `GetUcData`'s
real optional `uc-purpose` filter is accepted-but-unhonored, matching this project's own
established precedent for other unhonored optional filters); ~~group C1: `GetNSSAI`
(a real subset view into already-fetched am-data's own `nssai` field, same schema),
`GetUeCtxInAmfData`/`GetUeCtxInSmfData` (real, near-complete local composition from the
already-stored AMF/SMF registration records, same pattern ADR-0227's `GetLocationInfo`
established), `GetUeCtxInSmsfData` (real, COMPLETE local composition from the already-stored SMSF
registration records), `GetEcrData` (UDR's own existing `coverage-restriction-data` route under a
nested path)~~ **CLOSED, ADR-0230** (5-op slice -- real, disclosed narrowing:
`PduSession.dnn` is required while the source `SmfRegistration.dnn` is optional, so a registration
with no `dnn` is skipped rather than fabricated; `epsInterworkingInfo`/`pgwInfo`/`emergencyInfo`
remain real, disclosed gaps with no data source); ~~`Modify`~~ **CLOSED, ADR-0232** (real RFC 7396
merge-patch, same shape this file's other PATCH routes already use -- completes
`Subscribe`/`Unsubscribe`/`Modify` in full); ~~group C2: `GetTimeSyncSubscriptionData`/
`GetRangingSlPosData`~~ **CLOSED, ADR-0233** (real, disclosed correction of ADR-0230's own scoping
note -- UDR already had both resources fully live, no new UDR stores were actually needed, only
UDM-side wiring; closes `Nudm_SDM`'s entire group C in full); ~~`SorAckInfo`/`UpuAck`/
`S-NSSAIs Ack`/`CAG Ack`~~ **CLOSED, ADR-0234** (4 of 5 real SOR/UPU/ack write ops, real
accept-and-validate); ~~6-op shared-data family: `GetIndividualSharedData`/`GetSharedData`/
`GetGroupIdentifiers`/`SubscribeToSharedData`/`UnsubscribeForSharedData`/`ModifySharedDataSubs`~~
**CLOSED, ADR-0235** (`GetIndividualSharedData`/`GetGroupIdentifiers` real proxies to UDR's
already-live routes, ADR-0110/ADR-0140; `GetSharedData` composed at UDM via N real UDR calls since
UDR itself never built the bulk endpoint; subscription CRUD backed by a new, genuinely global
UDM-local `SharedDataSubscriptionStore`); ~~2 identifier-lookup ops: `GetSupiOrGpsi`/
`GetMultipleIdentifiers`~~ **CLOSED (forward direction), ADR-0236** (real `gpsis` field added to
UDR's seeded am-data; SUPI->GPSI direction genuinely real for both ops; GPSI->SUPI direction
remains a real, disclosed architectural gap -- no query-by-gpsi capability exists anywhere in the
real Nudr_DR API, and this project's NFs talk only over SBI, so it can't be built without either
inventing an API or duplicating UDR's data in UDM); leaving only `Update SOR Info` open (real KDF
primitive already exists in `libs/aka-crypto`, but a real CounterSoR state machine and real
steering list content do not -- disclosed, not stubbed) to fully close `Nudm_SDM`);
~~`Nudm_PP` (11 ops: the 5g-vn-groups/pp-data-store/mbs-group-membership groups already flagged
deferred in ADR-0082)~~ **CLOSED, ADR-0237** (all 3 real UDR backing resources were already fully
live -- 9 of 11 ops are real, direct proxies; the 2 `Modify` ops are a real RFC
7396-over-RFC-6902 translation via GET+merge_patch+PUT against UDR). Every one was a real,
individually small CRUD/subscription operation within already-wired files, not a structural gap.
**This closes `Nudm_PP` in full and, with it, UDM's entire Tier-B gap-closure backlog except
`Update SOR Info`.**

### Prioritization for closure (per ADR-0193's continuous, one-at-a-time process)

Smallest/clearest first, since they're genuinely closable in full rather than a partial slice:
`Nnrf_Bootstrapping` (1 operation, no request body) -- CLOSED, ADR-0194 -- was the natural first
item, and literally what prompted this audit. `Nausf_UPUProtection` (1 operation) -- CLOSED,
ADR-0195 -- was similarly small. The two `Nlmf_*` files (1 + 3 operations) -- CLOSED, ADR-0196 --
were next. The two UDR
comment-vs-spec factual errors (`operator-specific-data` PUT/DELETE) -- CLOSED, ADR-0197 -- were
fixed directly rather than left as backlog triage, since they were an active documentation defect,
not missing coverage. NEF's 13-file/~45-operation
surface and PCF's 7-file surface are the largest single blocks of remaining work in the project
and will need to be split across multiple turns even under the "one subsystem per turn" framing
already used for NEF/SCP/BSF.

## ADR-0294 sweep: the NFs the original pass never covered (2026-09-05)

The Summary table above says, of NSSF/BSF/NEF/SCP/5G-EIR/SMSF/GMLC/LMF, "Not yet swept against
free5GC/open5GS source". That line stood for months. This section closes it.

**Method, same as the original pass:** open5GS from the real `open5gs-main.zip` monorepo
(`src/<nf>/`), free5GC cloned live per-NF (`github.com/free5gc/{nssf,bsf,nef}.git`). Every claim
below cites what was actually read.

**First real finding, before any per-NF comparison:** of the eight NFs, **only four have a
reference to sweep against at all.**

| NF | free5GC | open5GS |
|---|---|---|
| NSSF | real (`f5-nssf`, 26 Go files) | real (`src/nssf`, 1,734 lines) |
| BSF | real (`f5-bsf`, 22 Go files) | real (`src/bsf`, 1,700 lines) |
| NEF | real (`f5-nef`, 35 Go files) | **does not exist** |
| SCP | **does not exist** | real (`src/scp`, 2,279 lines) |
| 5G-EIR | **does not exist** | **does not exist** |
| SMSF | **does not exist** | **does not exist** |
| GMLC | **does not exist** | **does not exist** |
| LMF | **does not exist** | **does not exist** |
| NSACF | **does not exist** | **does not exist** |

The bottom five are capabilities neither reference implementation has. Per ADR-0075's mandate
this is recorded as a real lead, not as a victory lap: "no reference exists" says nothing about
whether ours is *correct*, only that there is nothing to compare it against. Their own disclosed
gaps (ADR-0187/0188/0189/0191: no IMEI provisioning path, no SMS-GMSC relay, no LPP/PRU
positioning) are unaffected by this.

### NSSF -- one real, previously undisclosed gap, and one place we are ahead

Ours implements all 8 real operations across both `Nnssf_NSSelection` and
`Nnssf_NSSAIAvailability`, and accepts all five real slice-info request types. open5GS handles
**only** `slice-info-request-for-pdu-session` -- it returns `400 "Not implemented except PDU
session"` for anything else (`src/nssf/nnssf-handler.c`) -- and has **no `Nnssf_NSSAIAvailability`
service at all** (grep for `nssai-availability` over `src/nssf/` returns nothing). We are
genuinely ahead of open5GS on service surface.

free5GC is the deeper implementation, and the comparison is where the real finding is.
`nsselection_network_slice_information.go` is 747 lines over a 539-line `util.go`. Ours is one
function, `decide_slice_selection`, that filters the requested S-NSSAIs against a fixed seed
catalog. What free5GC does that we do not:

1. **The TAI is ignored entirely.** free5GC has `CheckSupportedSnssaiInTa`,
   `CheckSupportedSnssaiInAmfTa`, `GetRestrictedSnssaiListFromConfig(tai)`,
   `AuthorizeOfAmfTaFromConfig`. Our selection never reads the `tai` the request carries. A slice
   restricted in one tracking area is allowed everywhere.
2. **`nsiInformationList` is never returned.** This is the field that tells the AMF which NRF
   serves the selected network slice instance, and it is the *entire point* of open5GS's NSSF
   (`nssf_nsi_find_by_s_nssai` → `NsiInformation`). **Both** references produce it; we produce
   none. This is the most consequential item here.
3. **Our own NSSAIAvailability store is never consulted during selection.** We accept per-AMF
   `SupportedNssaiAvailabilityData`, store it, authorize it and fire real notifications on it --
   and then the selection decision ignores it and consults a static seed instead. free5GC's
   `GetSupportedSnssaiListFromConfig(nfId, tai)` reads exactly this data. This is data we already
   hold, unused.
4. **No serving↔home S-NSSAI mapping**, so roaming selection is not modeled
   (`FindMappingWithServingSnssai`/`FindMappingWithHomeSnssai`,
   `GetMappingOfPlmnFromConfig`).
5. **No `candidateAmfList`/`targetAmfSet`** in the response (`AddAmfInformation`) -- so NSSF can
   never trigger an AMF relocation, which is one of its two real jobs.
6. **`rejectedNssaiInTa` is never produced**; we only ever emit `rejectedNssaiInPlmn`.

ADR-0183 disclosed item (1)-adjacent language ("not real subscriber-entitlement/NRF-discovery/
NSAG-mapping logic"). Items 2, 3, 5 and 6 are **newly named here** and are more specific than that
disclosure was.

### BSF -- full parity, and the reference is the one that diverges from the spec

All 15 operations in `TS29521_Nbsf_Management.yaml` are implemented, and the query filters on
`GetPCFBindings` (`ipv4Addr`, `ipv6Prefix`, `macAddr48`, `dnn`, `supi`, `gpsi`, `ipDomain`,
`snssai`) match the spec's own parameter list. free5GC covers the same four resource families
(`pcfBindings`, `pcf-ue-bindings`, `pcf-mbs-bindings`, `subscriptions`) plus one extra --
`GetIndPCFBinding` (`GET /pcfBindings/{bindingId}`) -- **which does not exist in the R19 YAML**
(the individual resource is DELETE/PATCH only). That is free5GC carrying an older or extended
shape, not a gap on our side; noted rather than copied, per this project's YAML-is-the-source rule.

**No real gap found for BSF.** First NF in this project for which that is true against a real
reference.

### NEF -- the largest undisclosed gap this sweep found

This project's NEF implements all 14 `Nnef_*` YAML files (ADR-0185 through ADR-0210). That is the
*NF-facing* half. The **AF-facing half -- which is what "Network Exposure" names -- is entirely
absent**, and free5GC has it:

- free5GC mounts `/3gpp-traffic-influence/v1` (TS 29.522) and `/3gpp-pfd-management/v1`
  (TS 29.122) as real, OAuth2-protected AF-facing route groups (`internal/sbi/server.go`,
  `pkg/factory/config.go`).
- Those YAMLs are **already in this repository**: 16 `TS29122_*.yaml` files and 44 `TS29522_*.yaml`
  files under `specs/5G_APIs-REL-19/`. Of the 60, exactly **2 are wired into
  `libs/sbi-generated/CMakeLists.txt`** (`TS29522_DNAIMapping`, `TS29522_ECSAddress`) and both only
  as DTO sources for an `Nnef_*` SBI service -- neither is served as a northbound API. **58 real
  spec files, present on disk, never built.**
- **Why this was never caught:** ADR-0193's project-wide YAML coverage audit cross-referenced
  `N<nf>_*` files only. `TS29122_*`/`TS29522_*` do not match that pattern, so the audit's own
  "zero Tier-A gaps" verdict for NEF was scoped narrower than it read. Correcting the audit's
  scope, not just its result.

Second, structural: **our NEF has no outbound consumer at all.** Its only HTTP client traffic is
NRF registration and heartbeat (`nfs/nef/src/main.cpp:250-320`); everything else is stored in
in-process maps. free5GC's NEF really calls UDR (`AppDataInfluenceDataGet/Put`, `AppDataPfdsGet`)
and PCF. A NEF that stores traffic-influence data locally instead of writing it to UDR does not
influence anything -- the SMF/PCF that would act on it never see it. This is the same
"server with no consumer" failure class ADR-0293 just closed for UDM's MAP client.

### SCP -- the known gap, now corroborated with evidence

ADR-0186 recorded that SCP's real message-forwarding role (TS 29.500 §§6.10-6.11) is unbuilt and
was scoped out by explicit user decision. open5GS confirms the size of what that defers:
`src/scp/sbi-path.c` is **1,299 lines** of real request forwarding with delegated discovery --
parsing `3gpp-Sbi-Discovery-*` headers into a discovery option, preserving the original consumer's
NF type on the forwarded request, and routing to the resolved producer. Ours implements
`Nscp_EventExposure` (3 operations) and nothing else.

Unchanged as a decision; recorded here so the deferral has a measured size rather than an
adjective.

### What this sweep did NOT do

**No performance comparison was run, here or anywhere.** This is a capability sweep -- what each
implementation *does*, read from its source. P10's "exceeds free5GC" claim from the
commercialization mandate (ADR-0049) still has **zero** measurements behind it, and nothing in
this section changes that. Blocker 3 in `docs/COMPLIANCE_P1_P15.md` stands exactly as written.
