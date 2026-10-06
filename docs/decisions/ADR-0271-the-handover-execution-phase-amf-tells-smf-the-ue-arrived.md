## ADR-0271: the handover execution phase -- AMF tells SMF the UE arrived

**Date:** 2026-09-04
**Status:** Accepted

### What was missing

ADR-0269 built the relay and ADR-0270 gave SMF the target's downlink tunnel. Both stop at
`HandoverCommand`. The execution phase -- TS 23.502 §4.9.1.3.3, what happens once the UE has
actually shown up at the target -- was half-built: `handle_handover_notify` re-pointed AMF's own
registry and released the source gNB (ADR-0096), but told **SMF nothing at all**. SMF's session
stayed in whatever handover state preparation had left it in, with no record that the UE had ever
arrived.

The abandon path already did this correctly: ADR-0261 sends `hoState=CANCELLED` per PDU session
when a handover is called off. `HoState` is a real enum in TS29502_Nsmf_PDUSession.yaml --
`NONE/PREPARING/PREPARED/COMPLETED/CANCELLED` -- and only one of its two terminal values was ever
sent.

### What was built

- **AMF** (`handle_handover_notify`): after resolving the UE and **before** releasing the source,
  calls `Nsmf_PDUSession_UpdateSMContext` with `hoState=COMPLETED` for every stored SM context.
  The ordering is the spec's, not a convenience. The handler gains `smf_client`/`smf_oauth`/
  `ue_contexts` parameters, mirroring `handle_handover_cancel`'s existing shape rather than
  inventing a second way to reach SMF. A session with no stored `smContextRef` is skipped with a
  warning, never given an invented reference (ADR-0258's policy).
- **SMF**: a `COMPLETED` branch beside the existing `CANCELLED` one -- checked outside the N2
  dispatch, because `hoState` is a field of `SmContextUpdateData`, not an `N2SmInfoType`.
- **Test**: `NgapTestGnb::build_handover_notify` (mandatory IEs per `HandoverNotifyIEs`:
  AMF-UE-NGAP-ID(10), RAN-UE-NGAP-ID(85), UserLocationInformation(121)), and the relay test now
  continues through the execution phase. Renamed to
  `FullN2HandoverRelayThroughExecutionAndSourceRelease`, because the old name stopped being true.

### What SMF does on COMPLETED, and why it is not more

Stated plainly, since the honest answer is smaller than the spec's: a production SMF switches the
downlink path here if it had not already, and releases any indirect data forwarding tunnel.
Neither applies to this build, for reasons already on record rather than convenient ones:

- the downlink FAR was **really** repointed at the target's tunnel back at `HANDOVER_REQ_ACK`
  (ADR-0270, confirmed by UPF's own log naming the target's TEID), so there is no path left to
  switch; and
- indirect forwarding is not implemented at all -- SMF's `HandoverCommandTransfer` deliberately
  carries no `dLForwardingUP-TNLInformation` (ADR-0248) -- so no forwarding tunnel exists to tear
  down.

So SMF records the completion truthfully and answers, rather than performing a PFCP modification
with nothing to change. That is the same reasoning ADR-0261 recorded for `CANCELLED`, and it is a
real consequence of a disclosed scope, not a shortcut.

### Verified

```
amf-ngap: real HandoverNotify received -- AMF-UE-NGAP-ID=1, new RAN-UE-NGAP-ID=7777
smf:      handover COMPLETED for smContextRef smctx-6 -- recorded; the downlink FAR already points
          at the target gNB (HANDOVER_REQ_ACK) and this build has no indirect forwarding tunnel
amf-ngap: SMF acknowledged hoState=COMPLETED for SUPI imsi-999700000000001 pduSessionId=5
amf-ngap: sent real AMF-initiated UEContextReleaseCommand (18 bytes) to the source gNB
amf-ngap: re-pointed NGAP registry entry to the new RAN-UE-NGAP-ID=7777 after HandoverNotify
```

The test asserts on the `UEContextReleaseCommand` reaching the source. Because AMF calls SMF
first, that arrival is also evidence the SMF loop ran ahead of it -- but it is **not** proof SMF
answered 200, and the test does not claim to be: SMF's own log is what confirms that, and this
project has no read path into SMF's SM-context state to assert on. Said here rather than left for
someone to discover.

### Still open after this

`HandoverPreparationFailure` when the target rejects (`HandoverFailure`) is implemented and
relayed but has no end-to-end test -- the test gNB always admits. The 5-second N2 handover chain
from `HandoverRequired` to source release is now covered; the reject branch is not.

---

