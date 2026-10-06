# CLAUDE.md — 5G-Advanced (R19) Core Ecosystem

This file is the standing brief for every session working in this repository.
It condenses `PROMPT.md`. If this file and `PROMPT.md` ever disagree, treat
that as a bug — flag it, don't silently pick one.

## Project goal

A modular, standards-faithful 5G Core (5GC) **and Charging (CHF + Online
Charging, Gy/CAP)**
implementation in modern C++, targeting 3GPP **R19 (5G-Advanced)**. R19 is
what 3GPP itself brands 5G-Advanced; 6G has no stage-3 specification yet and
nothing here implements it, so it is deliberately absent from the title. When
Release 20 lands and 3GPP defines 6G, the intent is to carry this architecture
forward and revisit the name then — a statement of direction, not a capability
claim. Every Network Function's northbound API is **generated** from the
official 3GPP OpenAPI YAML (forge.3gpp.org/rep/all/5G_APIs, REL-19 branch) —
never hand-written — with a TM Forum SID-aligned charging/BSS domain, a
JSON-schema-driven operator GUI, and AI/ML pipelines wired into **both NWDAF
and the CHF**. Target: a production-grade, spec-traceable reference
implementation (raised from the original lab-grade scope — see ADR-0009 in
`docs/DECISIONS.md`).

## Source of truth (strict)

- 3GPP OpenAPI YAML (REL-19) is the ONLY source for API shapes, paths,
  schemas, and enums. Never hand-write a DTO the YAML can generate.
- If a YAML file is unavailable offline: **stop and ask**. Never invent
  field names, TS numbers, or reference points. Fabrication is the single
  worst failure mode on this project.
- Every generated NF carries a header comment citing its TS number and the
  exact YAML file + commit/branch it was generated from.
- Stage-2 behaviour references: TS 23.501 (architecture), TS 23.502
  (procedures), TS 23.503 (policy), TS 23.288 (NWDAF), TS 33.501 (security),
  TS 32.240/32.290/32.291 (charging), TS 29.500/29.501 (SBI framework).
- Where stage-3 YAML is genuinely missing for an R19 item (e.g. parts of
  AIOTF), implement against stage-2 and mark the gap explicitly — never
  invent an API to fill it.

## Non-negotiable engineering rules

- C++20 minimum, C++23 where the toolchain allows.
- No raw `new`/`delete`; RAII everywhere; `std::expected` / `tl::expected`
  for recoverable errors; exceptions only at API boundaries.
- Every NF is an independent binary + shared library, buildable standalone.
- No NF includes another NF's private headers. NFs talk ONLY over SBI.
- 100% of SBI traffic is HTTP/2 + JSON per TS 29.500: correct
  `ProblemDetails` on errors, `3gpp-Sbi-*` headers, OAuth2 tokens from NRF.
- Test-first for protocol logic: every procedure gets a test derived from
  the TS 23.502 call flow it implements.
- All third-party dependencies must be OSI-approved open source. No
  proprietary SDKs, no vendor lock-in, no closed binaries.

## Mandated tech stack

- **Build/tooling**: CMake 3.28+, Ninja, vcpkg (manifest mode),
  clang-format, clang-tidy, sanitizers (ASan/UBSan/TSan) in CI.
- **HTTP/2 + SBI**: nghttp2 (core), Boost.Beast or Pistache (service
  layer), libcurl (client), OpenSSL 3.x (TLS 1.3, mTLS).
- **Testing**: GoogleTest, Catch2, gMock, libFuzzer for codec fuzzing.
- **Deployment**: Docker + Compose (lab), Helm charts (k8s), all
  reproducible from a single `make lab-up`.

Remaining stack bullets (Codegen, JSON, Async, PFCP/UP, Storage, Observability, GUI, AI/ML): `docs/project-context/tech-stack-details.md`.

## Scope: Network Functions (R19)

- **Tier 1** (must, Phase 2-3): NRF, AMF, SMF, UPF, PCF, UDM, UDR, AUSF,
  NSSF, NEF, SCP, BSF, CHF.
- **Reference points**: every reference point in TS 23.501 §4.2.7 that is
  in scope for the implemented NFs, N1-N115. The numbering is sparse by
  design (N25, N39, N53-54, N64, N69, N72-79, N98 never assigned;
  N44-49/N100-109 reserved to TS 32.240; N90-95 to TS 23.503). Never
  invent numbers to fill gaps.

Tier 2 / Tier 3 lists: `docs/project-context/scope-tier2-tier3.md`.

## Charging domain: 3GPP + TM Forum SID

- CHF per TS 32.290/32.291 (`Nchf_ConvergedCharging`,
  `Nchf_SpendingLimitControl`, `Nchf_OfflineOnlyCharging`) with N40 (SMF),
  N28 (PCF), N41/N42 (AMF, home/visited).
- SID-aligned BSS layer mapping 3GPP charging events onto TM Forum SID
  entities (Product, Service, Resource, Customer, Party, Agreement,
  ProductOffering, ProductPrice, AppliedCustomerBillingRate, CustomerBill,
  BalanceTopUp, Event), exposed via TMF620/622/632/633/635/637/638/639/651/
  654/666/676/678/688/727.
  (TM Forum API extension history: `docs/project-context/tmforum-extension-history.md`.)
- Deliverable before any mapping code: `docs/CHARGING_MAPPING.md` — an
  explicit, reviewable table of 3GPP CDR field -> SID entity -> TMF API
  resource. Ambiguous mappings are marked TODO and asked about, never
  silently invented. Align to TM Forum ODA component boundaries so the
  BSS layer could be swapped for a commercial stack.

## Definition of done (per NF)

Nine items; full text in `docs/project-context/definition-of-done.md` (skill `new-nf-checklist`). The one always-visible rule:

7. Docker image + Compose entry + Helm chart. The compose entry's own
   `volumes:` MUST include `- ../../config:/build/config:ro` from the
   moment it is written -- never discovered missing later by a crash
   (ADR-0453, user-directed, mandatory: this class of mistake, a fresh
   NF container unable to even start because no mechanism ever shipped
   it its own `config/<nf>.json`, found live across ~25 of 28 existing
   NF services and must never recur).

## Working style for this project

- Work in small, reviewable increments. Never generate more than one NF
  (or one subsystem) per turn.
- Before writing any NF, show the TS 23.502 procedure list to be
  implemented and get approval first.
- When the spec is ambiguous or YAML is missing: **stop and ask**. A
  question costs a minute; a fabricated field costs a week of review.
- Keep `docs/DECISIONS.md` (ADR format) current for every architectural
  choice, including rejected alternatives and why.
- Flag honestly what is a stub, a simplification, or non-conformant.
  The user is a telecom architect and will spot it — say so first, don't
  let it be discovered in review.

## Guardrails (repeat when output starts drifting)

1. Never invent a TS number, reference point, API path, or JSON field.
   If it's not in the YAML or spec in hand, ask.
2. One NF (or one subsystem) per turn. Show the procedure list for
   approval before implementing.
3. State explicitly what is a stub, what is simplified, and what is not
   conformant.
4. Every architectural decision goes in `docs/DECISIONS.md`, including
   rejected alternatives.
5. If unsure whether something is correct, say so plainly rather than
   producing confident code.

## Context discipline

- Never Read `docs/DECISIONS.md` content whole, nor `PROMPT.md`, `CHARGING_PROMPT.md`, `README.md`, `docs/TRACEABILITY.md`.
- ADRs: `docs/DECISIONS.md` is an index; `docs/decisions/INDEX_DETAIL.md` has status/date/decision. Then `grep -n "^## ADR-0xxx"` and Read the one file with offset/limit.
- Specs: grep the schema name, then Read only that range (skill `spec-lookup`). Never Read a generated header or a spec YAML whole.

## Compact instructions

When compacting, preserve: ADR numbers touched this session, the approved TS 23.502 procedure list, failing test names, disclosed stubs and simplifications, open questions.

## Session start

Read CLAUDE.md, the `docs/DECISIONS.md` index, the last 5 ADR files in `docs/decisions/` and the tail of `docs/TRACEABILITY.md`. This replaces the context-reset prompt in `PROMPT.md`.

## Moved context (verbatim; map in `docs/optimization/MOVE_LOG.md`)
- `docs/project-context/`: `charging-title-rationale.md` (why "OCS" is not the title), `ai-pipelines.md` (NWDAF; training only in the Python sidecar, inference in-process C++), `phase9-production-documentation.md`, `kickoff-decisions.md`, `reality-check.md`, `commercialization-mandate.md` (ADR-0049), `adr-0445-charging-dataplane-debt.md`.
- Adding an ADR: skill `adr-workflow` (`scripts/docs/new_adr.py`); never edit `docs/DECISIONS.md` by hand.
