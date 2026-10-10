## ADR-0261: Handover Cancellation -- the last named AMF handover gap

**Status:** Closed (work pushed; last citing commit 3b48e79 on origin/main, 2026-10-06).

### What was built

`HandoverCancel` (TS 38.413 §8.4.5) end-to-end across two NFs. Every identifier was read from
source material rather than assumed:

| Fact | Source |
|---|---|
| `id-HandoverCancel ProcedureCode ::= 10` | `specs/NGAP/ngap-17.9.asn:11289` |
| `HandoverCancelIEs`: AMF-UE-NGAP-ID(10), RAN-UE-NGAP-ID(85), Cause(15), all mandatory | same file, `HandoverCancelIEs` |
| `HandoverCancelAcknowledgeIEs`: both IDs mandatory, CriticalityDiagnostics(87) OPTIONAL | same file |
| `HoState` enum incl. `CANCELLED`, a real field of `SmContextUpdateData` | `TS29502_Nsmf_PDUSession.yaml:4836`, field at :2221 |
| `CauseRadioNetwork_handover_cancelled` | generated `Cause` ASN.1 |

AMF, on receipt: resolves the UE by cold `amf_ue_id_index` lookup (the same discipline
`handle_handover_required`/`handle_handover_notify` use -- the AMF-UE-NGAP-ID is authoritative, not
this association's own state), tells SMF `hoState=CANCELLED` per PDU session, sends a real
`UEContextReleaseCommand` with `Cause = radioNetwork/handover-cancelled` to the **target** gNB, and
answers the source gNB with a real `HandoverCancelAcknowledge`.

SMF, on `hoState=CANCELLED`: records it against the SM context and answers 200 with the state
echoed.

### Two decisions worth recording

**AMF now persists the handover target gNB.** `handle_handover_required` stores
`handoverTargetGnbId` alongside the existing `smContextRefs` on the same SUPI-keyed UE context. The
alternative was to skip the target release and disclose it -- rejected, because without the target
gNB's identity a cancel leaves the target holding reserved resources permanently. That is a real
defect, not a scope choice, and the fix is one field on state that already exists rather than a new
per-UE store.

**SMF releases nothing, and says why.** ADR-0249's `HANDOVER_REQUIRED` answer allocates no new UPF
resource -- it returns the *same* N3 uplink F-TEID allocated at session establishment. So the
target-side reservation a production SMF would tear down here does not exist in this build. SMF
records the cancellation truthfully instead of performing a PFCP modification that would have
nothing to undo. Disclosed rather than dressed up as a completed teardown.

### The honest scope limit

With ADR-0258's synchronous relay, `handle_handover_required` **blocks the source association's own
read loop** while awaiting the target gNB's reply. A `HandoverCancel` arriving *during* preparation
therefore cannot be read until preparation finishes. This implementation covers cancellation of an
already-**PREPARED** handover -- the source gNB changing its mind after receiving `HandoverCommand`,
which is the main real case -- and **not** a cancel racing an in-flight preparation. Same root
cause as ADR-0258's own per-session blocking disclosure: ADR-0009's synchronous client. Stated
here rather than left for a reviewer to find.

### An ASN.1 patch, on the existing precedent

`HandoverCancel`/`HandoverCancelAcknowledge` were not among the message types ADR-0031 patched to
use a concrete `ConcreteProtocolIE-Container` in place of the real spec's parameterized
`ProtocolIE-Container {{...}}` (asn1c 0.9.29 cannot resolve the parameterization). They are now,
with the same comment convention and for the same documented reason. This changes no wire encoding
-- per X.691 clause 10.9 an open type's PER encoding is the octet-string-wrapped blob these helpers
already produce -- it is what lets the existing `find_ie`/`add_ie` helpers operate on these two
messages at all.

### Not tested end-to-end, for the reason ADR-0258 already recorded

UERANSIM's gNB implements no handover procedure at all, so there is still no way to drive a real
`HandoverCancel` over SCTP in this project. The AMF-side handler is exercised only by construction
and review; SMF's `hoState=CANCELLED` branch is reachable over real SBI and is the half that can be
tested. Said plainly: this is the weakest-verified item in the ADR-0258..0261 sequence.

