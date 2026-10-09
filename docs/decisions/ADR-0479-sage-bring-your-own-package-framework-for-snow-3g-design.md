## ADR-0479: SAGE bring-your-own-package framework for SNOW 3G (design only)

**Date:** 2026-10-09. **Status:** Proposed.

**Context.** Finding F2 needs 128-NEA1/NIA1 (SNOW 3G). The normative material (ETSI SAGE UEA2/UIA2 Documents 1-4) is licence-restricted; the 3GPP TS 35.215-218 contain only a pointer (snow3g/docs/sources.md). The user asked for a plug-and-play framework so any operator holding the SAGE package can integrate it and generate the code locally.

**Decision (design only).** Full design in `snow3g/docs/framework-design.md`: a stable C provider ABI plus loader with hash pin and fail-closed AMF wiring (NEA1/NIA1 offered only when a provider passes the operator's own known-answer self-test); an importer (`sage-import`) that fingerprints the operator's package and generates a local, untracked provider library with a traceability file; an enforced guard keeping restricted material out of git, images and logs. Steps F1-F2 (ABI, loader, guard, ingest) need no SAGE content and can be built now; F3-F5 (extractors, emit/build, real KAT run) are blocked on seeing the licensed package layout and on a Fork A/B choice (wrap the reference C source vs transcribe from document text).

**Disclosed.** Nothing implemented. No parser or algorithm detail was invented. Whether the SAGE undertaking permits operators to ship a generated library to their own AMFs is a legal question left to the operator.

**Rejected alternatives.** Vendoring a third-party SNOW 3G implementation or Open5GS code (licence unverified, and the instruction was not to copy it). Committing the tables/vectors to the repo (restricted). A compile-time-only integration (forces the restricted material into our build and images). Writing the algorithm from memory (no verification path, against project rules).
