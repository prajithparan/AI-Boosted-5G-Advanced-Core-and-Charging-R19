## ADR-0269: the full N2 handover relay, end to end -- and the gap that closing it exposed

**Date:** 2026-09-04
**Status:** Accepted

### What was built

The assertion every ADR from ADR-0090 through ADR-0261 was written towards and none could make:
one registered UE with one real PDU session handed over from a real source gNB to a real target
gNB, across two simultaneously open SCTP associations, ending in a real `HandoverCommand`.

`tests/integration/ngap_test_gnb.{hpp,cpp}` gains the target gNB's half of Handover Resource
Allocation (TS 38.413 §8.4.3):

- **`parse_handover_request`** -- reads the `AMF-UE-NGAP-ID` the acknowledge must echo and the PDU
  session IDs AMF actually prepared, out of the real message. Not assumed: AMF **skips** any
  session SMF could not answer for (ADR-0258), so the list is a real answer to a real question.
- **`build_handover_request_acknowledge`** -- the mandatory IE set per
  `HandoverRequestAcknowledgeIEs`: `AMF-UE-NGAP-ID`(10), `RAN-UE-NGAP-ID`(85) (the *target's* own
  newly allocated one, which is the whole point of the IE), `PDUSessionResourceAdmittedList`(53),
  `TargetToSource-TransparentContainer`(106).

Each admitted session carries a **real** PER-encoded `HandoverRequestAcknowledgeTransfer` -- the
target's own DL N3 GTP tunnel plus the QoS flows it admits -- not an opaque marker. That
distinction is deliberate and matches what the ASN.1 says: the transparent containers really are
opaque (TS 38.413 §9.3.1.31/§9.3.1.32, AMF relays them byte-for-byte and a short marker is honest),
while `handoverRequestAcknowledgeTransfer` is `OCTET STRING (CONTAINING ...)` -- a structure with a
real consumer.

`establish_pdu_session` is factored out of ADR-0267's test so the relay test reaches the same UE
state without duplicating it.

### The chain the test drives

```
source gNB --NGSetup--> AMF                         target gNB --NGSetup--> AMF
UE registers (ADR-0265/0266) + PDU session (ADR-0267) on the source association
source --HandoverRequired--> AMF
                             AMF --Nsmf UpdateSMContext(HANDOVER_REQUIRED)--> SMF --N4--> UPF
                             AMF --HandoverRequest--> target      (target's own thread)
                             AMF <--HandoverRequestAcknowledge--  target
source <--HandoverCommand--- AMF
```

AMF's own log, rather than a green test taken on trust:

```
amf-ngap: HandoverRequired for AMF-UE-NGAP-ID=1, RAN-UE-NGAP-ID=1, SUPI=imsi-999700000000001,
          HandoverType=0, 1 PDU session(s), target gNB identity 9 bytes
smf: HANDOVER_REQUIRED answered with a real PDUSessionResourceSetupRequestTransfer
     (real UPF N3 uplink TEID=0x1) -- no placeholder tunnel
amf-ngap: sending real HandoverRequest (151 bytes) to target gNB, awaiting reply
amf-ngap: real HandoverRequestAcknowledge received from target gNB -- sending real HandoverCommand
amf-ngap: sent real HandoverCommand (33 bytes) to source gNB, AMF-UE-NGAP-ID=1
```

### The three traps ADR-0267 recorded, and what each cost

All three produce the **same** symptom -- `HandoverPreparationFailure` on the source association,
which reads like an AMF or SMF bug and is actually test sequencing. Recording them in advance is
what made this increment cheap:

1. **`handle_handover_required` blocks the source association** for up to 10 s awaiting the
   target's reply (ADR-0258's synchronous relay). The target is therefore driven on its own thread.
   Its receive is bounded by the driver's own 30 s socket timeout (ADR-0267), so a `HandoverRequest`
   that never arrives fails the test with a message naming the step rather than hanging until ctest
   kills the binary.
2. **Spawning UPF is not sufficient.** SMF establishes the N4 session *inline* during
   `CreateSmContext` and never returns to a session it could not establish one for. So
   `wait_for_upf_sx_association` gates on the real thing -- a throwaway SUPI's session probed with
   `HANDOVER_REQUIRED` until SMF answers 200 -- **before** the UE creates its own. In the verifying
   run that took 5 attempts across ~1.1 s: SMF logged `no UPF Sx Association established yet` four
   times, then `PFCP Sx Association established`, then a real uplink F-TEID.
3. **AMF's stores live in a shared, long-lived Redis** across tests and runs, so the test
   establishes its own session in its own body rather than leaning on ADR-0267's test having run.

A fourth was verified rather than discovered: AMF keys the target association on
`ngap::encode_value(&asn_DEF_GlobalGNB_ID, ...)` bytes captured from that gNB's `NGSetupRequest`
(`ngap_task.cpp`), and resolves a handover's target by re-encoding the source's `TargetID` the same
way (`ngap_handover.cpp`). The test gNB fills both through one `fill_global_gnb_id`, so the keys
are byte-identical -- checked before writing the test, because a one-byte difference would have
produced trap-shaped symptom number four.

### The gap this exposed -- disclosed, not quietly left

**AMF never calls SMF on the acknowledge leg.** TS 23.502 §4.9.1.3.2 has AMF send
`Nsmf_PDUSession_UpdateSMContext` carrying the target's `HANDOVER_REQ_ACK` N2 SM info, so SMF can
install the target gNB's DL tunnel in UPF before the UE arrives. SMF implements that direction
(ADR-0248). AMF does not call it: `n2SmInfoType` appears exactly once in all of `nfs/amf/src/`, as
`HANDOVER_REQUIRED`.

This is the exact mirror of the gap ADR-0258 closed for the other direction, and it is **not**
fixed here. The consequence is real: the relay completes and the source gNB gets its
`HandoverCommand`, but UPF's downlink still points at the source gNB's tunnel, so a real UE would
lose downlink at the moment it arrived at the target. This test passes because it asserts on the
NGAP relay, which is what this increment built -- the next increment is AMF calling SMF with the
transfer the target just sent.

### Other disclosed limits

- **Stops at `HandoverCommand`.** `HandoverNotify` (procedure 11) and the `UEContextReleaseCommand`
  it triggers toward the source are a separate handler with its own registry re-pointing, untouched
  here.
- **No real RRC.** The transparent containers are 4-byte markers in both directions. That is what
  the spec permits AMF to be given, not a stand-in for something the test should have built.
- **The UPF datapath does not run in this environment.** UPF logs that eBPF/XDP could not start
  without ambient capabilities; PFCP control-plane signalling is real and is what this test
  depends on. No user-plane packet is forwarded, and none is asserted on.
- **One relay in flight per target gNB**, per `GnbAssociationRegistry`'s own disclosed lab-scope
  simplification. Unchanged by this increment.

---

