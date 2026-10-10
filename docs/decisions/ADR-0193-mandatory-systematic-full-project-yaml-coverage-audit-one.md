## ADR-0193: mandatory systematic full-project YAML coverage audit, one gap at a time, ASAP priority (user-directed)

### Context

While rebuilding CHF for the Apache Doris migration (ADR-0192, in progress), the user asked why
`Nnrf_Bootstrapping` isn't implemented. Checked: real, confirmed gap, not a disclosed stub --
`specs/5G_APIs-REL-19/TS29510_Nnrf_Bootstrapping.yaml` (TS 29.510 v19.5.0, one real operation,
`GET /bootstrapping` -> `BootstrappingInfo`) exists in the vendored spec tree but was never added
to `libs/sbi-generated/CMakeLists.txt`'s pilot set, unlike its three siblings
(`Nnrf_NFManagement`/`Nnrf_NFDiscovery`/`Nnrf_AccessToken`) which are wired for NRF. It was simply
missed while scoping NRF, not deferred with disclosure.

The user's response, verbatim: "Similar like this anything not implemented in YAML in any NFs,
please implement, I want this project to be Superior from all currently available", then,
explicitly, as a **strict instruction**: "Please check /audit everything one by one ASAP." This
is a standing, project-wide directive, not scoped to NRF or to this session -- consistent with,
and now formalizing as an explicit ADR, the pre-existing "full YAML coverage mandatory" and
"capability superiority mandate" working norms this project has followed since the UDR
resource-breadth gap-closure pattern (ADR-0093 and the ~20 UDR slices that followed it, tasks
#106/#116-136) and repeated for every Tier 1/Tier 2 NF since.

### Decision

1. **Audit mechanism**: a systematic, evidence-based cross-reference -- for every NF already
   implemented in this repo, every real 3GPP-assigned YAML API for that NF (matched by real
   service-name prefix and the YAML's own `title:`/`description:` fields, not guessed) is checked
   against `libs/sbi-generated/CMakeLists.txt`'s pilot set. APIs present in `specs/` but never
   wired in at all are **Tier-A gaps** (the `Nnrf_Bootstrapping` shape: a whole API missing, not
   even generated). For APIs that ARE wired in, every real `operationId` is checked against that
   NF's actual routed handlers and classified real / stub (`501`/`404`/TODO) / entirely
   unrouted -- these are **Tier-B gaps**. Nothing is asserted without checking the real YAML and
   the real handler code; where a quick check can't tell, it's recorded as "needs closer look",
   never guessed either way.
2. **Closure discipline stays unchanged.** "ASAP" governs pacing and priority -- work moves
   through the resulting backlog continuously, one item at a time, without pausing for
   confirmation between items (already established by ADR-0184's continuous pipeline) -- it does
   NOT relax the per-item bar. Every gap closed still gets the full, real Definition of Done: real
   YAML as the only source of DTO shapes, live verification over real mTLS/OAuth2 (not
   self-consistency tests), `ctest` clean, an ADR entry, `docs/TRACEABILITY.md` and
   `docs/CAPABILITY_GAP_ANALYSIS.md` updates, and a commit under the real author identity. No
   scope-widening exception, no shortcut on disclosure of what remains a genuine stub (e.g. RF/LPP-
   dependent operations that stay honest `501`s, per ADR-0189/ADR-0191's own precedent) -- "make
   the project superior" means closing real, checkable gaps, not fabricating coverage.
3. **Scale, disclosed honestly.** This project vendors 531 REL-19 YAML files; only a subset maps
   to NFs actually built so far. A full, first-pass audit across every currently-implemented NF is
   itself a multi-item undertaking, and each closure is real engineering work (codegen wiring,
   real handler logic, live verification, tests) -- "ASAP" is the priority ordering relative to
   other work, not a claim that the entire backlog closes in one turn. This does not relax
   CLAUDE.md's own "Reality check" section.
4. The audit's own output (per-NF, per-operation gap table) is tracked in
   `docs/CAPABILITY_GAP_ANALYSIS.md` as it lands, not duplicated into this ADR -- this ADR records
   the decision and process, not the gap list itself, to avoid ADR churn as items are closed.

### What this ADR does NOT include

The gap list itself (lands in `docs/CAPABILITY_GAP_ANALYSIS.md` once the audit completes). Any
claim that a specific gap is already closed -- closures get their own ADR each, same as every
prior gap-closure task. No change to the "never fabricate a TS number/field/API path" rule --
audit findings of "needs closer look" stay unresolved until actually checked, not resolved by
assumption.

