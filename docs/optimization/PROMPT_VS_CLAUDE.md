# PROMPT.md vs CLAUDE.md -- divergences found (evidence only; PROMPT.md not edited)

CLAUDE.md says it "condenses PROMPT.md" and that a disagreement is a bug. Found by targeted grep (line numbers are PROMPT.md's):

| # | PROMPT.md says | CLAUDE.md says | Kind |
|---|---|---|---|
| 1 | Standing rule 5 (~L463): never report a metric you have not measured, never call anything production-ready without evidence | absent (only the ADR-0445/0049 "no claim without a baseline" lines, scoped to CHF) | **missing from CLAUDE.md** |
| 2 | Standing rule 6: in the charging domain the model informs, the deterministic engine decides | absent | **missing from CLAUDE.md** |
| 3 | Standing rule 7: configuration over code -- a business change that needs a recompile is a redesign | absent (only the narrower "no hardcoded config" memory/ADR-0077) | **missing from CLAUDE.md** |
| 4 | Standing rule 4: each ADR "states which of P1-P15 it satisfies or strains" (principles at L39+, 9 mentions of P15) | Guardrail 4 stops at "including rejected alternatives"; P1-P15 appear nowhere in CLAUDE.md | **missing from CLAUDE.md** |
| 5 | ADRs live in `docs/DECISIONS/` (a directory); context reset reads `docs/DECISIONS/` | `docs/DECISIONS.md`; now an index plus `docs/decisions/` (lower-case) | path/case drift -- `docs/DECISIONS/` does not exist on Linux |
| 6 | Context-reset prompt (L451): read PROMPT.md, CHARGING_PROMPT.md, DECISIONS, TRACEABILITY and summarise | Session start block (new): CLAUDE.md, ADR index, last 5 ADRs, TRACEABILITY tail | intentional replacement (token cost); PROMPT's version is superseded |
| 7 | L422: "excellent for a lab ... reference implementation" | production-grade target (ADR-0009) | historical, superseded by ADR-0009 |
| 8 | Title/scope "Universal Charging System" (L1); P5 "100% TM Forum Open APIs and SID" | "CHF + Online Charging", explicit TMF list (OCS/UCS deliberately not the title) | narrowed on purpose, recorded in CLAUDE.md |
| 9 | No TMF numbers at all (grep finds none) | TMF620...727 list, extended 2026-08-10 | CLAUDE.md more specific; the CLAUDE.md text calling the older list "original" refers to an earlier CLAUDE.md revision, not PROMPT.md |

Resolved 2026-10-06: items 1-3 added to CLAUDE.md as Guardrails 6-8 (user-directed). Item 4 (the P1-P15 clause of rule 4) is NOT added -- the P1-P15 list is not in CLAUDE.md, so the clause would dangle; needs a decision.
