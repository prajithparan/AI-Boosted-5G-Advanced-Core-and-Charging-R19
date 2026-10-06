## ADR-0356: the architecture diagram as an audit surface, and Redis replaced by Valkey

**Date:** 2026-09-14. **Status:** accepted. User-directed: a diagram of core components, datastores
and open-source products, published in the README and updated after each phase.

### Decisions

- **Mermaid, in the README, with the source in `docs/diagrams/`.** Text diffs with the code and
  changes in the same commit as the change it describes; GitHub renders it in the viewer's theme.
  A rendered image needs a render step, and "update after each phase" does not survive a step
  someone has to remember. Two copies are unavoidable (GitHub cannot include a file into a fenced
  block), so `tools/diagrams/check_readme_sync.py` runs in the lint job and fails on drift.
- **Built and planned are drawn differently.** Solid = exists and has a test or live run; dashed =
  in scope, not built. A box moves in the commit that lands the NF's first tested service. A
  diagram showing NWDAF or LI as solid today would be the overclaim this project forbids.
- **A product/license column, sourced from port metadata and LICENSE files, not recall.** P1 is
  strict OSI-only and the column is the audit.
- **Diagram upkeep is definition-of-done item 9 in CLAUDE.md**, so it is a checklist entry rather
  than a memory. The other diagram types the user asked for (deployment, ERD, object, security,
  sequence, NWDAF internal) are recorded as tasks D1-D10 in `docs/diagrams/README.md`, each with
  the artefact it must be drawn from; none is drawn from something that does not yet exist.

### What the audit surface caught before the first box was drawn

- **The lab ran Redis 7.4.10.** ADR-0044 mandates Valkey precisely because Redis relicensed to
  RSALv2/SSPL -- and 7.4 is the first such version. Compose still said `redis:7-alpine`, and that
  tag floats. Swapped to `valkey/valkey:8-alpine` in compose and both CI jobs; RESP-compatible, no
  client change. Verified with live traffic: CHF and AMF connect, a charging Create returns 201,
  CHF's `chf:cdr`, `chf:cdr:content` and `chf:idem` keys land in Valkey.
- **`tl-expected` is CC0-1.0**, a public-domain dedication OSI declined to list in 2012. Permissive,
  header-only, and not on the approved list. Flagged in the table for a P1 decision rather than
  hidden behind "permissive".
- **`librdkafka`'s port metadata carries no license field.** Its LICENSE file is BSD-2-Clause;
  recorded from the file.
- **Every NF aborts on an unreachable Redis at startup** (`sw::redis::IoError` uncaught). Found
  by the swap, recorded as blocker 0a in `COMPLIANCE_P1_P15.md`, not fixed here.

### Rejected

- A rendered SVG as the source of truth -- render step, above. An SVG *export* for use outside
  GitHub is fine later; Mermaid stays the source.
- Drawing the pending diagrams now from intent rather than from artefacts. The user asked for
  them as tasks; a diagram of a table that does not exist is a promise wearing a diagram's clothes.

