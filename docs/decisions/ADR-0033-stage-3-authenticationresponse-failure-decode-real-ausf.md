## ADR-0033: Stage 3 (AuthenticationResponse/Failure decode -> real AUSF confirmation -> KAMF derivation)

**Date:** 2026-08-07
**Status:** Accepted

**Context:** Stage 3 of the staged NGAP/NAS plan: decode the NAS-PDU carried in the UE's
`UplinkNASTransport` reply to Stage 2's `AuthenticationRequest`. TS 24.501 defines exactly two real
outcomes here (EAP out of scope): a success `AuthenticationResponse` (carries RES*), confirmed with
real AUSF's `PUT .../5g-aka-confirmation`, from which KAMF (TS 33.501 Annex A.7) is derived; or an
`AuthenticationFailure` (cause + optional AUTS), which this stage decodes and logs but does not
act on -- SQN resynchronization is a disclosed, explicitly out-of-scope gap (needs new AUSF/UDM
logic to reissue a vector from the UE's AUTS, not just an AMF-side change).

**New code:** `amf::nas::decode_authentication_outcome` (`nfs/amf/src/nas_codec.{hpp,cpp}`) decodes
both outcomes via generic Type-4 TLV walks, byte layouts cross-checked against
`simulators/ransim/vendor/UERANSIM/src/lib/nas` as before. `aka_crypto::derive_kamf`
(`libs/aka-crypto/src/kdf.cpp`, FC=0x6D) -- same reconstruction-not-citation disclosure as
ADR-0026's KAUSF-for-EAP-AKA', cross-checked against UERANSIM's own `DeriveKeysSeafAmf`.
`nfs/amf/src/ngap_task.cpp`'s `handle_uplink_nas_transport` reuses the exact
`ConfirmationData{resStar}` -> `ConfirmationDataResponse{authResult,kseaf}` shape AUSF's endpoint
already defines (ADR-0027).

**Real interop confirms the failure path, not the success path -- an unavoidable, structural
limitation, not a gap in this stage's testing.** UDM's seeded subscriber uses TS 35.207 Test Set
1's fixed SQN (`ff9bb4d0b607`) -- legitimately larger than any fresh UE's `SQN-MS=0`, so a real
`nr-ue` *always* sends `AuthenticationFailure` (SYNCH_FAILURE, with AUTS) on first contact, never
`AuthenticationResponse`. Confirmed via a real run: AMF correctly decoded the failure
(`mmCause=0x15`, AUTS present), logged the disclosed-gap message, and did not crash or misbehave --
this exactly matches `nr-ue`'s own reported behavior. The success path (RES* decode -> AUSF
confirmation -> KAMF) cannot be reached this way without implementing SQN resync, which is out of
scope by design (see ADR-0032's own rejected-alternative entry).

**Verification strategy for the unreachable success path:** hand-constructed, spec-correct NAS-PDU
byte vectors (`tests/conformance/test_nas_codec.cpp`) cover
`decode_authentication_outcome`'s success path directly, plus `decode_registration_request`
(previously only indirectly covered via Stage 2's real interop) and `derive_kamf`'s determinism/
input-dependence. The AUSF-confirmation+KAMF chain itself is *not* re-verified with a new
standalone harness -- `handle_uplink_nas_transport`'s AUSF call uses the identical request/response
shape `AusfIntegration.FiveGAkaSuccessfulAuthenticationCrossChecksHxresAndKseaf`
(`tests/integration/test_ausf_ue_authentication.cpp`) already exercises end-to-end against a real
running AUSF with real Milenage-computed RES* -- a second harness would only re-prove the same
thing.

**Consequence:** Stage 3 complete, 59 tests total (was 51 before this stage; +8 new). The SQN
blocker discovered here recurs identically in every downstream stage (4, 5) -- documented once
here, referenced rather than re-litigated in ADR-0034/ADR-0035.

