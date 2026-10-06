## ADR-0247: TS 28.552/28.554 measurement mapping (ADR-0238 step (1)) -- and two real gaps it exposes

### Context

The user vendored TS 28.552 and TS 28.554 into `specs/`, unblocking ADR-0238's step (1), which
ADR-0244/ADR-0246 had recorded as blocked-on-spec-material. Both are real and current:
**TS 28.552 v19.8.0 (2026-08)** and **TS 28.554 v19.7.0 (2026-04)**, both Release 19.

### Decision

`docs/PERFORMANCE_MAPPING.md` is the reviewable mapping deliverable, same shape as
`docs/CHARGING_MAPPING.md` for the TM Forum side: spec measurement -> project measurement point ->
status, with gaps marked rather than invented. TS 28.552 carries **663 distinct measurement
names** (counted from the spec's own `e)` fields) across 13 NF clause families.

NRF's clause 5.10 (`NFS.*`, 13 measurements) is mapped **exhaustively** as the first NF -- chosen
because it is small enough to complete honestly and is the NF ADR-0244/0246 already benchmarked.
The other NFs get an applicability triage and a metric count, explicitly **not** a guessed
per-measurement mapping; each needs the same line-by-line treatment against the spec text.

### Two real gaps this exposed, both worth stating plainly

**1. TS 28.552 does not cover CHF at all.** Confirmed by direct search: there is no
"Performance measurements for CHF" clause. Nor for AUSF, BSF, SCP, 5G-EIR, GMLC or SEPP -- all
built here. CHF is this project's commercial centre under ADR-0049, and the framework ADR-0238
selected does not measure it; charging performance lives in the TS 32.4xx series instead. This
does not invalidate ADR-0238 (28.552 remains correct for the NFs it does cover), but
"carrier-grade measurement of CHF" is **not** achieved by 28.552 alone, and that must not be
assumed. Recorded as a real limitation of the selected framework rather than resolved by mapping
CHF onto a loosely-similar family.

**2. Clause 5.7 "Common performance measurements for NFs" is mostly unusable here.** Read rather
than assumed from its title: 5.7.1's `VR.VCpuUsageMean`/`VMemoryUsageMean`/`VDiskUsageMean` are
sourced, in the spec's own words, from ETSI GS IFA 027 measurements "**from VNFM**" -- the
NFV-MANO layer ADR-0238 already established this project does not have. The same reasoning that
rejected NFV-TST/NFV-REL applies again. Only 5.7.2 (connection data volumes) is implementable.
The same dependency makes TS 28.554's 6.4.2 and Energy-Efficiency KPIs unreachable in this
deployment model.

### The single highest-leverage finding

TS 28.552 consistently splits every operation into **request / success / per-cause failure**.
This project's counters overwhelmingly count only "it happened" -- NRF's own
`nrf_registrations_total` serves as both `NFS.RegReq` and `NFS.RegSucc`, so a success rate is not
computable from it at all. **0 of NRF's 13 measurements are fully conformant today; 2 are
partially served by that one conflated counter.**

Every 5GC-reachable TS 28.554 KPI (6.2.3, 6.2.5, 6.2.12, 6.2.14, 6.2.15, 6.2.16, 6.4.1) is a
*ratio* of exactly those request/success/failure counters. So splitting them is not merely
conformance tidiness -- it is the prerequisite that makes the end-to-end KPIs computable, and
therefore the prerequisite for ADR-0238's step (4) comparison against free5GC.

### Further specs required (answered in advance, per user request)

For the mapping and for instrumenting counters: **none**. 28.552's core-NF clauses reference
TS 23.501/502/503 (already this project's stage-2 reference) and the TS 29.5xx SBI specs (already
vendored as R19 YAML). Needed only for later stages: TS 28.622 + TS 28.541 (measured-object NRM
classes), TS 28.532 (PM reporting interface), TS 32.401 (PM concepts, optional), and -- **highest
priority given ADR-0049** -- the TS 32.4xx charging-performance series, without which CHF cannot
be measured at all.

### Not claimed

No conformance claim, and no free5GC comparison. This ADR delivers the mapping and names the gaps.

