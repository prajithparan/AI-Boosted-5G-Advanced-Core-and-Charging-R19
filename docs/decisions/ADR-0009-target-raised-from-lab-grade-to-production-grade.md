## ADR-0009: Target raised from lab-grade to production-grade

**Date:** 2026-08-04
**Status:** Accepted

**Context:** PROMPT.md Section 2 and CLAUDE.md's original goal statement both scoped this project
as "lab-grade... not a carrier-grade core," and CLAUDE.md's own Reality Check section still says
so. The user edited CLAUDE.md's goal line to "production-grade" and, when the resulting
inconsistency (production-grade goal vs. still-lab-grade Reality Check, vs. PROMPT.md's original
framing) was flagged per CLAUDE.md's own "if this file and PROMPT.md disagree, treat that as a
bug" rule, confirmed the change is deliberate: the real target is production-grade, not lab-grade.

**Decision:** The project's quality bar is raised to production-grade. CLAUDE.md's Reality Check
section is updated to match (see that section's amended text) rather than left contradicting the
goal statement.

**Immediate consequence -- work already built must be revisited:**
- **ADR-0005 (h2c only, no TLS/mTLS)** was justified as "an acceptable Phase 0 lab
  simplification." Under a production-grade bar it is not acceptable as a final state -- real
  TLS 1.3 + mTLS (per TS 33.501 and the non-negotiable engineering rules) must be added before
  Phase 2 NFs talk to anything beyond the throwaway stub-nrf. Not fixed in this ADR; tracked as
  required follow-up, first candidate being before/during Phase 2's NRF work since NRF is the
  trust anchor every other NF's OAuth2 flow depends on.
- **stub-nrf's unsigned, fake OAuth2 token** similarly stops being acceptable once real NFs need
  to validate tokens against it -- the real NRF (Phase 2) must issue and NFs must validate
  properly signed JWTs.
- **ADR-0006 (synchronous libcurl client)** and the ~40 outstanding `clang-tidy` style warnings
  noted in PHASE0-NOTES below become real technical debt against a production bar, not
  acceptable-forever lab shortcuts. Not urgent individually, but should not accumulate further
  without a plan to close them.
- Every future phase's Definition of Done should be read as production-grade from here forward:
  full TS 23.502 procedure coverage (not "mandatory only" read loosely), real conformance testing,
  and no silent gaps -- consistent with CLAUDE.md's existing "flag stubs/simplifications honestly"
  rule, which does not change, only the bar for what's an acceptable *permanent* state does.

**Rejected alternative:** Revert CLAUDE.md's goal line back to "lab-grade" to match PROMPT.md and
avoid the rework above. Rejected -- user explicitly confirmed production-grade is the intended
target after the conflict was surfaced.

**Note:** PROMPT.md itself is left unedited as the historical record of the original brief;
CLAUDE.md is the living document and is what future sessions should treat as authoritative on this
point going forward.

---

