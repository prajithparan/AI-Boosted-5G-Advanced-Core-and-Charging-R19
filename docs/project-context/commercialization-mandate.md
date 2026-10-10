# Commercialization mandate (ADR-0049, user-directed, mandatory)

Moved verbatim from CLAUDE.md (section "Commercialization..."); see `docs/optimization/MOVE_LOG.md`.

- Intent is to commercialize this system. Two concrete requirements: (1)
  performance and reliability must **exceed** free5GC, not just match it;
  (2) CHF and every other component must be tested as carrier-grade
  products against real standards/frameworks, not just this project's own
  conformance tests against the 3GPP OpenAPI schemas.
- Stated honestly, not softened: as of ADR-0049, **zero benchmarking of
  any kind** has been performed against free5GC or anything else. Known
  debt that blocks a meaningful performance claim: the synchronous HTTP/2
  client (ADR-0009), no HA/clustering across NF instances, no
  benchmarking/load-generation harness (Phase 8's synthetic traffic
  generator is the intended home, not started).
- Carrier-grade test framework selected (ADR-0238, 2026-08-30): **3GPP
  TS 28.552** (5G performance measurements) + **TS 28.554** (5G
  end-to-end KPIs) — real, current-through-R19 specs that measure 5G
  Core network functions themselves. ETSI NFV-TST/NFV-REL (ADR-0049's
  original named candidates) were investigated and found to target the
  ETSI NFV-MANO virtualization/orchestration layer (VNF lifecycle
  management), which this project has no plans to build (deployment is
  plain Docker Compose/Helm, no MANO layer) — corrected, not silently
  kept. Standard telecom "five nines" HA convention remains informal
  industry context only, unchanged.
- This does not retroactively make anything already built carrier-grade
  or proven superior to free5GC — no such claim exists anywhere in this
  codebase yet. The Reality check above (multi-engineer, multi-quarter
  program; "carrier-grade" is a destination reached through conformance
  and soak testing, not a label applied at commit time) still stands and
  is not contradicted by this mandate.
