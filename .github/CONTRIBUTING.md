# Contributing

This is a standards-faithful 5G Core and Charging implementation (3GPP R19). Its value depends on
staying spec-accurate, so please read this before opening a PR.

**Status of these rules.** Items marked **(planned)** are agreed but not yet in force or not yet
automated; everything else applies now. The project is preparing for open contribution and the
maintainer will announce when outside PRs are welcome (see [Licensing and the CLA](#licensing-and-the-cla)).

## How the project is run

- One maintainer owns the final merge, releases and version tags. Nothing reaches `main` without
  their merge. Review feedback is advisory from area reviewers and binding from the maintainer.
- Work happens from forks and pull requests. Every PR from someone without write access waits for
  maintainer approval before CI runs (repository setting, in force).
- Versioning is the maintainer's: SemVer with the 3GPP release, with tags cut by the maintainer
  **(planned)**. Do not bump versions in a PR.

## Pick one area

Each PR belongs to **exactly one** area and touches only that area's paths. This keeps review
tractable and lets contributors with domain experience stay in the part they know.

| Area | Paths | Typical background |
|---|---|---|
| Core | `nfs/{amf,nrf,smf,pcf,scp,nssf,nef,bsf,upf,smsf,gmlc,lmf,nsacf}` | 5GC SBI, session/mobility procedures |
| Provisioning | `nfs/{udm,udr,ausf,udsf,eir}` | subscriber data, authentication |
| Charging / BSS | `nfs/chf`, `bss/`, `libs/{bss-sid,cap-core,diameter-core,ss7-core,tcap-core,map-core,tap3-core,tbcd-core}` | online/offline charging, TM Forum SID |
| Products and Catalog | `bss/product-catalog`, product and offer definitions (Consumer, Enterprise) | telco product catalog, pricing, bundles |
| Security / LI | `nfs/li-*`, `libs/li-*` | lawful interception, TS 33.x |
| NWDAF / Analytics | `nfs/{nwdaf,adrf,dccf,mfaf}`, `libs/analytics-features` | TS 23.288, ML pipelines |
| Automation / Deploy | `deploy/`, `scripts/`, `.github/`, `tools/` | Docker, Helm, CI |
| GUI | `gui/` | React, JSON Forms |
| Platform | `libs/{sbi-core,nf-config,sbi-generated,ngap-core,ngap-generated,pfcp-core,event-bus}`, `nfs/hello-nf` | maintainer-only, see below |

- Paths not listed (`agents/`, `simulators/`, `specs/`, `docs/`, repo-root files) are assigned by the
  maintainer on the issue. A test lives with the area it tests.
- A change that needs two areas is split into one PR per area, or agreed first in an issue and
  labelled `cross-area` by the maintainer.
- **Platform libraries are maintainer-only.** A change there affects every NF. Open an issue
  describing the need; do not send a PR.
- Enforcement is by `CODEOWNERS`, path labels and a CI check that rejects multi-area PRs
  **(planned)**. Until then the maintainer enforces it in review.
- Not sure where something belongs? Ask in an issue first; the area table above is the source of
  truth and may grow.

## Ground rules (non-negotiable)

These come from [`CLAUDE.md`](../CLAUDE.md), which governs every change here:

- **The 3GPP OpenAPI YAML (REL-19, vendored under [`specs/`](../specs/)) is the only source** for
  API shapes, paths, schemas and enums. Never hand-write a DTO the YAML can generate.
- **Never invent a TS number, reference point, API path or JSON field.** If the spec you need is not
  in the repo, open an issue instead of guessing. A fabricated field costs far more review time than
  a question.
- **Say what is a stub, a simplification or non-conformant**, in the PR description. Do not let a
  reviewer find it.
- **C/C++/Python only for new code.** The one exception is the operator GUI (React + JSON Forms).
- **No raw `new`/`delete`.** RAII everywhere; `std::expected`/`tl::expected` for recoverable errors;
  exceptions only at API boundaries.
- **NFs talk only over SBI** (HTTP/2 + JSON, TS 29.500). No NF includes another NF's private headers.
- **Configuration over code.** A business change (rates, offers, thresholds) must not need a
  recompile. Runtime settings go in `config/<nf>.json`, never as a literal in a `.cpp`.
- **Charging: the model informs, the deterministic engine decides.** ML output may inform the
  charging engine; it never makes the charging decision.
- **Third-party dependencies must be OSI-approved open source.** No proprietary SDKs or closed
  binaries, and every datastore must be self-hostable open source.

## Before you start

- For anything beyond a small fix, open an issue first (use the templates and pick the area), so
  scope and spec references are confirmed before you spend time.
- Keep PRs small: one NF or one subsystem each. Large multi-subsystem PRs are asked to split.
- A new NF is built in its own turn of work with its TS 23.502 procedure list agreed first. Post the
  list in the issue and wait for the maintainer's approval before writing code.

## Setting up and building

Requirements: CMake 3.28+, Ninja, GCC 13 or Clang 18, `clang-format-18`, Docker (for service
containers used by integration tests), and [vcpkg](https://github.com/microsoft/vcpkg) in manifest
mode. A one-command installer (`scripts/bootstrap.sh`) with pinned, checksum-verified versions is
**(planned)**; until it exists, install these yourself.

```sh
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics
scripts/setup-asn1c.sh          # builds the patched asn1c used by the ASN.1 targets; idempotent

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build --target <nf>      # build one NF instead of everything
ctest --test-dir build --output-on-failure
```

- **A full build is slow**: the generated `sbi_generated` sources dominate it and a cold build can
  take around two hours. Build only the target you are changing, and keep the vcpkg binary cache.
  A shared prebuilt cache is **(planned)**.
- Sanitizer builds: `-D5GC_ENABLE_ASAN=ON` or `-D5GC_ENABLE_TSAN=ON` (mutually exclusive). See
  the "Building" section of the [`README`](../README.md).
- Integration tests start real NF processes on fixed loopback ports. Stop any NF you started by
  hand before running `ctest`, or tests collide with it.
- Format every file you touch before committing: `clang-format-18 -i <files>`. (Plain
  `clang-format` is not the version CI uses.)

CI ([`ci.yml`](workflows/ci.yml)) runs four jobs: `build`, `sanitize (asan-ubsan)`,
`sanitize (tsan)` and `lint` (`clang-format` + `clang-tidy`). All four must pass.

## What a PR must include

- **Code citing its source** in a header comment: the exact YAML file, or the TS document and
  clause, in the style already used across the codebase.
- **Tests derived from the TS call flow** the change implements. Where the change touches a running
  service, live verification (real process, real database, real mTLS) is preferred over
  self-consistency tests alone.
- **An ADR** for any decision beyond a trivial fix, including rejected alternatives and disclosed
  stubs. Create it with the script, never by editing the index by hand:
  `python3 scripts/docs/new_adr.py "<title>"`, then fill in the new file under `docs/decisions/`,
  then run `python3 scripts/docs/check_adr_index.py`. ADR numbers are sequential, so two open PRs
  can collide on a number: rebase, re-run the script's check and take the next free number. The
  maintainer may renumber at merge.
- **A `docs/TRACEABILITY.md` entry** mapping procedure to TS clause, source file and test.
- **Docs kept current in the same PR**: the README, the architecture diagram and any doc whose
  statement your change makes false. Out-of-date docs are a reason to hold a merge.
- **For a new NF**, the full Definition of Done ([`docs/project-context/definition-of-done.md`](../docs/project-context/definition-of-done.md)),
  including a Docker image, Compose entry and Helm chart. The Compose service's own `volumes:` must
  include `- ../../config:/build/config:ro` from the start; a container without it cannot start.
- The checklist in [`PULL_REQUEST_TEMPLATE.md`](PULL_REQUEST_TEMPLATE.md), filled in honestly.

## AI-assisted contributions

AI tools are welcome; this project is built with them. You remain fully accountable for what you
submit:

- Every field, path and TS reference must be checked against the YAML or spec yourself. An AI
  statement is not a source. Fabrication is the project's worst failure mode and a PR containing it
  is closed.
- State in the PR description that AI assistance was used and what it was used for.
- Do not add AI tools as commit co-authors (no `Co-Authored-By` trailer for an AI). This matches
  how this repository's own history is kept.

## Licensing and the CLA

The project is [Apache License 2.0](../LICENSE). Contributions will be accepted under a
**Contributor License Agreement** (the maintainer chose a CLA over a DCO; ADR-0467). The CLA text
and the signing bot are **(planned)** and not live yet: **do not open outside PRs until the
maintainer announces that the CLA is in place**, because a contribution cannot be merged without it.

## Security

Report vulnerabilities privately, following [`SECURITY.md`](SECURITY.md), never in a public issue.
Lawful Interception code is sensitive: that area is limited to contributors the maintainer has
approved in advance.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md).

## Questions

Open a [GitHub issue](https://github.com/prajithparan/AI-Boosted-5G-Advanced-Core-and-Charging-R19/issues).
GitHub Discussions is not enabled, so there is no other support channel.
