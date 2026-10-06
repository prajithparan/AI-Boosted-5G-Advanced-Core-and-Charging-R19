## ADR-0001: Greenfield build, no fork of Open5GS or free5GC

**Date:** 2026-08-04
**Status:** Accepted

**Context:** PROMPT.md Section 2 flags build-vs-fork as the decision most likely to dominate this
project's economics, and asks for a build-vs-fork analysis as a first deliverable.

**Decision:** Build greenfield. Open5GS (C) and free5GC (Go) remain reference reading only.

**Rejected alternative:** Fork or structurally borrow from Open5GS/free5GC. Rejected per explicit
user instruction ("Greenfield from scratch"), given without requesting the comparative analysis
first.

**Consequence:** No shortcut on control-plane procedure implementations later; everything is
generated from the R19 YAML or hand-written against the spec text, per the source-of-truth rule.

---

