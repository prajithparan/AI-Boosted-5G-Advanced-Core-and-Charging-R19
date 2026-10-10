## ADR-0259: SMF's downlink-endpoint `N2SmInfoType` family -- three more real values

### What was built

Three more of the 22 `N2SmInfoType` values ADR-0251 counted as stubs. All three are the ones that
hand SMF a **new NG-RAN downlink endpoint**, so all three have exactly the real UPF consequence
ADR-0092 built for `PATH_SWITCH_REQ` -- repoint the downlink FAR's `OuterHeaderCreation`:

| `n2SmInfoType` | Transfer (TS 38.413) | Where the tunnel sits | SMF's answer |
|---|---|---|---|
| `PDU_RES_SETUP_RSP` | `PDUSessionResourceSetupResponseTransfer` | `dLQosFlowPerTNLInformation` | 204 |
| `PDU_RES_MOD_RSP` | `PDUSessionResourceModifyResponseTransfer` | `dL_NGU_UP_TNLInformation` (**OPTIONAL**) | 204 |
| `PDU_RES_MOD_IND` | `PDUSessionResourceModifyIndicationTransfer` | `dLQosFlowPerTNLInformation` | 200 + `PDU_RES_MOD_CFM` |

Every field's presence/optionality above was read from the generated ASN.1 struct, not inferred
from the name.

### The reuse this forced, and why it is the point

`install_downlink_far()` and `read_gtp_tunnel()` are new file-scope helpers holding what ADR-0092
had written inline. `PATH_SWITCH_REQ`/`HANDOVER_REQ_ACK` now call them too, so the real PFCP
Session Modification exists **once** rather than five times. Behaviour is unchanged for the two
pre-existing values -- this is a factoring, and the ADR-0092 disclosure it carries with it
(downlink PDR matches on `SourceInterface=Core` only, no UE IP Address IE, because this project
has never allocated a real UE IP anywhere) moves into the helper rather than being dropped.

### An absent OPTIONAL field is an answer, not an error

`PDU_RES_MOD_RSP`'s `dL_NGU_UP_TNLInformation` is `OPTIONAL` in the real ASN.1. Absent means the
modification changed no downlink endpoint, which is legal. SMF therefore acknowledges without
touching UPF, and logs that it did so. Present-but-malformed is still a 400. The distinction is
deliberate: rejecting a spec-legal message would be a conformance defect, and silently treating a
malformed one as "nothing to do" would hide a real peer bug.

### `PDU_RES_MOD_CFM` is built from what NG-RAN said, not from what SMF assumes

`PDUSessionResourceModifyConfirmTransfer` (§9.3.4.6) has two MANDATORY fields.
`qosFlowModifyConfirmList` is populated by echoing the QFIs from the indication's own
`associatedQosFlowList` -- SMF confirms the flows NG-RAN actually named and does not invent a set.
`uLNGU_UP_TNLInformation` is UPF's own real N3 F-TEID, the same one `PATH_SWITCH_REQ_ACK` returns.
If that F-TEID is not on record, SMF answers 500 rather than fabricating a tunnel -- the same
refusal ADR-0249 already makes on the same missing state.

### Coverage after this ADR

7 of 26 `N2SmInfoType` values are now real (was 4): `PATH_SWITCH_REQ`, `PATH_SWITCH_REQ_ACK`,
`HANDOVER_REQ_ACK`, `HANDOVER_REQUIRED`, plus the three above and the `PDU_RES_MOD_CFM` SMF now
produces. ADR-0260 is scoped to take the remainder to full accounting.

