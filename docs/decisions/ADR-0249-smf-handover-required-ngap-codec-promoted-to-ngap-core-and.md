## ADR-0249: SMF `HANDOVER_REQUIRED`, `ngap_codec` promoted to `ngap_core`, and AMF's missing SM context ref

### Context

ADR-0248 closed SMF's `HANDOVER_REQ_ACK`. This continues the same user mandate (R19 compliance
without losing free5GC/open5GS functionality, and superiority over anything in the market) by
attacking the other half of the N2 handover gap: AMF sends the target gNB a **placeholder** uplink
tunnel (`TEID=0`, IP `0.0.0.0`) from `build_placeholder_ho_request_transfer()`, because nothing
ever gave it the real one.

### Decision 1: SMF now answers `HANDOVER_REQUIRED` with the real tunnel

TS 23.502 §4.9.1.3 step 3: AMF relays the source gNB's Handover Required to SMF, and SMF returns
the N2 SM info the TARGET gNB needs -- a real `PDUSessionResourceSetupRequestTransfer` carrying
UPF's own **real** N3 uplink F-TEID, answered with `n2SmInfoType=PDU_RES_SETUP_REQ`.

The F-TEID is not new state: it is the same `ulTeid`/`ulIpv4` UPF allocated at Session
Establishment and SMF has persisted since ADR-0092. The QoS flow (QFI=1, non-dynamic 5QI=9 --
the real 3GPP non-GBR default, TS 23.501 Table 5.7.4-1 -- ARP 8) is the same one this project's
establishment path already uses, carried over unchanged rather than newly invented.

SMF now implements **4 of the 26** real `N2SmInfoType` values (`PATH_SWITCH_REQ`,
`PATH_SWITCH_REQ_ACK`, `HANDOVER_REQ_ACK`, `HANDOVER_REQUIRED`), up from 2.

### Decision 2: `ngap_codec` promoted from `nfs/amf/src/` into `libs/ngap-core`

SMF needs `make_ie`/`add_ie`/`encode_value` to build that transfer. They lived in **AMF's private
header**, and CLAUDE.md's non-negotiable rules forbid one NF including another NF's private
headers. The options were duplicating ~60 lines of ASN.1 boilerplate into a second NF, or moving
genuinely shared protocol-codec infrastructure into the shared library it belongs in. Promoted --
these helpers encode NGAP's ProtocolIE-Container pattern and are not AMF policy in any sense.
AMF's own call sites are untouched; only its include line changed.

### Decision 3 (the real find): AMF was structurally incapable of calling SMF again

Investigating the relay surfaced why it was never built. After `CreateSMContext` returns 201, AMF
**read the response and discarded the `Location` header**. TS 29.502 returns the created
resource's URI there, and its last path segment is the SM context ref -- the handle every
subsequent `Nsmf_PDUSession_UpdateSMContext` call needs. Without it, "AMF doesn't call SMF during
handover" was never a skipped call: AMF **could not make one at all**, for handover or anything
else.

AMF now parses `Location`, extracts the ref, and stores it per PDU session id under the UE's own
SUPI-keyed context (`smContextRefs[pduSessionId]`), alongside the association data
`UeContextStore` already holds. A 201 without `Location` is logged as a warning naming the
consequence, rather than silently leaving a later handover to fail with a confusing lookup miss.

### What remains open, stated precisely

**AMF still does not make the handover relay call.** This ADR removes the structural blocker (the
ref now exists and is stored) and completes the SMF side (both steps answer correctly), but
`handle_handover_required` still calls `build_placeholder_ho_request_transfer()`. The placeholder
is still what goes on the wire today. Closing it needs the handover path to map its NGAP UE
identities to the SUPI key the refs are stored under, then issue the call -- real remaining work,
not a wiring one-liner, and deliberately not overstated as done.

22 of 26 `N2SmInfoType` values remain unimplemented. None of the handover work is live-verified
end-to-end: that needs the AMF relay plus a gNB-side handover simulation, neither of which exists.

### Testing

Full build clean, clang-format clean, conformance 333/333, `ctest` 469/469. One real compile error
was found and fixed on the way: `QosFlowSetupRequestItem.h` reached AMF transitively but not SMF.

