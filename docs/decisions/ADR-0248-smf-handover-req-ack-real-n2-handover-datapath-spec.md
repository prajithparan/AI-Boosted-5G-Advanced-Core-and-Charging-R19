## ADR-0248: SMF `HANDOVER_REQ_ACK` (real N2 handover datapath), spec acquisition, and a correction to ADR-0247

### Part 1 -- capability gap closed: SMF now handles N2-based handover's datapath step

User mandate restated this turn: **Release 19 compliance without losing functionality free5GC or
open5GS implement, and superiority over anything in the market.** `docs/CAPABILITY_GAP_ANALYSIS.md`
listed, as a real open gap, that SMF implemented only `PATH_SWITCH_REQ`/`PATH_SWITCH_REQ_ACK` of
the real `N2SmInfoType` enum, and that AMF's completed NGAP handover chain (ADR-0095/0096) never
reached SMF.

Checked against the real YAML rather than memory: `TS29502_Nsmf_PDUSession.yaml` defines
**26** `N2SmInfoType` values; this project implemented **2**.

**Closed here: `HANDOVER_REQ_ACK`** -- the N2 handover step (TS 23.502 §4.9.1.3, Handover Resource
Allocation) where the TARGET gNB's accepted downlink tunnel returns to SMF. Chosen first because
it is the one handover value with a **real datapath consequence**, and because reading both
generated ASN.1 structs showed `HandoverRequestAcknowledgeTransfer` carries the same real
`dL_NGU_UP_TNLInformation` field `PathSwitchRequestTransfer` does -- so the real UPF action
(repoint the downlink FAR at the new gNB via a real PFCP Session Modification) is identical, and
the existing proven ADR-0092 datapath code is reused rather than duplicated. That equivalence was
**verified by reading the structs, not assumed from the similar names.**

The response differs and is not glossed: `PATH_SWITCH_REQ` is answered with
`PathSwitchRequestAcknowledgeTransfer` + `n2SmInfoType=PATH_SWITCH_REQ_ACK`, while
`HANDOVER_REQ_ACK` is answered with `HandoverCommandTransfer` + `n2SmInfoType=HANDOVER_CMD`, which
AMF relays to the source gNB.

**Real, disclosed scope limits:**
- Every field of `HandoverCommandTransfer` is OPTIONAL in the real ASN.1 and this build sets
  **none** -- specifically no `dLForwardingUP_TNLInformation`, because indirect data forwarding
  (a real §4.9.1.3 option needing a second forwarding UPF tunnel) is not implemented. An empty
  `HandoverCommandTransfer` is spec-legal and means "handover accepted, no data forwarding"; it is
  not a placeholder for a value that failed to compute.
- **AMF still does not call SMF during handover.** This closes the SMF half only. The AMF->SMF
  relay wiring remains a real, open, disclosed gap -- unchanged and not overstated.
- 23 of 26 `N2SmInfoType` values remain unimplemented.
- **Not live-verified end-to-end**: this is built and compiles clean with the full conformance
  suite passing (333/333), but exercising it for real needs an AMF that relays handover plus a
  gNB-side handover simulation, neither of which exists yet. Stated plainly rather than implied.

### Part 2 -- specs downloaded directly (user-directed), all Release 19

The user asked that specs be fetched rather than hand-supplied. The 3GPP FTP archive rejects
plain requests (HTTP 403) but serves with a normal User-Agent; ETSI's mirror serves PDFs directly
but returned 403 for TS 28.541 at every version tried, so 3GPP was used and LibreOffice converted
the `.docx` originals. All confirmed Release 19 by reading each document's own version line:

| Spec | Version | Purpose |
|---|---|---|
| TS 28.622 | **v19.6.0** | Generic NRM -- measured-object classes |
| TS 28.532 | **v19.3.0** | PM reporting MnS -- the reporting interface |
| TS 28.541 | **v19.6.0** | 5G NRM |
| TS 32.401 | **v19.0.0** | PM concepts and requirements |
| TS 32.240 | **v19.4.0** | Charging architecture and principles |
| TS 32.290 | **v19.4.0** | 5G charging services |
| TS 32.291 | **v19.4.0** | 5G charging, converged charging protocol |

### Part 3 -- correction to ADR-0247: there is no charging-performance-measurement spec

ADR-0247 named "the TS 32.4xx charging-performance series" as the high-priority spec needed to
measure CHF. **That was imprecise and is corrected here rather than left to mislead.** Having
downloaded and read the actual documents:

- **TS 32.240 v19.4.0 contains no CHF performance measurements.** Its single "performance
  measurement" mention concerns RAN-sharing charging and points at TS 32.130 / TS 28.201.
- **TS 32.401 v19.0.0 is a PM *framework*** -- measurement-schedule administration and result
  collection across GSM/UMTS/LTE. It defines no NF counters and explicitly defers "measurements
  available for collection" to other specifications.
- TS 32.290/32.291 are charging *functionality* specs (already this project's references), not
  measurement specs.

**Conclusion, stated plainly: CHF appears to have no 3GPP-defined performance measurements at
all.** TS 28.552 has no CHF clause; the charging series defines no counters. So "carrier-grade
measurement of CHF" cannot be achieved by adopting a spec -- it requires this project's own
counters, structured to follow TS 28.552's **pattern** (request / success / per-cause failure)
without claiming conformance to named measurements that do not exist. That is a defensible
engineering position; claiming spec conformance for CHF measurements would not be.

### Not claimed

The handover work is not live-verified, and no free5GC comparison is made.

