# Phase D -- duplicate paragraphs (report only; nothing applied)

- Across all `*.md` outside `docs/DECISIONS.md` (before the split): **0** exact duplicate paragraphs (>= 200 normalised chars).
- Among the split ADR files: 7 groups of identical paragraphs, almost all in the "gap-closure task 106 continuation" series
  (ADR-0106..0162: 15 + 11 + 10 + 6 + 5 files in the larger groups) plus ADR-0172/0173 and ADR-0180..0182. They are
  repeated boilerplate inside historical ADRs. Replacing them with a link would edit the ADRs' text, so none was touched;
  list the groups with `scripts/docs`-style grep on request and approve per group before anything changes.
