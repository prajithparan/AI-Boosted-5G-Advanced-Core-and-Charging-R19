# Contributing

Thanks for your interest in this project. It's a standards-faithful 5G Core (5GC) implementation,
and its usefulness depends entirely on staying spec-accurate — please read this before opening a
PR.

## Ground rules (non-negotiable)

These come directly from [`CLAUDE.md`](../CLAUDE.md), which governs every change in this repo:

- **3GPP OpenAPI YAML (REL-19, vendored under [`specs/`](../specs/)) is the only source of truth**
  for API shapes, paths, schemas, and enums. Never hand-write a DTO the YAML can generate.
- **Never invent a TS number, reference point, API path, or JSON field.** If the spec material you
  need isn't available in the repo, open an issue instead of guessing — a fabricated field costs
  far more reviewer time than a question does.
- **C/C++/Python only for new code.** The only exception is the operator GUI, which uses
  React + JSON Forms.
- **No raw `new`/`delete`**, RAII everywhere, `std::expected`/`tl::expected` for recoverable
  errors, exceptions only at API boundaries.
- **State what's a stub, a simplification, or non-conformant.** If your change includes any of
  these, say so explicitly in the PR description — don't let it be discovered in review.

## Before you start

- For anything beyond a small fix, open an issue first describing the change (see the issue
  templates) so scope and spec references can be confirmed before you invest time.
- This project moves in small, reviewable increments — one NF or one subsystem per PR, matching
  the pattern in [`docs/DECISIONS.md`](../docs/DECISIONS.md)'s ~100 existing ADR entries. Large,
  multi-subsystem PRs are hard to review against the spec and are likely to be asked to split.

## Building and testing locally

```sh
git clone https://github.com/microsoft/vcpkg.git ~/vcpkg
~/vcpkg/bootstrap-vcpkg.sh -disableMetrics

cmake -S . -B build -G Ninja \
  -DCMAKE_TOOLCHAIN_FILE=$HOME/vcpkg/scripts/buildsystems/vcpkg.cmake \
  -DCMAKE_BUILD_TYPE=Debug
cmake --build build
ctest --test-dir build --output-on-failure
```

See the main [`README.md`](../README.md#building) for the full build instructions, including
sanitizer builds (`-D5GC_ENABLE_ASAN=ON` / `-D5GC_ENABLE_TSAN=ON`). CI
([`.github/workflows/ci.yml`](workflows/ci.yml)) runs four jobs on every push/PR: `build`,
`sanitize (tsan)`, `sanitize (asan-ubsan)`, and `lint` (`clang-format` + `clang-tidy`). A PR should
pass all four before it's ready for review.

Run `clang-format` on any files you touch before committing:

```sh
clang-format-18 -i <changed files>
```

## What a PR should include

- The generated/hand-written code itself, citing the exact YAML file (and section/clause of the
  relevant TS document, if not YAML-derived) it's sourced from, in a header comment — matching the
  style already used throughout the codebase.
- Tests: every procedure needs a test derived from the TS 23.502 (or relevant TS) call flow it
  implements. Live/integration verification (real process, real database, real mTLS handshake) is
  preferred over pure unit tests where the change touches a running service — see any recent ADR
  in `docs/DECISIONS.md` for the established bar.
- An entry in [`docs/DECISIONS.md`](../docs/DECISIONS.md) (ADR format: Context / Implementation /
  Testing and verification / What this does NOT include) for anything beyond a trivial fix.
- An update to [`docs/TRACEABILITY.md`](../docs/TRACEABILITY.md) mapping procedure → TS clause →
  source file → test.
- See [`.github/PULL_REQUEST_TEMPLATE.md`](PULL_REQUEST_TEMPLATE.md) for the exact checklist.

## Licensing

This project is licensed under [Apache License 2.0](../LICENSE). By submitting a pull request, you
agree that your contribution is licensed under the same terms.

## Code of Conduct

This project follows the [Contributor Covenant](CODE_OF_CONDUCT.md). Please read it before
participating.

## Questions

Open a [GitHub issue](https://github.com/prajithparan/AI-Boosted-5G-Advanced-Core-and-Charging-R19/issues) —
there's no other support channel for this project (GitHub Discussions is not enabled on the repository).
