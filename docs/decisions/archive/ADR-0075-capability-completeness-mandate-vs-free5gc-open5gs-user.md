## ADR-0075: capability-completeness mandate vs free5GC/open5GS (user-directed, mandatory)

**Status:** Closed (work pushed; last citing commit aeb260a on origin/main, 2026-10-06).

### Context

The user directed a full capability gap-analysis sweep of every built NF against real free5GC and
open5GS source (`docs/CAPABILITY_GAP_ANALYSIS.md`, in progress). Partway through the AMF pass --
which surfaced large, real, structural gaps (3 of 4 `Namf_*` services entirely missing, the
`ServiceRequest` NAS procedure entirely missing, ~4 of ~39 real NGAP procedures implemented, zero
N2 handover support) -- the user issued an explicit, emphatic standing directive: **do not skip
any capability found in either reference implementation; this project must always end up superior
to both free5GC and open5GS, never behind, on every NF compared.**

### What this means, concretely

Extends ADR-0049's commercialization mandate ("performance and reliability must exceed free5GC,
not just match it") from a performance-only claim into an explicit **capability-completeness**
mandate: every real capability either reference NF has, this project's equivalent NF must
eventually have too -- not "the important ones," not "what fits this pass." Every real gap found
during the sweep goes on the permanent record in `docs/CAPABILITY_GAP_ANALYSIS.md` as something
that WILL be implemented, never silently triaged out. If a found gap turns out to be genuinely
inapplicable to this project's own architecture, that is a case for asking the user before
excluding it, not a unilateral judgment call -- matching this project's own established
stop-and-ask-survives-autonomy precedent.

### What this does NOT change

Pacing and sequencing (which NF next, how deep per turn, one-subsystem-per-turn with
procedure-list approval) remain governed by this project's own already-established working style.
This mandate is about **end-state completeness**, not a demand to implement the entire gap list in
a single pass -- staging is fine; silently dropping a found gap from the plan is not. Also
recorded in the assistant's own cross-session memory
(`project_capability_superiority_mandate.md`) so the mandate survives context compaction and future
sessions, not just this one.

### Status

The gap-analysis sweep itself is still in progress (`docs/CAPABILITY_GAP_ANALYSIS.md`: NRF and AMF
sections complete as of this ADR; AUSF, PCF, SMF, UDM, UDR, UPF, CHF still pending). No
implementation against any found gap has started yet -- this ADR records the mandate itself, not
a completed body of work.

