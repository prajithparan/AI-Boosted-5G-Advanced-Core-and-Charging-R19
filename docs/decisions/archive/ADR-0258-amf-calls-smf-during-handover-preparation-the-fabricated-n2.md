## ADR-0258: AMF calls SMF during handover preparation -- the fabricated N2 transfer is deleted

**Status:** Closed (work pushed; last citing commit 36ec8be on origin/main, 2026-10-06).

### What was built

**TS 23.502 §4.9.1.3 step 3.** On `HandoverRequired`, AMF now asks SMF, per PDU session, for the
N2 SM info the *target* gNB needs, and relays SMF's real answer in `HandoverRequest`:

```
source gNB --HandoverRequired--> AMF
                                 AMF --POST /nsmf-pdusession/v1/sm-contexts/{ref}/modify-->  SMF
                                     <--200 multipart: PDU_RES_SETUP_REQ + n2SmInfo binary--
AMF --HandoverRequest (PDUSessionResourceSetupListHOReq)--> target gNB
```

This closes the last piece of a chain built across four ADRs that never met:

| ADR | What it built | Why it was not enough alone |
|---|---|---|
| 0095/0096 | AMF's full NGAP handover chain, real concurrent gNB associations | Fabricated the per-session transfer locally |
| 0248 | SMF `HANDOVER_REQ_ACK` | Nothing called it |
| 0249 | SMF `HANDOVER_REQUIRED` -> real `PDUSessionResourceSetupRequestTransfer` with UPF's real N3 uplink F-TEID; AMF persists `smContextRefs[pduSessionId]` from the CreateSMContext `Location` | Built **expressly** to remove the placeholder -- and then nothing called it either |

**`build_placeholder_ho_request_transfer()` is deleted, not left unused.** It encoded a
`UL-NGU-UP-TNLInformation` with TEID=0 and transportLayerAddress 0.0.0.0 -- a structurally valid
transfer describing a tunnel that does not exist. A target gNB accepting it would have had nowhere
to send uplink traffic. It now has no caller and no reason to remain reachable.

### The design decision worth recording: skip, never substitute

`fetch_ho_request_transfer_from_smf` returns `std::nullopt` when a session cannot be prepared for
real -- no stored `smContextRef` (session predates ADR-0249's `Location` capture, or SMF sent
none), no OAuth2 token, non-200 from SMF, or an answer carrying no resolvable binary part. Such a
session is **skipped**, and if *no* session survives, AMF answers the source gNB with a real
`HandoverPreparationFailure` (`Cause` = `ho-failure-in-target-5GC-ngran-node-or-target-system`).

The rejected alternative was to fall back to the placeholder for unpreparable sessions. That was
rejected because a handover that silently carries a TEID=0 transfer is *worse* than one that
reports it could not prepare: the first fails as a black-hole after the UE has already moved, the
second fails at preparation time, visibly, with a spec-defined cause the source gNB can act on.

### Verified rather than assumed, because both failure modes are silent

Both of AMF's assumptions about the seam were checked by direct read of the other side, because
each fails as a `warn` log and a skipped session -- a green build and a passing SMF-side test would
not have caught either:

1. **Key shape.** AMF writes `ctx["smContextRefs"][std::to_string(pdu_session_id)]`
   (`ngap_task.cpp:1665`); the handover path reads the identical `std::to_string(...)` key. A
   ref-keyed or array-shaped store would have missed on *every* lookup.
2. **`contentId`.** SMF sets `n2SmInfo.contentId = "n2SmInfo"` on the root part and the same string
   as the binary part's `content_id` (`nfs/smf/src/main.cpp`). AMF resolves by `contentId` read
   from the root part rather than by part ordering, with `"n2SmInfo"` only as a fallback.

`tests/integration/test_smf_handover_n2sminfo.cpp` now pins both, plus the declared
`n2SmInfoType=PDU_RES_SETUP_REQ`, and asserts the returned transfer is not an all-zero payload --
the shape the deleted fabrication had.

Writing that test also demonstrated AMF's skip path for real, rather than by argument. SMF only
holds an N3 uplink F-TEID once it has discovered UPF through NRF and completed a PFCP Sx
Association -- its own asynchronous retry loop. A session created before that answers
HANDOVER_REQUIRED with **500**, which is precisely the case AMF now skips instead of fabricating.
The test therefore retries the create+prepare pair until the association is up rather than
sleeping a guessed interval, and reports the last failure body if it never does.

### Disclosed limits, stated here rather than found in review

- **The fetch is a blocking SBI round-trip per PDU session, made on the NGAP handler thread.** N
  sessions means N serial blocking calls while the source association waits. This is ADR-0009's
  synchronous HTTP/2 client debt surfacing on a latency-sensitive path (handover preparation has a
  real timing budget in a live network). Disclosed, not fixed here -- the fix is the async client,
  not a special case in this handler.
- **Still no end-to-end NGAP test.** Not for want of a simulator in general -- UERANSIM v3.3.0 is
  vendored arms-length (ADR-0016) and drives real NGAP/NAS -- but **its gNB implements no handover
  procedure at all**. Grep-confirmed rather than assumed: `HandoverRequired` appears in its
  generated ASN.1 (`src/asn/ngap/`) and never once in `src/gnb/`. So the whole handover chain
  (ADR-0095/0096/0248/0249/0258) is still exercised only from SMF's side over real SBI. The new
  test says so in its own header rather than implying more coverage than it has.
- `kSmfBase` is duplicated in `ngap_handover.cpp` following this file's stated
  locally-duplicated-constant convention; the real fix is the config retrofit tracked as gap-closure
  task #109 (ADR-0077), not a new one-off header.
- Unchanged from ADR-0095/0096: NCC=0 fixed chain position, fixed lab AMBR, one relay in flight per
  target gNB.

### Stale plan row corrected

The working plan this turn inherited listed "UDM: UECM non-3GPP-AMF/SMSF/IP-SM-GW/NWDAF
registration groups; UEAU `GetRgAuthData`/`GenerateAv`/`GenerateGbaAv`" as an open capability gap.
**It is not open.** UEAU's three ops closed in ADR-0214, and UECM's entire Tier-B backlog closed
across ADR-0215/0216/0217/0218/0219/0220/0221/0227. Corrected here rather than carried forward, the
same discipline ADR-0250/0251/0256/0257 applied to their own stale reasons.

