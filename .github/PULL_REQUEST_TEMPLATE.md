## What does this PR do?

<!-- One or two sentences. Which NF/subsystem, which procedure(s)/resource(s). -->

## Spec source

<!-- Exact YAML file(s) and/or TS document + clause this is generated/derived from.
     Cite a commit/branch if referencing specs/5G_APIs-REL-19/. -->

## Checklist

- [ ] API surface (if any) is generated from the R19 OpenAPI YAML — nothing hand-written that the
      YAML could generate.
- [ ] No invented TS numbers, reference points, API paths, or JSON fields — everything is cited
      against a real spec document I've actually read.
- [ ] This PR touches exactly one area from the table in `.github/CONTRIBUTING.md`: <!-- name it -->
- [ ] Added/updated an ADR created with `python3 scripts/docs/new_adr.py` (not by editing
      `docs/DECISIONS.md`), and `python3 scripts/docs/check_adr_index.py` passes.
- [ ] README and every doc made false by this change are updated in this PR.
- [ ] Updated `docs/TRACEABILITY.md` (procedure → TS clause → source file → test).
- [ ] Added tests derived from the relevant TS call flow; `ctest --test-dir build` passes locally.
- [ ] Ran `clang-format-18` on changed files.
- [ ] Any stub, simplification, or non-conformant shortcut in this PR is called out explicitly
      below (not left for review to discover).
- [ ] New code is C/C++/Python only (or the GUI's established React/JSON Forms exception).
- [ ] I have signed the CLA (once it is announced as live; see CONTRIBUTING.md) and this
      contribution is under this repo's [Apache License 2.0](../LICENSE).
- [ ] If AI assistance was used, I say so below and I checked every spec reference myself.

## Known limitations / stubs / simplifications

<!-- Be explicit. "None" is fine if genuinely true. -->

## How was this tested?

<!-- Unit tests, integration tests, live/manual verification (e.g. curl against a running NF over
     mTLS, docker compose up + live check) — this project prefers live verification over
     self-consistency tests alone where the change touches a running service. -->
