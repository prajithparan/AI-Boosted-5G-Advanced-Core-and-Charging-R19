## ADR-0049: Commercialization mandate -- exceed free5GC, carrier-grade testing

**Date:** 2026-08-10
**Status:** Accepted. Mandatory, user-directed, recorded plainly per explicit instruction ("no
compromise") -- this ADR states the requirement honestly, including what is and is not true today,
rather than either softening it or claiming premature compliance.

**Context:** The user's stated intent for this project is commercialization. Two concrete
requirements follow, added to `PROMPT.md`'s Section 2 (Reality check) in the same commit that adds
this ADR:

1. **Performance and reliability must exceed free5GC** (the real, existing open-source 5GC this
   project has cited as a scale comparator since the project's original bootstrap brief) -- not
   merely match it.
2. **CHF and every other component must be tested as carrier-grade products**, against real
   standards and frameworks, not just this project's own conformance tests against the 3GPP
   OpenAPI schemas.

**Honest current state, stated plainly, not softened.** As of this ADR, **zero benchmarking of any
kind has been performed** in this codebase -- no throughput measurement, no latency measurement, no
concurrent-session capacity testing, no comparison against free5GC or any other implementation.
Asserting a performance/reliability claim without that measurement would be exactly the kind of
unearned "carrier-grade" label `PROMPT.md`'s own Section 2 already warns against (a rule already in
force before this ADR, now reinforced, not contradicted, by it). Known, already-disclosed
architectural debt that stands between this codebase and a meaningful performance claim: the
synchronous HTTP/2 client (tracked since ADR-0009), no load-balancing/clustering/high-availability
across NF instances (every NF today is a single, un-replicated process), no benchmarking harness or
load-generation infrastructure (Phase 8's planned "synthetic traffic generator" is the intended
home for this, not started), no chaos/fault-injection testing, no soak testing.

**Carrier-grade testing framework: real candidates named, none yet selected.** Evaluated but not
adopted (a real "evaluate before picking" decision deferred to its own future turn, matching
ADR-0004's precedent, not guessed at here): the ETSI NFV-TST (pre-deployment testing) and NFV-REL
(resiliency requirements) specification series, and standard telecom high-availability conventions
(e.g. "five nines" uptime) as industry context, not yet a number this project has committed to.
Which framework(s) actually apply, and how they'd integrate with this project's existing
conformance-test-per-procedure discipline (`docs/TRACEABILITY.md`), is real, open scope.

**What this changes, and what it doesn't.** This is a mandatory goal the project now carries
forward -- future architecture decisions (e.g. whether to finally close the synchronous-HTTP-client
debt item, whether/when to build real HA/clustering, when to build the benchmarking harness) should
be made with this mandate in view. It does **not** retroactively make any existing component
carrier-grade or benchmarked-superior-to-free5GC -- no such claim is made anywhere in this codebase
as of this ADR. `PROMPT.md`'s own existing Section 2 reality check (multi-engineer, multi-year
program; "carrier-grade" is a destination reached through conformance and soak testing, not a label
applied at commit time) remains fully in force and is not contradicted by adding this mandate --
the mandate states the destination; Section 2's honesty about the distance to it stands.

**Consequence:** No code changes in this ADR -- this is a governance/goal-setting record, the same
category as ADR-0009 (raising the project's target from lab-grade to production-intent). Concrete
next steps, each needing its own future turn and approval: select a real carrier-grade test
framework, close the synchronous-HTTP-client debt item, build real benchmarking/load-generation
infrastructure, and only then produce a real, evidence-based comparison against free5GC -- not
before.

**Forward pointer (2026-10-02):** "zero benchmarking of any kind" above is no longer true in
general -- ADR-0329 added one real, narrow NRF-only benchmark against free5GC. It is still true for
the charging data plane specifically; ADR-0445 picks this mandate back up for CHF/balance-management
and states that scoping explicitly rather than let this ADR's framing go stale silently.

