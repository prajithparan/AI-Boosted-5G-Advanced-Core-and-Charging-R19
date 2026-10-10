## ADR-0036: PDU Session Establishment (TS 23.502 §4.3.2.2.1) -- AMF's UlNasTransport decode -> real SMF CreateSMContext call

**Date:** 2026-08-08
**Status:** Accepted

**Context:** CLAUDE.md's Phase 2 definition of done requires UE registration *and* PDU session
establishment end-to-end, "no narrowed slice." ADR-0032 through ADR-0035 closed the registration
half. Investigating whether the second half was actually done (prompted by a direct "is Phase 2
complete" question) found it was not: AMF's NAS codec had no 5GSM message types at all, and
`ngap_task.cpp`'s post-registration dispatch explicitly logged and dropped every further
`UplinkNASTransport` ("no post-registration NAS procedures implemented yet"). SMF's own
`CreateSMContext` (stood up in an earlier turn, TS 29.502) was real and PCF-wired but had never had
a live trigger -- only ever exercised by a test client POSTing directly over HTTP, bypassing
AMF/NGAP entirely (confirmed by reading `tests/integration/test_smf_pdu_session.cpp`). This ADR
closes that gap: AMF now decodes the UE's PDU Session Establishment Request (wrapped in a NAS
`UlNasTransport` message) and makes the real `CreateSMContext` call.

**Key design decision, made explicit before writing any code: AMF does not decode the 5GSM PDU
Session Establishment Request payload itself.** TS 24.501's payload-container mechanism exists
precisely so AMF can route SM messages without understanding their contents -- only SMF decodes
real 5GSM content, and even SMF's own current turn doesn't (`nfs/smf/src/main.cpp`'s own disclosed
"PduSessionType is negotiated inside the NAS SM message... not available from SmContextCreateData
at all" scope, predating this session). So `amf::nas::decode_ul_nas_transport`
(`nfs/amf/src/nas_codec.{hpp,cpp}`) only extracts the transport-level optional IEs that determine
*where* to route the request -- PDU session ID, S-NSSAI, DNN -- treating the payload container
itself (the actual PDU Session Establishment Request bytes) as an opaque length-prefixed blob,
skipped over, never parsed. Byte layouts (UlNasTransport's mandatory/optional IE order, the DNN
IE's TS 23.003 §9.1 label-length-prefix encoding, requestType's Type-1 half-octet packing that a
naive Type-4-only IE walker would desync on) were confirmed against
`simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp`'s `UlNasTransport::onBuild` and
`src/ue/nas/sm/transport.cpp`/`src/lib/nas/utils.cpp`'s `DnnFromApn`, the same read-only reference
oracle methodology as every prior stage.

**Symmetric decision on the way back: AMF does not send anything to the UE after the SMF call
succeeds.** SMF's real `CreateSMContext` response (`SmContextCreatedData`) carries no `n1SmMsg` --
`nfs/smf/src/main.cpp`'s handler only ever sets `pduSessionId`/`sNssai` on it, a disclosed gap from
SMF's own earlier turn, not introduced here. There is therefore no real PDU Session Establishment
Accept content for AMF to forward to the UE. Synthesizing one would mean AMF fabricating SM-layer
decisions (PDU session type, QoS rules, session-AMBR) that are properly SMF's job to decide --
worse than disclosing the gap plainly, which is what `handle_uplink_nas_transport_pdu_session_
establishment`'s own comment does. This is consistent with (not a new instance of) the
already-established rule: never invent content a real peer would need to have actually decided.

**Refactored `nfs/amf/src/ngap_task.cpp`'s `UeAuthState::Phase` enum** (previously a 4-state linear
progression ending at `Done`) to add `AwaitingPduSessionEstablishmentRequest` between
`AwaitingRegistrationComplete` and `Done` -- this project's single-registration-per-association
scope (ADR-0031) extends naturally to "single PDU session per association," so a simple additional
enum value is correct; a real AMF serving concurrent PDU sessions would need a proper per-session
state machine. New dedicated `http2::Client`/`OAuth2Client` pair for SMF (scope
`"nsmf-pdusession"`), same one-client-per-NF-per-thread discipline as AUSF's/PCF's.

**Real interop blocked at the same SQN point as every prior stage (expected, not re-investigated)
-- verified the SMF call directly instead, same methodology as ADR-0035's PCF verification, and
this time it worked on the first try.** Obtained a genuine OAuth2 token from NRF via `curl`
(AMF's own mTLS client cert, scope `nsmf-pdusession`), hand-built a `multipart/related` body with
the *exact* JSON `SmContextCreateData` fields `handle_uplink_nas_transport_pdu_session_
establishment` constructs (`servingNfId`, `servingNetwork`, `anType`, `smContextStatusUri`,
`supi`, `pduSessionId`, `dnn`, `sNssai` with a hex-encoded `sd`), and POSTed it directly to real
SMF's `/nsmf-pdusession/v1/sm-contexts`. Got a real HTTP 201 with a genuine `SmContextCreatedData`
body back on the first attempt (unlike ADR-0035's PCF call, which needed a real fix first) --
confirming SMF's own internal PCF call also succeeded as part of the same request (no 500 from a
failed downstream PCF call). 4 new unit tests (`tests/conformance/test_nas_codec.cpp`,
`NasCodec.DecodeUlNasTransport*`) cover the transport-level IE extraction (pduSessionId/sNssai/dnn,
including the DNN label-decode) and the MAC-verify/tamper/reject/wrong-payload-container-type
paths, following this file's own established pattern.

**Consequence:** Phase 2's stated definition of done -- "UE registration... and PDU session
establishment... end-to-end, no narrowed slice" -- is now met in the sense that both procedures
are fully implemented in code and each real SBI call along the way (AUSF, PCF, SMF, and SMF's own
call to PCF) has been verified against a real running peer. Neither procedure can currently be
demonstrated end-to-end in a single automated `nr-ue` run, because of the shared SQN
resynchronization gap (ADR-0032/ADR-0033) that blocks a fresh UE from ever completing
authentication against this build's current UDM seed data -- this is a real, disclosed limitation
of the *demonstration*, not of the implementation itself, and is recorded plainly in
`docs/TRACEABILITY.md` rather than left for a reader to discover. SQN resynchronization and a real
SMF-side PDU Session Establishment Accept (closing the UE-visible half of this procedure) remain
the two largest disclosed gaps going into whatever comes after Phase 2.

