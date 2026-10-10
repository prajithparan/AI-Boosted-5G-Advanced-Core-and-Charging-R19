---
name: new-nf-checklist
description: Definition-of-done checklist for a new or changed 5GC Network Function - generated API, NRF registration, TS 23.502 procedures, ProblemDetails, telemetry, conformance test, Docker/Compose/Helm (compose config:ro mount, ADR-0453), traceability and README diagram. Use when starting, reviewing or closing out any NF.
---

Read `docs/project-context/definition-of-done.md` (the nine items, verbatim from CLAUDE.md; the opening of item 7 lives only in CLAUDE.md's "Definition of done" section) and walk every item for the NF at hand.
Item 7 is the one that has bitten before: the compose entry's `volumes:` MUST include `- ../../config:/build/config:ro` from the moment it is written (ADR-0453); a Dockerfile `COPY` is not a substitute.
Before writing a new NF, show the TS 23.502 procedure list and get approval (CLAUDE.md "Working style").
