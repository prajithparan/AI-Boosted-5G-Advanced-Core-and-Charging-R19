## ADR-0260: the rest of `N2SmInfoType` -- all 26 values accounted for

### What was built

ADR-0251 counted 22 of 26 `N2SmInfoType` values as stubs, all falling through to one blanket 204.
ADR-0259 took three. This closes the accounting for the remaining nineteen, in two groups.

**Ten more values SMF really receives.** Each decodes its own real transfer -- which is what makes
a malformed peer message a 400 instead of a silently accepted 204 -- and three of them owe a real
N2 answer:

| `n2SmInfoType` | Transfer decoded | SMF's answer |
|---|---|---|
| `PDU_RES_SETUP_FAIL` | `PDUSessionResourceSetupUnsuccessfulTransfer` | 204 |
| `PDU_RES_MOD_FAIL` | `PDUSessionResourceModifyUnsuccessfulTransfer` | 204 |
| `PDU_RES_REL_RSP` | `PDUSessionResourceReleaseResponseTransfer` | 204 |
| `PDU_RES_NTY` | `PDUSessionResourceNotifyTransfer` | 204 |
| `PDU_RES_NTY_REL` | `PDUSessionResourceNotifyReleasedTransfer` | 204 |
| `SECONDARY_RAT_USAGE` | `SecondaryRATDataUsageReportTransfer` | 204 |
| `UE_CONTEXT_SUSPEND_REQ` | `UEContextSuspendRequestTransfer` | 204 |
| `PATH_SWITCH_SETUP_FAIL` | `PathSwitchRequestSetupFailedTransfer` | 200 + `PATH_SWITCH_REQ_FAIL` |
| `HANDOVER_RES_ALLOC_FAIL` | `HandoverResourceAllocationUnsuccessfulTransfer` | 200 + `HANDOVER_PREP_FAIL` |
| `UE_CONTEXT_RESUME_REQ` | `UEContextResumeRequestTransfer` | 200 + `UE_CONTEXT_RESUME_RSP` |

**Ten values are SMF-originated** and now get a 400 rather than a 204: `PDU_RES_SETUP_REQ`,
`PDU_RES_REL_CMD`, `PDU_RES_MOD_REQ`, `PDU_RES_MOD_CFM`, `PDU_RES_MOD_IND_FAIL`,
`PATH_SWITCH_REQ_ACK`, `PATH_SWITCH_REQ_FAIL`, `HANDOVER_CMD`, `HANDOVER_PREP_FAIL`,
`UE_CONTEXT_RESUME_RSP`.

### What is honestly claimed, and what is not

**The direction split is a reading, not a quotation.** TS 29.502 does not tabulate a direction per
`N2SmInfoType` value. The ten above are classified as SMF-originated because TS 38.413 defines
those transfers as ones SMF builds and sends towards NG-RAN. Said plainly in the source comment
too, rather than dressed up as a spec rule. A 400 is still strictly better than the blanket 204
that stood there before, which acknowledged an impossible request as though it had been processed.

**The seven 204s validate and record; they do not act.** Named individually rather than left for a
reviewer to discover: there is no per-QoS-flow rule state for `PDU_RES_NTY`'s notify/released
lists to modify, no charging path consuming `SECONDARY_RAT_USAGE`'s usage report, and no
RRC-Inactive suspend state for `UE_CONTEXT_SUSPEND_REQ` to enter. Each is a real, separate gap.
What changed is that a malformed transfer is now rejected instead of accepted.

### The one case a cause cannot be echoed

The two failure answers echo the cause NG-RAN reported instead of inventing one. `Cause` is an
ASN.1 CHOICE whose five real arms are enumerated longs, so rebuilding one is a value copy. The
sixth arm, `choice_Extensions`, is a pointer into the decoded structure: shallow-copying it would
double-free, and this codec has no deep-copy helper. An extension-coded `Cause` therefore returns
**500** saying exactly that, rather than being quietly replaced by a cause SMF made up.

`UEContextResumeResponseTransfer`'s every field is OPTIONAL; an empty one is spec-legal and means
"all flows resumed". It is not a placeholder for a value SMF failed to compute -- SMF has no
per-flow resume state to report a failure from, which is why the list is omitted rather than
guessed.

### Coverage

**26 of 26 `N2SmInfoType` values are now accounted for**: 16 decoded and handled inbound, 10
rejected as SMF-originated. The `docs/CAPABILITY_GAP_ANALYSIS.md` row reading "22 of the 26 real
N2SmInfoType values remain a stub" is closed by this ADR. Depth still varies by value, and the
table above says which ones record rather than act -- coverage of the enum is not the same claim
as behavioural parity, and this ADR does not make the second one.

