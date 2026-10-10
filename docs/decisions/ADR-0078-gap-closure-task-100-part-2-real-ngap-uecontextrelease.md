## ADR-0078: gap-closure task #100 (part 2) -- real NGAP UEContextRelease{Request,Command,Complete}

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s AMF section flagged zero N2 handover support and, during
ADR-0076's own live-interop verification, a concrete, newly-discovered blocker: UERANSIM's real
gNB-initiated idle-mode-reentry trigger (`nr-cli <gnb> --exec 'ue-release <id>'`) sends a real
NGAP `UEContextReleaseRequest` this AMF could not decode at all (`amf-ngap: failed to decode NGAP
PDU (25 bytes), ignoring: 002a4015...`). This ADR closes that specific gap: real
`UEContextReleaseRequest`/`UEContextReleaseCommand`/`UEContextReleaseComplete` (TS 38.413
§8.3.3/§9.2.1.9-11). Full N2 handover (`HandoverRequired`/`HandoverRequest`/`HandoverCommand`/
`PathSwitchRequest`/etc.) remains open -- a separate, larger body of work, not attempted here (see
"What this ADR does NOT include" below).

### The real ASN.1 module change this required

Per ADR-0031, this project's NGAP codec works around a confirmed asn1c 0.9.29 limitation (real
IOC/parameterized-type resolution failure) by repointing specific message definitions in
`specs/NGAP/ngap-17.9.asn` at a project-added `ConcreteProtocolIE-Container` type in place of the
real spec's parameterized `ProtocolIE-Container {{XxxIEs}}`. Before this ADR, only six message
types were patched this way (`NGSetupRequest`/`Response`/`Failure`, `InitialUEMessage`,
`DownlinkNASTransport`, `UplinkNASTransport`). Extended to three more:
`UEContextReleaseRequest`, `UEContextReleaseCommand`, `UEContextReleaseComplete` -- same patch
shape, same disclosed rationale, both the per-message comments and the shared
`ConcreteProtocolIE-Container` doc comment updated to list all nine. Confirmed via a real asn1c
regeneration (`cmake --build . --target ngap_generated`) that the patched module still compiles
cleanly and produces the expected `UEContextReleaseRequest.h`/`UEContextReleaseCommand.h`/
`UEContextReleaseComplete.h` with real, usable (non-empty) IE containers.

### What's built (`nfs/amf/src/ngap_task.cpp`)

Real, RAN-initiated release round trip only (the direction a real gNB actually exercises for
idle-mode re-entry / O&M-triggered release):
1. `handle_ue_context_release_request`: decodes the gNB's `UEContextReleaseRequest`
   (AMF-UE-NGAP-ID, RAN-UE-NGAP-ID mandatory; `Cause` decoded best-effort for logging only -- no
   cause-driven behavior branch, a real, disclosed scope narrowing), then replies with
   `UEContextReleaseCommand` carrying `UE-NGAP-IDs` (the `aMF-UE-NGAP-ID` CHOICE arm) and
   `Cause=nas/normal-release` -- this lab's own network-triggered-release choice, not a value
   taken from the request's own cause.
2. `handle_ue_context_release_complete`: decodes the gNB's confirming `UEContextReleaseComplete`
   (`SuccessfulOutcome`, procedureCode 41), unregisters the UE from `NgapUeRegistry`, and resets
   `auth_state` to its default value. **Real, disclosed behavior decision**: this resets the
   per-association `UeAuthState` rather than tearing down the SCTP association itself, so the
   same association can serve a fresh UE context afterward -- matching a real gNB's own behavior
   of keeping one association open across many UE contexts. This lab's own "one UE at a time per
   association" scope (ADR-0031) becomes "one at a time, but the association itself now survives
   a release," a real (if still simplified) improvement, not a new limitation.

**Real, disclosed scope boundary, not silently dropped**: the AMF-INITIATED direction (an AMF
that decides on its own -- e.g. after processing a Deregistration -- to send
`UEContextReleaseCommand` unprompted) is NOT implemented; this lab has no such trigger yet, and
only the RAN-initiated request/command/complete round trip was in scope for this pass.

### Verification -- full real UERANSIM interop, the exact scenario that found this gap

Re-ran the identical scenario ADR-0076's own live-interop pass used when it first hit this
blocker: full lab stack (nrf/udr/udm/ausf/chf/pcf/smf/upf/amf) + real `nr-gnb` + real `nr-ue`
(`imsi-999700000000001`) through a complete Initial Registration + PDU Session Establishment,
then `nr-cli UERANSIM-gnb-999-70-1 --exec 'ue-release 1'`. Real, observed, successful outcome
(not simulated):
- gNB log: "Sending UE Context release request (NG-RAN node initiated)" -> "UE Context Release
  Command received" -> "Releasing RRC connection for UE[1]".
- AMF log: "UEContextReleaseRequest for AMF-UE-NGAP-ID=1, RAN-UE-NGAP-ID=1, cause group=1" ->
  "sent UEContextReleaseCommand (18 bytes) ... Cause=nas/normal-release" -> "UEContextRelease
  Complete received ... UE context released, association ready for a new UE context".
- UE log: "RRC Release received" -> "UE switches to state [CM-IDLE]".
- `nr-cli UERANSIM-gnb-999-70-1 --exec 'ue-list'` before: one entry (`ue-id: 1`); after: empty --
  confirms the gNB's own UE context was genuinely released, not just an AMF-side log line.

`cmake --build . --target amf` succeeded on the first real build with zero errors (the asn1c
regeneration + new C++ handler code, no iteration needed). Full `conformance_tests` and
`integration_tests` both rebuilt clean afterward with no source changes required elsewhere.

### What this ADR does NOT include

Full N2 handover (`HandoverRequired`/`HandoverRequestAcknowledge`/`HandoverCommand`/
`HandoverNotify`/`HandoverCancel`/`PathSwitchRequest`) -- free5GC's ~39-procedure NGAP coverage
vs this project's now-6 real procedures (`NGSetupRequest`, `InitialUEMessage`,
`UplinkNASTransport`, and now `UEContextReleaseRequest`/`Command`/`Complete`) still leaves a real,
large gap, tracked as the remainder of task #100/#101. The AMF-initiated `UEContextRelease`
direction (see above). `PDUSessionResourceListCxtRelReq`/`PDUSessionResourceListCxtRelCpl` (the
optional PDU-session-list IEs on the request/complete messages) -- not decoded or acted on; this
lab's single-PDU-session-per-UE scope makes them unnecessary for now, same disclosed narrowing
pattern as other optional IEs skipped elsewhere in this codebase. Cause-driven response behavior
(e.g. distinguishing a radio-link-failure release from an O&M-triggered one) -- logged, not acted
on differently.

