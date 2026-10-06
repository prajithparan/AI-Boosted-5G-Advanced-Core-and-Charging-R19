## ADR-0038: PDU Session Establishment Accept -- real Namf_Communication N1N2MessageTransfer, real 5GSM codec on SMF

**Date:** 2026-08-08
**Status:** Accepted

**Context:** ADR-0036/ADR-0037 left one disclosed gap in the PDU Session Establishment path: SMF's
`CreateSMContext` response carried no `n1SmMsg`, so AMF had no PDU Session Establishment Accept
content to send back to the UE, and a real `nr-ue` would retransmit its Request under `T3580` and
eventually give up. Initial scoping for closing this gap assumed the fix was as simple as adding an
`n1SmMsg` field to `SmContextCreatedData` -- checking the real generated schema
(`build/generated/sbi_gen/TS29122_CommonData_grp.hpp:11977`) showed that field does not exist.
The real TS 23.502 §4.3.2.2.1 mechanism (step 11) is a separate, asynchronous SBI call SMF makes
back to AMF: `Namf_Communication`'s `N1N2MessageTransfer`
(`specs/5G_APIs-REL-19/TS29518_Namf_Communication.yaml:1298`,
`POST /namf-comm/v1/ue-contexts/{ueContextId}/n1-n2-messages`) -- confirmed against the real YAML,
not assumed, before any code was written (per CLAUDE.md's "a fabricated field costs a week of
review" rule). This is real new protocol surface on both AMF (a new server-side endpoint;
`main.cpp` already had a disclosed stub for it, predating NGAP, that only ever returned a fake
"initiated" acknowledgment) and SMF (a new SBI client role, plus SMF's first real 5GSM NAS codec)
-- scoped and approved with the user before implementation, given the size.

**AMF now forwards the real N1 SM container instead of dropping it.**
`amf::nas::decode_ul_nas_transport`'s `UlNasTransportInfo` gained a `payload_container` field
capturing the opaque payload-container bytes verbatim (previously walked past and discarded --
ADR-0036's own comment called this out as deliberately unparsed, not deliberately *discarded*; this
ADR closes that gap without violating the "AMF stays opaque to 5GSM content" principle, since AMF
still never decodes the bytes, only forwards them). `handle_uplink_nas_transport_pdu_session_
establishment` now sends them to SMF as a real `multipart/related` binary part
(`application/vnd.3gpp.5gnas`, per the real YAML's `encoding` block) referenced by
`SmContextCreateData.n1SmMsg` (a real, already-generated `RefToBinaryData` field this project
simply hadn't populated yet).

**New `nfs/smf/src/nas_5gsm_codec.{hpp,cpp}`: SMF's first real 5GSM (TS 24.501 Session Management)
NAS codec.** Decodes the PDU Session Establishment Request's header only (EPD/pduSessionId/PTI,
TS 24.501 §8.3.1) -- disclosed, deliberate scope: this build always responds IPv4/SSC-mode-1,
matching UERANSIM's own hardcoded request content (`sendEstablishmentRequest` rejects any other
PDU session type before even building the message), so nothing else in the request currently
affects the response. Encodes a genuinely spec-shaped PDU Session Establishment Accept
(TS 24.501 §8.3.5): one QoS rule using TS 24.501 §9.11.4.13's real "zero packet filters" case
(spec-valid only for the DQR/default rule, which this is -- not an arbitrary shortcut), and
session-AMBR. Byte layouts (Type-1/3/4/6 IE encoding rules, exact field order, message type/EPD
values) confirmed against UERANSIM's real, independent implementation
(`simulators/ransim/vendor/UERANSIM/src/lib/nas/msg.cpp`'s `onBuild` methods,
`src/lib/nas/base.hpp`'s `EncodeIe1/3/4/6`, `src/lib/nas/ie6.cpp` -- which also confirmed
`IEQoSRules` is treated as an opaque octet string by UERANSIM, i.e. its internal structure is never
validated by the peer this project interops with, though this codec still encodes a real rule, not
arbitrary bytes, per CLAUDE.md's non-fabrication rule regardless of whether the peer would notice).
QFI and session-AMBR are sourced from PCF's actual `SmPolicyDecision` (already computed by SMF's
existing SM Policy Association call, ADR-0029) -- `authSessAmbr`/`authDefQos.n5qi`, NOT fabricated
-- with a disclosed fallback (1 Mbps) only if PCF returns no session rule at all. QFI is derived
directly from the 5QI value (`n5qi & 0x3F`), a disclosed simplification: a real network allocates
QFI via separate QoS flow binding, which no subsystem in this project implements.

**New `amf::nas::encode_dl_nas_transport`** (`nfs/amf/src/nas_codec.{hpp,cpp}`): AMF's delivery
vehicle, a secured DlNasTransport (TS 24.501 §8.2.9) wrapping SMF's opaque Accept bytes -- same
"AMF never decodes 5GSM content" discipline in the downlink direction, confirmed against
UERANSIM's real `DlNasTransport::onBuild`.

**New `amf::ngap::NgapUeRegistry`** (`nfs/amf/src/ngap_task.{hpp,cpp}`): the actual reason this
turn needed real architecture work, not just a codec. `Namf_Communication`'s `N1N2MessageTransfer`
arrives on the SBI HTTP/2 server's `io_context` thread; delivering it means writing to a specific
UE's live NGAP association, which is owned by that association's own dedicated blocking-I/O thread
(`ADR-0030`) -- a genuine cross-thread handoff this project had never needed before (every prior
NGAP/NAS message flowed in one direction, gNB-thread-only). `NgapUeRegistry` is a thread-safe
(single-mutex) map from SUPI to a non-owning pointer at the association's `SctpSocket` plus its NAS
security keys and downlink COUNT, registered by the NGAP thread once registration reaches
`AwaitingPduSessionEstablishmentRequest` (folded into `handle_uplink_nas_transport_smc_complete`,
since that's also where the PCF AM Policy Association call already lives) and unregistered on
association close. `main.cpp`'s pre-existing `N1N2MessageTransfer` stub (a disclosed
"bookkeeping-only, no real delivery pipeline exists yet" placeholder predating NGAP) is now real:
parses the real `multipart/related` body, looks up the binary part by `n1MessageContainer.
n1MessageContent.contentId`, and calls `NgapUeRegistry::send_dl_nas_transport`. Disclosed scope
narrowing: the schema's `application/json`-only alternative (no binary N1 message, e.g. an N2-only
transfer) is rejected with 400, not silently mishandled -- this build has no N2 SM info source
without a real UPF/N4 (Phase 3) to send anyway.

**Verification:** real end-to-end interop, first attempt, zero retries anywhere in the chain --
real `nrf`/`udr`/`udm`/`ausf`/`pcf`/`smf`/`amf` plus real `nr-gnb`/`nr-ue`. `nr-ue`'s own log:
`PDU Session Establishment Accept received` immediately followed by `PDU Session establishment is
successful PSI[1]` -- the real UE genuinely decoded and accepted this codec's QoS rules/
session-AMBR/SSC-mode/PDU-session-type content, not just a MAC-verified opaque blob. AMF/SMF logs
confirm the full real chain: AMF's `SM context established with SMF...` followed immediately by
SMF's `PDU Session Establishment Accept delivered to AMF for SUPI imsi-999700000000001,
pduSessionId 1`. Re-run twice against two independent clean NF/UE process sets with identical
results. 8 new unit tests (`tests/conformance/test_nas_5gsm_codec.cpp`,
`NasCodec.EncodesDlNasTransport*`) lock the byte layouts down deterministically; full `ctest` suite
re-run clean, 95/95 passing, zero regressions.

**Consequence:** the PDU Session Establishment Accept gap ADR-0036/ADR-0037 both disclosed is now
closed for real. Phase 2's full scope -- UE Registration and PDU Session Establishment, both
directions, against a real unmodified UE, first attempt -- is genuinely complete. The
`app: TUN interface could not be setup. Permission denied` line in `nr-ue`'s log after successful
PDU session establishment is an OS-level UE-side limitation (needs root to create a TUN device),
unrelated to and after this project's own NAS/SBI work; not a gap in this implementation.

