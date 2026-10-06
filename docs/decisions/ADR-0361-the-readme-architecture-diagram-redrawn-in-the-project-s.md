## ADR-0361: the README architecture diagram, redrawn in the project's own poster language

**Date:** 2026-09-14. **Status:** accepted. User-directed: *"The diagram generated is not
professional one and not good at all"* -- with `specs/5G-Advanced_Protocols_and_Node_Capabilities_
R19_A1_print.pdf`, the user's own A1 poster of R19 protocols and node capabilities, named as the
reference for how the architecture should be drawn.

**What the poster does that Mermaid could not.** Each NF is an icon tile with its acronym, its
full name, the TS numbers behind it and the service operations it exposes; NFs are grouped into
functional areas with headed panels; every interface is an orange line labelled with its
reference point and the TS that defines it; release badges sit on the tile corner. A Mermaid
flowchart has none of that vocabulary -- no icons, no typographic hierarchy inside a node, no
control over routing -- and the result was a box-and-arrow sketch. ADR-0356 had picked Mermaid
for a reason that still matters (text source, no render step to forget); this ADR keeps the reason
and changes the mechanism.

**Decision.**

- `docs/diagrams/architecture.json` is the source: areas, tiles (acronym, name, TS, services,
  glyph, colour, `source` path, `planned`), edges (`from`, `to`, label, route). The renderer,
  `tools/diagrams/render_architecture.py`, is ~400 lines of stdlib Python that lays tiles on the
  grid the JSON dictates, routes orthogonal edges (auto elbows, or explicit `via` points where
  the auto route would cross a tile), and emits `architecture.svg` and `architecture-dark.svg`.
  It knows nothing about 5G; it cannot add a box.
- The README embeds both through `<picture>`, as the title/motto banners already do; the image
  links to the full-size SVG.
- `tools/diagrams/check_readme_sync.py` (already in the CI lint job) now: re-renders and fails
  on a stale SVG; fails if the README does not embed both files or still carries the Mermaid
  block; and -- new -- fails if any solid tile's `source` (a directory, a file, or a
  `compose:<service>`) does not exist. That last check is what turns "the diagram is an audit
  surface" from a convention into a build failure: 37 solid tiles resolve today, 10 are planned.
- Content rules, now written into `docs/ARCHITECTURE.md`: an edge is drawn only for a call that
  is wired in code (`config/*.json` `*_base_url`, or a client call in the NF's source), labelled
  with the reference point as TS 23.501 §4.2.7 names it (TS 23.501 V19.8.0 fetched for this,
  `specs/3gpp/TS_23.501_j80.txt`), the service operation, and the stage-3 TS. Datastore tiles list
  their clients rather than receive a line from each. No release badges: which release introduced
  an NF is not something this repository can cite from a spec it holds, so the chip carries a
  number it can -- services served from generated YAML.

**What drawing it honestly showed.** Deriving edges from config rather than from the old Mermaid
made three things visible that the old diagram glossed: (1) `nfs/amf/src` contains no Nudm
client call and `config/amf.json` no `udm_base_url` -- N8 is not wired (the AUSF's N13 is the
only AMF-side route into UDM data), so no N8 line is drawn; (2) the SMF reads `influenceData`
from the UDR directly, a path with no TS 23.501 reference point, disclosed in ADR-0320 and now
labelled as that on the diagram; (3) by the same two greps, AMF→NSSF (N22) and AMF→5G-EIR (N17)
are not wired either, although both NFs serve their APIs. These are findings, not fixes -- recorded here so the gaps are chosen, not
discovered. The BSF is drawn in the SBI-support column next to the NRF and SCP (a grouping of this
diagram -- functions other NFs use to find and route to each other -- not a clause of the spec),
rather than in the control-plane grid, where its tile sat on two reference-point lines.

**Rejected.** *Hand-editing the SVG* -- unauditable, and the first content change would rot it.
*Graphviz/dot for layout* -- a dependency in the lint job, and its routing is what makes diagrams
look generated; the poster's layout is deliberate, so the JSON carries grid positions and the
renderer honours them. *Keeping Mermaid and adding an SVG export* -- two sources of truth for
one diagram. *Release badges copied from the poster* -- would be recall, not citation.

