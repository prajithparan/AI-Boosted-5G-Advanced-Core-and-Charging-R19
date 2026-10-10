## ADR-0270: AMF tells SMF where the target wants downlink -- ADR-0269's disclosed gap, closed

**Date:** 2026-09-04
**Status:** Accepted

### The gap

ADR-0269 completed the N2 handover relay end to end and disclosed, rather than buried, what
completing it exposed: `n2SmInfoType` appeared exactly once in all of `nfs/amf/src/`, as
`HANDOVER_REQUIRED`. AMF received the target gNB's `PDUSessionResourceAdmittedList` -- carrying,
per admitted session, the DL N3 tunnel the target had just reserved -- and **dropped it**. It then
answered the source gNB with a `HandoverCommand` and considered the procedure done.

The consequence was not cosmetic. SMF owns the PFCP session that programs UPF's downlink FAR, and
AMF was the only party holding the target's tunnel. So UPF kept sending downlink to the **source**
gNB: a real UE would have lost downlink at the exact moment it arrived at the target. The NGAP
relay was correct and the user plane was left behind.

The mirror image of this gap in the other direction was ADR-0258. SMF's side of this one has
existed since ADR-0248. Nothing called it.

### What was built

`send_ho_req_ack_to_smf` in `nfs/amf/src/ngap_handover.cpp` -- the acknowledge-direction twin of
ADR-0258's `fetch_ho_request_transfer_from_smf`, per TS 23.502 §4.9.1.3.2. For each session in the
target's admitted list, AMF now calls `Nsmf_PDUSession_UpdateSMContext` with
`n2SmInfoType=HANDOVER_REQ_ACK` and the target's own `HandoverRequestAcknowledgeTransfer` bytes.

Unlike the `HANDOVER_REQUIRED` direction, this one carries real N2 SM info **up** to SMF, so the
request is `multipart/related`: a jsonData root part naming the binary part by `contentId`, plus
the transfer itself. That shape was read from SMF's own handler (which resolves the binary part by
the `contentId` the root names, and ignores a body carrying none), not assumed from the spec's
prose.

SMF answers each call with a real `HandoverCommandTransfer`, which AMF now puts in the
`HandoverCommand`'s `PDUSessionResourceHandoverList` (IE 59) -- an IE AMF previously omitted
entirely, disclosed at the time as a scope narrowing and now genuinely fillable because SMF
returns real content for it. The IE is `SIZE(1..maxnoofPDUSessions)`, so it is added only when at
least one session really switched; its absence honestly means no session's downlink moved.

**A session SMF will not confirm is left OUT of the list rather than given a fabricated transfer** --
the same rule the request direction already follows (ADR-0258), and the reason a partial handover
is visible rather than plausible.

### Verified by three separate processes, not by one green test

```
amf-ngap: real HandoverRequestAcknowledge received from target gNB
upf:  real Create PDR 2 / Create FAR 2 for TEID 0x2 -- downlink Outer Header Creation decoded
      (GTP-U/UDP/IPv4, peer TEID=0xbeef, peer IPv4=127.0.0.22)
smf:  HANDOVER_REQ_ACK real N4 Session Modification succeeded (UP F-SEID=0x2, new gNB TEID=0xbeef)
      -- real downlink GTP-U tunnel now points at the new gNB
amf-ngap: 1 PDU session(s) switched to the target gNB's downlink tunnel via SMF
amf-ngap: sent real HandoverCommand (42 bytes) to source gNB, AMF-UE-NGAP-ID=1
```

`peer TEID=0xbeef, peer IPv4=127.0.0.22` is the tunnel the **test's target gNB** advertised in its
own acknowledge (`kTargetGnbDlTeid`/`kTargetGnbIpv4` in `ngap_test_gnb.cpp`). UPF learned it from
SMF, which learned it from AMF, which learned it from the target -- four processes, one value, no
shared memory between them. The `HandoverCommand` also grew from 33 to 42 bytes: the new list.

### Test

`AmfNgapTestGnb.FullN2HandoverRelayReachesHandoverCommand` gains an assertion on the
`HandoverCommand`'s own `PDUSessionResourceHandoverList`, via a new
`NgapTestGnb::parse_handover_command`. Stated plainly because it is the point: without that
assertion, deleting this ADR's entire SMF call would leave the relay test passing -- the NGAP
exchange is unchanged by it. An empty list now fails with a message naming the real consequence
(UPF still pointed at the source).

### Disclosed limits

- **No indirect data forwarding.** SMF's `HandoverCommandTransfer` sets no
  `dLForwardingUP-TNLInformation` (ADR-0248's own disclosure): every field of that transfer is
  OPTIONAL and an empty one legally means "handover accepted, no data forwarding". Indirect
  forwarding needs a second forwarding tunnel in UPF and is not implemented.
- **UPF's downlink is programmed at the PFCP control plane only.** UPF's own log says it: the
  downlink Outer Header Creation is decoded and acknowledged but not wired into the eBPF/XDP
  datapath, which has no downlink GTP-U encapsulation path. Pre-existing and separately disclosed;
  this ADR does not change it, and no packet is asserted on.
- **Still stops at `HandoverCommand`.** `HandoverNotify` and the execution phase (§4.9.1.3.3),
  where the UE actually arrives and the source is released, remain the next increment.
- **Per-session calls are serial**, inside the same handler that already blocks the source
  association (ADR-0009's synchronous client). A UE with many sessions makes preparation slower in
  proportion. Unchanged by this ADR, and the same debt ADR-0258 recorded.

---

