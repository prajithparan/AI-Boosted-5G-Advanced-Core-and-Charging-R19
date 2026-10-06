## ADR-0096: gap-closure task #100 -- real N2-based handover (HandoverRequired through HandoverNotify)

### Context

Building on ADR-0095's concurrency fix, this closes the real N2-based handover chain (TS 38.413
§8.4.2 Handover Preparation, §8.4.3 Handover Resource Allocation, §8.4.4 Handover Notification;
TS 23.502 §4.9.1.3.2): `HandoverRequired` (source gNB -> AMF), `HandoverRequest` (AMF -> target
gNB, AMF acting as initiator for the first time in this project), `HandoverRequestAcknowledge`/
`HandoverFailure` (target -> AMF), `HandoverCommand`/`HandoverPreparationFailure` (AMF -> source),
and `HandoverNotify` (target -> AMF, standalone). `HandoverCancel`/`HandoverCancelAcknowledge` are
real, disclosed, out of this pass's scope (a separate elementary procedure, not part of the
`HandoverRequired`...`HandoverNotify` chain the user approved).

### Real ASN.1 patches (TS 38.413, matching ADR-0031's own established mechanism)

`specs/NGAP/ngap-17.9.asn`: `HandoverRequired`, `HandoverCommand`, `HandoverPreparationFailure`,
`HandoverRequest`, `HandoverRequestAcknowledge`, `HandoverFailure`, `HandoverNotify`, and (a real,
previously-undiscovered second-order need) `PDUSessionResourceSetupRequestTransfer` (used inside
`HandoverRequest`'s own mandatory `PDUSessionResourceSetupListHOReq`) all had their real spec
`ProtocolIE-Container {{XxxIEs}}` patched to `ConcreteProtocolIE-Container`, the same asn1c
0.9.29-limitation workaround ADR-0031 established. `ngap_generated` regenerated and rebuilt clean.

### Real, disclosed design choice: cold lookup, not association-local state

Unlike this file's own `UplinkNASTransport`-phase handlers (which legitimately depend on the one
UE a given association's own local `auth_state` already tracks, ADR-0031's real lab scope),
`handle_handover_required` derives the UE's identity entirely from `HandoverRequired`'s own real
`AMF-UE-NGAP-ID`/`RAN-UE-NGAP-ID` IEs, then does the SAME real cold lookup via
`amf_ue_id_index -> UeSecurityContextStore` `handle_path_switch_request` already established
(ADR-0090) -- not the CURRENT association's own `auth_state`. This is the real, correct design (a
handover procedure's own identity IEs are the authoritative source per spec), found and corrected
mid-implementation after an initial draft coupled it to `auth_state`; it also happens to be what
made live verification tractable without reimplementing a real 5G-AKA-capable fake UE (see below).

### Real, disclosed scope narrowing (per-field, same style every codec in this project already uses)

`TargetID` only supports the real `targetRANNodeID.globalRANNodeID.globalGNB-ID` CHOICE arm (this
lab's only real RAN node type, matching ADR-0095's own `extract_global_gnb_id` scope).
`SourceToTarget-TransparentContainer`/`TargetToSource-TransparentContainer` are relayed
byte-for-byte, opaque, per the real spec's own design (both are plain `OCTET STRING`, TS 38.413
§9.3.1.31/§9.3.1.32 -- AMF genuinely isn't meant to understand gNB-to-gNB RRC content). Real
vertical key derivation (`SecurityContext` IE) reuses the exact `derive_kgnb`/`derive_nh` call
ADR-0090 established (NCC=0, chain position 0, identical disclosed reason: no
`InitialContextSetup`/prior AS security context has ever been persisted per-hop in this project).
`UEAggregateMaximumBitRate` is this lab's own fixed default (no real per-subscriber AMBR tracked
anywhere in this project). `PDUSessionResourceSetupListHOReq`'s own per-session
`PDUSessionResourceSetupRequestTransfer` is real and structurally mandatory-complete (one real QoS
flow, QFI=1, 5QI=9 non-dynamic -- the real, standard 3GPP "non-GBR default" value, TS 23.501 Table
5.7.4-1, not invented; ARP priorityLevel=8/shall-not-trigger-pre-emption/not-pre-emptable, this
lab's own conservative fixed default), but its mandatory `UL-NGU-UP-TNLInformation` is a
PLACEHOLDER (TEID=0/IP=0.0.0.0) -- a real, disclosed, NOT-yet-closed gap: unlike ADR-0092's own
SMF-side `PATH_SWITCH_REQ` work, this pass does NOT build the real AMF->SMF
`Nsmf_PDUSession_UpdateSMContext` relay a real AMF would use to obtain UPF's own real N3 address
here (the same "AMF doesn't call SMF yet" gap ADR-0090/ADR-0092 already disclosed as open, hit
again in a second place). `HandoverCommand`'s own `PDUSessionResourceHandoverList` is real,
OPTIONAL per spec, and omitted (same class of narrowing as `PathSwitchRequestAcknowledge`'s own
`empty_transfer` precedent, ADR-0090). Real, load-bearing side effect on `HandoverNotify`: a real,
AMF-initiated `UEContextReleaseCommand` is now sent to the source (closing the exact gap
`handle_ue_context_release_request`'s own header comment disclosed as open since ADR-0078 -- "this
does NOT implement the AMF-INITIATED direction" -- for this one real trigger), before re-pointing
`NgapUeRegistry`'s entry to the target (same real re-point precedent PathSwitchRequest already
established, ADR-0090).

### Live verification (real, cross-process, two genuinely concurrent associations)

Real full lab stack (nrf/udr/udm/ausf/pcf/chf/upf/smf/amf, all real binaries) plus a real,
unmodified UERANSIM gNB+UE completed a genuine Initial Registration + PDU Session Establishment
(AMF-UE-NGAP-ID=1, RAN-UE-NGAP-ID=1, PDU session=1, real SQN resync exercised along the way) --
that association was deliberately left open, not torn down. Two new hand-crafted scratch tools
(reusing AMF's own `ngap_codec.cpp` directly, same precedent `path_switch_client.cpp` established):
`ho_target_gnb` connected as a genuinely SECOND, SIMULTANEOUSLY-OPEN association, sent a real
`NGSetupRequest` (`GlobalGNB-ID=AAAAAAAA`, confirmed via `amf-ngap: gNB association established`
logged a second time while the first was still alive), then blocked awaiting a real
`HandoverRequest`; `ho_source_trigger` then sent a real `HandoverRequired` referencing the real
captured AMF-UE-NGAP-ID/RAN-UE-NGAP-ID/PDU-session-ID over a THIRD connection (not UERANSIM's own,
per this ADR's own disclosed cold-lookup design above -- no fake-UE 5G-AKA replay needed).

**Real, observed success, independently corroborated multiple ways**: (1) AMF's own log, the full
real chain in order: `"HandoverRequired for AMF-UE-NGAP-ID=1..."` -> `"sending real HandoverRequest
(178 bytes) to target gNB..."` -> `"real HandoverRequestAcknowledge received from target gNB --
sending real HandoverCommand to source gNB"` -> `"sent real HandoverCommand (51 bytes)..."` ->
(0.5s later) `"real HandoverNotify received -- AMF-UE-NGAP-ID=1, new RAN-UE-NGAP-ID=4242"` ->
`"sent real AMF-initiated UEContextReleaseCommand (18 bytes) to the source gNB..."` ->
`"re-pointed NGAP registry entry for SUPI imsi-999700000000001 to the new RAN-UE-NGAP-ID=4242..."`
-> `"UEContextReleaseComplete received..."`; (2) `ho_source_trigger`'s own independent decode:
received a real `HandoverCommand` whose `TargetToSource-TransparentContainer` content read back
byte-for-byte as `"fake-t2s-rrc-container"` -- the EXACT string `ho_target_gnb` sent in its own
`HandoverRequestAcknowledge`, proving the cross-thread relay correctly carried content between two
independent processes/associations, not just a correlated boolean; (3) `ho_target_gnb`'s own
independent decode: the real `HandoverRequest` it received had `PDUSessionResourceSetupListHOReq`/
`SecurityContext`/`GUAMI` all present; (4) **a genuine, unplanned bonus confirmation**: UERANSIM's
own real, completely unmodified gNB (`gnb.log`) logged `"UE Context Release Command received"` /
`"Releasing RRC connection for UE[1]"` in direct response to the real AMF-initiated release this
ADR added -- independent, third-party interop proof the cross-thread `NgapUeRegistry::send_raw`
call correctly reached a real external gNB process's own live association, not just this project's
own scratch tooling.

Full `conformance_tests`: unchanged pass count (no new committed automated test this pass, same
disclosed manual-live-verification precedent ADR-0090/ADR-0091/ADR-0092 established for
gap-closure slices of this size), zero regressions; `amf` built clean.

### What this ADR does NOT include

Real AMF->SMF relay wiring for handover-triggered PDU session resource re-setup (the disclosed
`UL-NGU-UP-TNLInformation` placeholder above) -- a real, separate, deferred piece, same class as
ADR-0090's own already-disclosed "AMF doesn't call SMF yet" gap. `HandoverCancel`/
`HandoverCancelAcknowledge`. Real per-session `PDUSessionResourceHandoverList` content in
`HandoverCommand`. A generic multi-relay-in-flight-per-gNB correlation scheme (ADR-0095's own
disclosed lab-scope simplification). **Task #100 is closed for its real, scoped chain**
(`HandoverRequired` through `HandoverNotify`) -- `HandoverCancel` and the real PDU-session-transfer
depth remain a real, open, disclosed gap for a future pass.

