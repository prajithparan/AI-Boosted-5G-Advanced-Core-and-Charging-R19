## ADR-0238: Carrier-grade test framework selection -- corrects ADR-0049's named candidates

### Context

With ADR-0193's project-wide YAML-coverage audit fully closed (ADR-0228 through ADR-0237), the
user directed the project onto ADR-0049's own next concrete step: select the real carrier-grade
test framework ADR-0049 itself left as "evaluated but not adopted... a real 'evaluate before
picking' decision deferred to its own future turn." ADR-0049 named two real candidates --
ETSI NFV-TST (pre-deployment testing) and ETSI NFV-REL (resiliency requirements) -- but had not yet
actually investigated whether either applies to this project's own architecture.

Direct research this turn found a real, disclosable mismatch: both NFV-TST and NFV-REL are ETSI
NFV-MANO series specifications -- they test the **virtualization/orchestration layer** (VNF
lifecycle management by a VNFM/NFVO: onboarding, instantiation, scaling, fault/image/performance
management of the *virtualized infrastructure*), not a network function's own application-layer
SBI performance or behavior. NFV-TST007 ("Guidelines on Interoperability Testing for MANO") and
NFV-TST010 ("API Conformance Testing Specification") both target MANO API conformance; NFV-REL's
own resiliency models describe NFV-MANO-orchestrated failover, not a standalone process's own
reliability. This project has no ETSI NFV-MANO layer and was never scoped to build one --
deployment is plain Docker Compose (lab) and Helm charts (k8s), per CLAUDE.md's own mandated
stack, with no VNF descriptors, VNFM, or NFVO anywhere in this codebase or its plans. Applying
NFV-TST/NFV-REL literally here would mean either building a MANO layer never previously scoped, or
citing a spec series that doesn't actually govern what this project needs tested -- neither is
honest, so this ADR corrects ADR-0049's own named candidates rather than silently proceeding with
a mismatched citation.

The real, correctly-scoped alternative, confirmed by direct search (not assumed): **3GPP
TS 28.552** ("Management and orchestration; 5G performance measurements") and **3GPP TS 28.554**
("Management and orchestration; 5G end to end Key Performance Indicators (KPI)"), both real, both
current through Release 19 (TS 28.554 v19.5.1, 2025-12). TS 28.552 defines the actual counter-based
performance measurements 5G Core network functions expose (confirmed real measurement families
directly matching this project's own NFs and even its own SBI operation names -- e.g. AMF policy
association measurements keyed on `Npcf_AMPolicyControl_Create`, SMF measurements keyed on
`Npcf_SMPolicyControl_Create`/`Nsmf_PDUSession_CreateSMContext`); TS 28.554 defines how those
counters compose into end-to-end KPIs. This is genuinely the right layer -- it measures 5GC network
functions themselves, not a MANO orchestration layer this project doesn't have.

### Decision

Adopt **TS 28.552 + TS 28.554** as this project's real carrier-grade performance-measurement
framework, replacing ADR-0049's named-but-unadopted ETSI NFV-TST/NFV-REL candidates for that
purpose. The standard telecom "five nines" HA convention named in ADR-0049 remains informal
industry context only, unchanged by this ADR -- it was never adopted as a committed number and
this ADR doesn't change that. ETSI NFV-REL's own resiliency *concepts* (failure-injection framing,
resilience-model vocabulary) may still be useful informal reference for future chaos/fault-injection
work, but are not adopted as a formal conformance target given this project's non-MANO deployment
model.

### Consequence

No code changes in this ADR -- governance/decision record only, same category as ADR-0049 itself.
Concrete next steps per ADR-0049's own stated order, each needing its own future turn: (1) map this
project's own NFs' real measurement points against TS 28.552's real counter families (which
measurement families are directly reusable vs. need this project's own OpenTelemetry
instrumentation extended); (2) close the synchronous-HTTP-client debt item (ADR-0009) before
benchmarking, since a synchronous client would produce misleading throughput numbers not
reflective of the target architecture; (3) build a real load-generation harness (Phase 8's planned
"synthetic traffic generator"); (4) only then produce a real, evidence-based comparison against
free5GC, framed against TS 28.552/28.554's own real KPI definitions rather than informal numbers.

