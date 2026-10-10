## ADR-0014: NRF Docker image -- Ubuntu 24.04 multi-stage, PKI generated at container start

**Date:** 2026-08-05
**Status:** Accepted

**Context:** No NF has been containerized before this (Phase 0's binaries were throwaways, never
built a Dockerfile). `nfs/nrf/CMakeLists.txt` bakes `CERTS_DIR` in as a compile-time absolute path
(`${CMAKE_SOURCE_DIR}/certs`, an already-disclosed Phase 0/ADR-0011 simplification) -- this has a
direct, concrete consequence for how the image has to be built.

**Decision:** `deploy/docker/nrf.Dockerfile`, multi-stage: `builder` (Ubuntu 24.04, full build
toolchain + vcpkg + codegen Python deps, builds the `nrf` target only -- `-D5GC_BUILD_TESTS=OFF`),
`runtime` (Ubuntu 24.04 + `openssl`/`ca-certificates` only, copies the built binary). Both stages
use `/build` as `WORKDIR` specifically so the compile-time `CERTS_DIR` path resolves at runtime
too. The entrypoint runs `scripts/gen-lab-pki.sh nrf` fresh on every container start (ephemeral
CA/certs per container instance by default -- consistent with this being a lab image, not a
provisioned deployment).

**Real build issues hit and fixed, not just anticipated:**
- Docker build context initially copied the host's `build/` directory (stale `CMakeCache.txt`
  pointing at the host's absolute path) -- CMake correctly refused to reuse it inside the
  container. Fixed with `.dockerignore` excluding `build/`, `build-*/`, `certs/`, `.git/`, etc.
- `git clone --depth 1` for vcpkg inside the Dockerfile failed vcpkg's own baseline resolution:
  `vcpkg.json`'s `builtin-baseline` pins a specific historical commit, but a shallow clone only
  fetches the current tip of the default branch, which does not contain that commit object. Fixed
  with a full clone + explicit `git checkout <pinned-commit>`.

**Verification:** actually built (`docker build -f deploy/docker/nrf.Dockerfile -t 5gc-nrf:test .`)
and ran in this environment, not just written-but-unverified Dockerfile text -- see
`docs/TRACEABILITY.md` for the concrete command/output this ADR's claims are backed by.

**Rejected alternative:** a distroless or Alpine runtime base for a smaller image. Rejected for
now on time/complexity grounds (musl vs. glibc ABI concerns with the vcpkg-built static libs
weren't worth resolving for a first Docker pass); worth revisiting once image size actually
matters for the lab.

**Disclosed gap:** no non-root `USER` directive yet -- the container currently runs as root. Real
gap against container-security best practice, not fixed in this pass; flagged rather than silently
shipped as if it were fine.

---

