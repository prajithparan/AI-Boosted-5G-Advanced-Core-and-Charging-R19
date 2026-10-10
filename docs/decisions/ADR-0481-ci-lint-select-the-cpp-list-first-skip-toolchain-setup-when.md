## ADR-0481: CI lint: select the .cpp list first, skip toolchain setup when it is empty

**Date:** 2026-10-10. **Status:** Accepted (workflow change; NOT yet exercised by a CI run).

**Context.** The lint job always ran vcpkg bootstrap, pip, asn1c and a full CMake Configure before clang-tidy, even when
the file list was empty. ADR-0468 requires pull_request lint to diff against the PR base, so a docs-only push to a PR with
earlier .cpp changes must still lint them; gating clang-tidy on "this push touched a .cpp" would reintroduce that bug.

**Decision.** A new `sel` step computes the identical file list (ADR-0468/0371 logic moved verbatim) and outputs `count`.
Bootstrap vcpkg, pip deps, asn1c, Configure and clang-tidy run only when `count != 0`. Format, README-diagram and
restricted-material checks still always run.

**Disclosed.** Savings apply only when the diff has no .cpp under libs/ nfs/ (e.g. docs-only pushes to main, docs-only PRs);
they do not reduce the cost of a PR that carries .cpp changes. Not measured. YAML parses; the workflow has not run yet.

**Rejected alternatives.** (1) Gate clang-tidy on the push's own changes: reintroduces the ADR-0468 hole.
(2) Cache lint results per file hash: headers change results, unsafe without dependency tracking.
