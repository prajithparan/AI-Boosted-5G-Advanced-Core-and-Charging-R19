# Reality check

Moved verbatim from CLAUDE.md (section "Reality check..."); see `docs/optimization/MOVE_LOG.md`.

- A conformant multi-NF 5GC is a multi-engineer, multi-quarter program.
  Open5GS and free5GC each represent years of work, and neither covers
  R19. The target is production-grade (ADR-0009), which is a
  substantially larger undertaking than the project's original lab-grade
  framing — treat every phase's Definition of Done as the real bar, not
  an aspiration: full procedure coverage, real TLS/mTLS, no permanent
  stubs. A single solo session will not get there in one pass; this is
  still built incrementally, phase by phase, NF by NF — the destination
  changed, not the pace.
- Build-vs-fork (study/fork Open5GS (C) or free5GC (Go) vs. greenfield)
  is a first-order decision affecting project economics — resolved
  greenfield (ADR-0001).
- Dev machine (MX450, CUDA 12.6) is fine for control-plane dev and ONNX
  inference; UPF datapath and serious model training will want a larger
  lab tier.
- Known debt against the production-grade bar, tracked in
  `docs/DECISIONS.md` ADR-0009: Phase 0's h2c-only transport (no
  TLS/mTLS yet), the synchronous HTTP/2 client, and ~40 outstanding
  `clang-tidy` style warnings. None of these are acceptable as a final
  state anymore. (The "unsigned fake OAuth2 token" once listed here is
  resolved: tokens are ES256-signed JWTs, `libs/sbi-core/src/jwt.cpp`.
  The h2c-only transport is likewise resolved: every SBI is TLS 1.3 +
  mTLS. Verified during the 33-series review, `docs/SECURITY_COMPLIANCE.md`.)
