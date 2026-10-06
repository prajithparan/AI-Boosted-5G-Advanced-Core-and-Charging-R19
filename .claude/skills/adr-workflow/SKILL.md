---
name: adr-workflow
description: Add or look up an Architecture Decision Record in this repo. Use whenever recording an architectural decision, citing an ADR, or checking what an ADR says - docs/DECISIONS.md is an index, never edited by hand.
---

- New ADR: `python3 scripts/docs/new_adr.py "<title>"` (next number, template, index rows); then fill the file in `docs/decisions/ADR-NNNN-*.md`. Never edit `docs/DECISIONS.md` by hand.
- Look one up: `grep -n "ADR-0xxx" docs/DECISIONS.md` for the file, or `docs/decisions/INDEX_DETAIL.md` for status/date/decision; then Read the single file.
- After any edit that adds/renames ADR files: `python3 scripts/docs/check_adr_index.py` (fails on drift/duplicates).
- Record rejected alternatives and every disclosed stub/simplification (CLAUDE.md guardrails 3-4).
