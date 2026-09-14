# Architecture diagram — conventions

The diagram and the product/license table live in `README.md`, because that is what a reader sees
first. This file is the rulebook for keeping them true.

## Legend

- **Solid tile**: exists in this repository *and* has a test or a live run behind it. Its
  `source` in `docs/diagrams/architecture.json` names the directory, file or compose service,
  and CI fails if that does not exist.
- **Dashed tile** (`"planned": true`): in scope per `CLAUDE.md`, not built. A tile moves from
  dashed to solid in the same commit that lands the NF's first tested service — never before.
- **Edge label**: the reference point as TS 23.501 §4.2.7 names it, the service operation, and
  the stage-3 TS (N7 · Npcf_SMPolicyControl · TS 29.512); non-3GPP interfaces carry the protocol
  (Gy, CAP, TMF620). No edge is drawn for a call that does not happen in code; a call that has no
  3GPP reference point (SMF reading `influenceData` from the UDR directly) is labelled with the
  ADR that discloses it.
- **Chip**: the number of services the NF serves from generated R19 YAML.
- **Datastore tiles** list their clients instead of receiving a line from each -- the lines drawn
  to datastores are the ones that carry a pipeline (CHF → Kafka → Doris → feature store).

## When it must be updated

Part of every NF's definition of done (`CLAUDE.md`). Concretely, the diagram or table changes
when any of these does:

1. an NF or BSS component is added, or first gets a passing integration test;
2. a datastore, broker, or runtime product is added, removed, or **changes version across a
   license boundary** (Redis 7.2 → 7.4 was exactly that, and the table exists so it is seen);
3. a reference point is wired for the first time;
4. something on the dashed list is built.

## Why a rendered SVG from a JSON source, not Mermaid (ADR-0361, superseding ADR-0356 here)

ADR-0356 chose Mermaid so that the diagram would be text and need no render step. The rendered
result was judged not fit for a professional README against the project's own R19 poster
(`specs/5G-Advanced_Protocols_and_Node_Capabilities_R19_A1_print.pdf`): Mermaid gives no control
over placement, iconography or label typography, and the result read as a box-and-arrow sketch.
What survives from ADR-0356 is the property that mattered -- the diagram stays true without
anyone remembering -- delivered differently:

- the *source* is still text (`docs/diagrams/architecture.json`) and diffs with the code;
- the render step is a CI check (`tools/diagrams/check_readme_sync.py` re-renders and fails on
  a stale SVG), so it cannot be forgotten, only failed;
- the renderer is stdlib Python with no 5G knowledge -- it cannot add a box, it lays out what
  the JSON says;
- light and dark variants are generated from one source and GitHub picks one via `<picture>`,
  like the SVG banners above the diagram.

Edit the JSON, run `python3 tools/diagrams/render_architecture.py`, commit the JSON and both
SVGs together.

## Why the license column is there

P1 is strict OSI-only. The column is the audit. Two products that would have passed a glance did
not pass the table: Redis 7.4 (RSALv2/SSPL — replaced by Valkey, ADR-0044/0356) and tl-expected
(CC0-1.0, which OSI declined to list — flagged, not hidden). A product's license is taken from
its port metadata or its own LICENSE file; nothing in the table is recalled.
