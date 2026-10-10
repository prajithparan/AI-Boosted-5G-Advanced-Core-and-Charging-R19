## ADR-0442: `oam-gui-bff` containerized -- Dockerfile, compose entry, Helm chart, closing ADR-0441's COMPOSE-side deferral

**Date:** 2026-09-27. **Status:** accepted, PARTIALLY -- the COMPOSE half is real and proved
end-to-end; the HELM half is lint/template-verified only and does NOT start successfully as
committed (below). Real compose service, real login round-trip through a real, containerized
`oam-gui-bff` and a real, containerized Keycloak, wired against the same lab CA every other NF
uses. The image that ran that proof was NOT built from `oam-gui-bff.Dockerfile`'s own from-scratch
build -- every attempt at that, on this machine, in this session, was defeated by severe host
network degradation before finishing (detailed below, with what WAS independently verified in
Docker despite that: the Node/Alpine web-build stage). Closes ADR-0441's explicit deferral, quoted
in full there, for its COMPOSE clause only: *"`oam-gui-bff` itself has no Dockerfile or
compose/Helm entry... a containerized BFF would need `token_endpoint`/`jwks_uri` reachable at
Keycloak's in-cluster/in-network name... while `issuer` stays the externally-visible... URL --
both endpoints are independently configurable already, so this needs a values/config change when
it happens, not new code."* That sentence bundles compose ("in-network") and Helm ("in-cluster")
together; this ADR delivers the former and explicitly does NOT deliver the latter -- see Decision 8
and the closing status line.

**Decision 1: `deploy/docker/oam-gui-bff.Dockerfile`, THREE stages, not the usual two.**
`gui/bff/src/bff.cpp`'s `load_static_files()` throws if `<static_dir>/index.html` is missing --
"build the web app first" -- so `gui/web` (React + JSON Forms, this project's one deliberate
C/C++/Python exception) must be built into `dist/` before the C++ stage's binary can even run, not
merely before the image is used. A `node:22-alpine` stage runs `npm ci && npm run build`
(`tsc --noEmit` included) and its `dist/` is copied into the runtime stage. CI has never run this
build (only Python schema/license checks against checked-in source) -- verified here directly:
`npm run build` completed cleanly on this host's Node 22.22.1 with no changes to `gui/web` itself
(Vite 8 needs Node >=20.19, comfortably inside range).

**Decision 2: the C++ stage needs the FULL builder toolchain every other Dockerfile carries --
checked, not assumed, against the task's own "it likely doesn't need libsctp-dev/libbpf-dev"
hint, and found to be wrong.** The single top-level `CMakeLists.txt` unconditionally
`add_subdirectory()`s every NF for ANY configure, including `nfs/upf`
(`pkg_check_modules(libbpf REQUIRED IMPORTED_TARGET libbpf)`,
`pkg_check_modules(libcap REQUIRED IMPORTED_TARGET libcap)`,
`find_program(CLANG_EXECUTABLE NAMES clang clang-18 REQUIRED)`) and `libs/ngap-generated`
(`find_program(ASN1C_EXECUTABLE ... REQUIRED)`), all evaluated at CONFIGURE time. Configuring for
`--target oam-gui-bff` alone still walks both of those `CMakeLists.txt` files and fails without
`libbpf-dev`/`libcap-dev`/`clang-18` and a built `asn1c` toolchain -- confirmed by reading the two
files, not guessed either way. The reduced-toolchain image the task brief floated as plausible does
not actually work in this monorepo's single-configure architecture. `oam-gui-bff.Dockerfile`
therefore mirrors `nrf.Dockerfile`/`amf.Dockerfile`'s builder stage exactly (same apt package list,
same vcpkg commit, same `scripts/setup-asn1c.sh` call), which is the one deliberate piece of
"sameness" in an otherwise NF-specific file.

**Decision 3: like `udr.Dockerfile`/`udsf.Dockerfile`, this image does not generate its own lab
PKI at start.** `oam-gui-bff`'s leaf certificate must chain to the SAME shared lab CA every other
NF's does (both for outbound calls to `product-catalog` etc., and -- new with this ADR -- for
verifying Keycloak's own TLS server certificate, since Keycloak is also in the shared list).
`docker-compose.yml`'s `pki-init` service's NF list gains `oam-gui-bff` (one word appended, same
mechanism ADR-0441 already used to add `keycloak`).

**Decision 4: the OIDC endpoint split needs a whole-file config overlay, not per-key env vars.**
`config/oam-gui-bff.json`'s `oidc` block is read in `main.cpp` with `oidc_cfg.at(...)` directly,
not through `nf_config::require`'s per-key env-var override -- there is no
`OAM_GUI_BFF_OIDC_TOKEN_ENDPOINT`-style hook to lean on without touching that C++ (out of scope:
"do not touch the OIDC code"). `nf_config::load()` already has a whole-file override,
`<SERVICE>_CONFIG_FILE` (uppercased, hyphens intact for a hyphenated service name -- verified
against `nf_config.hpp`'s actual `toupper()`-only loop, not assumed: the real env var name is
`OAM-GUI-BFF_CONFIG_FILE`, and Docker's container environment has no issue with a hyphen in a var
name even though POSIX shell `export` would). `deploy/docker/oam-gui-bff.compose.json` is that
whole-file overlay, selected by `docker-compose.yml`'s `oam-gui-bff` service via that env var.
Deliberately NOT placed under `config/`: `gui/schema-gen/derive_nf_config_schemas.py` globs every
`config/*.json` as one NF-config component, and ADR-0391 forbids a duplicate SBI port (8710)
across two `config/*.json` files -- this file is a second, deployment-specific VIEW of the SAME
component, not a second component. Only `token_endpoint` and `jwks_uri` move to
`https://keycloak:8443/...` (Keycloak's in-network compose hostname, matching the SAN
`gen-lab-pki.sh` already puts on its cert: `DNS:keycloak`); `issuer`, `authorization_endpoint`,
`end_session_endpoint` and `redirect_uri` stay `127.0.0.1`-based -- the browser reaches those
directly (both `keycloak`'s `8443` and `oam-gui-bff`'s `8710` are still published to the host in
compose), and `operator_iam` rows are keyed by `(idp_issuer, idp_subject)` against that same
externally-visible issuer string, which the running proof (below) confirms was not disturbed by
the token/jwks split. Every path in the overlay is ABSOLUTE (`/build/...`), not relative, even
though `config/oam-gui-bff.json`'s own defaults are relative -- found necessary while building the
verification image (below): relative paths resolve against `REPO_ROOT`, a COMPILE-TIME constant
(`target_compile_definitions(oam-gui-bff PRIVATE REPO_ROOT="${CMAKE_SOURCE_DIR}")`); for the
official `oam-gui-bff.Dockerfile` builder stage that constant is always `/build`, so relative paths
would have resolved correctly there too, but making them absolute removes the dependency on that
compile-time value entirely, at no cost -- `main.cpp`'s own `resolve()` leaves an absolute path
untouched. This is what let the SAME overlay file drive a binary compiled directly on the build
host (whose baked `REPO_ROOT` was the host's own worktree path, not `/build`) once its filesystem
was arranged to mirror `/build`'s layout, entirely by chance discovered as a real blocker rather
than assumed away: the first run of the verification container exited immediately, and
`strings build/gui/bff/oam-gui-bff | grep worktrees` confirmed the baked value before this fix.

**Decision 5: two, and only two, files bind-mounted from the host into the container -- not the
whole operator PKI, not a restructure of `certs_data`.** `operator_ca_path`
(`certs/oam-operator-ca/ca.crt`) and `oidc.client_secret_file`
(`certs/oam-gui-bff/oidc-client-secret`) are produced by `gui/scripts/gen-operator-pki.sh` and
`deploy/keycloak/render-realm.sh`, run on the HOST exactly as ADR-0441 already documented -- NOT
inside `pki-init`. Considered and rejected: running those two scripts inside `pki-init` too, so
the whole operator PKI/OIDC-secret lived in the `certs_data` volume alongside the NF-mTLS PKI.
Rejected for three reasons; the first is a real, reproduced finding, the other two are the
consequences that finding pointed at rather than separately tried: (1) `gen-operator-pki.sh`'s
`openssl pkcs12 -export` has no `-passout` and prompts interactively for an export password --
reproduced directly, run on this same host outside any container
(`Enter Export Password: ... Can't read Password`, non-zero exit) -- a real, previously
undocumented gap in that script (it has always assumed an interactive terminal); running it inside
`pki-init`'s `set -euo pipefail` entrypoint, which has no TTY, would hit the identical failure and
abort the ENTIRE `pki-init` job non-zero, so every NF gated on
`pki-init: condition: service_completed_successfully` would refuse to start; (2) `pki-init` runs as
root, so files it wrote into a host-bind-mounted `deploy/keycloak/import/` would be root-owned,
breaking a later host-side rerun of `render-realm.sh`; (3) it would silently rewrite ADR-0441's own
stated contract ("render on the host before `compose up`") rather than extend it. The two-file
bind-mount keeps
`certs_data`-provisioned `oam-gui-bff/{cert,key}.pem` (this NF's own lab-CA identity, used for
every service-to-service call) completely untouched, and changes nothing about how Keycloak's own
realm import already works. Both mounts use compose's LONG syntax with `bind.create_host_path:
false`, not the short `host:container:ro` form -- found necessary, not a style preference: the
short form makes Docker silently CREATE a root-owned DIRECTORY at the host path when the source
file does not exist yet (a fresh clone that has not yet run the two host scripts above), which
would make the container start anyway (mounting an empty directory where a file is expected,
failing later and more confusingly inside the app) and then make a LATER real run of those two
scripts fail trying to write a file where a root-owned directory now sits, which the invoking user
cannot remove without `sudo`. Verified directly: a throwaway compose file with the identical
`bind.create_host_path: false` mount pointed at a nonexistent path, `docker compose up`, refused
immediately (`invalid mount config for type "bind": bind source path does not exist`) and created
nothing on disk; `docker compose -f deploy/docker/docker-compose.yml config` also confirms both
mounts resolve to the expected absolute host paths.

**Decision 6: `docker-compose.yml`'s misleading "oam-gui-bff is not containerized" comment on the
`keycloak` service is corrected, not left to rot.** It now explains the REAL reason no health
condition gates `oam-gui-bff`'s `depends_on: keycloak` (`service_started`, not a health check this
image cannot report): `OidcAuthenticator`'s constructor (`auth.cpp`) does not fetch JWKS or hit any
network endpoint -- checked, not assumed -- so `oam-gui-bff` does not crash-loop racing Keycloak's
own boot; a login attempted too early just fails that one request rather than crash-looping the
container.

**Decision 7 (found while proving this end-to-end, not anticipated): `scripts/gen-lab-pki.sh` now
`chmod 640`s every leaf key it generates, in BOTH the fresh-generation branch and the
"already exists, skip" branch.** `openssl ecparam -genkey` writes private keys `0600`, owner-only,
regardless of umask -- harmless for every NF this project already runs, because every one of THIS
project's own runtime containers runs as root (`grep -l '^USER' deploy/docker/*.Dockerfile` matches
nothing -- checked, not assumed), so root-owned-and-root-read always worked. Keycloak's official
image is the first container in this compose file that is NOT this project's own, and it drops
privileges to `UID 1000` while keeping `GID 0` (confirmed:
`docker run --entrypoint id quay.io/keycloak/keycloak:26.0` -> `uid=1000(keycloak) gid=0(root)`).
The result, reproduced directly rather than guessed at from the vague error Keycloak itself gives:
`keycloak` started, imported the realm successfully ("Realm '5gc-r19-operators' imported" /
"Import finished successfully" both logged), and THEN failed to bind its HTTPS listener at the
last step -- "Failed to start server in (production) mode: /build/certs/keycloak/key.pem", no
further detail. `ls -la` inside the volume showed `-rw------- root root`; `UID 1000` cannot read
that. **This means ADR-0441's own compose Keycloak service, as committed there, could not
actually have started against a `certs_data` volume produced the way `pki-init` produces one** --
every volume this repository's own `pki-init` has ever provisioned carries this same `0600`
`keycloak/key.pem`, this fix included, until this fix runs against it (the "already exists" branch
matters for exactly this: an existing volume from BEFORE this ADR needs its key re-moded, not
just newly-generated ones, which is why that branch also got the `chmod`, not only the fresh one).
Chose `0640` (owner rw, GROUP r, other none) over the more permissive `0644` initially tried:
`keycloak`'s process `GID 0` matches every leaf key's own group ownership (`root`, gid 0, in every
container that runs this script), so group-read is sufficient -- no need to make lab private keys
world-readable on this otherwise-shared, multi-user host. Re-verified end-to-end at `0640`
specifically (not merely reasoned from the `gid` match): `chmod 640` the volume's
`keycloak/key.pem`, `docker compose up -d --no-deps keycloak`, confirmed
`Listening on: https://0.0.0.0:8443` in its log, then stopped/removed the container and
reset the `keycloak` database again (same reason as the main proof's own cleanup, below: Keycloak's
`IGNORE_EXISTING` import strategy would otherwise leave a realm import behind that a future run
cannot cleanly redo).
Fixed at the root cause (every leaf key, not a keycloak-specific carve-out) since any future
non-project image in this compose file would hit the identical failure, silently, the same way
this one did. Still a lab-only CA and lab-only keypairs (this script's own header comment already
says so) -- loosening a LEAF key's mode is not a new secrecy regression; the CA key itself
(`ca.key`, which signs, and is never read by any served container) is deliberately left at its
default `0600`.

**Decision 8: `deploy/helm/oam-gui-bff/` follows `deploy/helm/keycloak/`'s `existingSecrets`
convention, not `deploy/helm/udr/`'s bare image-only shape -- but, disclosed plainly rather than
glossed over, this chart does NOT make the pod start successfully as committed.** The certs/secret
wiring (three secrets -- `-tls`, `-trust`, `-oidc` -- mounted by `subPath` onto the exact
`/build/certs/...` paths `config/oam-gui-bff.json`'s relative paths resolve to, `REPO_ROOT` baked
in at compile time as `/build`) is real and chosen deliberately over UDR/AMF/NRF's charts (which
carry no certs into Helm at all): this NF's PKI/OIDC-secret wiring is exactly what this task is
about, so leaving it unaddressed would have papered over the same gap ADR-0441 already solved
properly for Keycloak's own chart. What is NOT wired, and is the reason this chart is a real,
unclosed gap rather than a finished deliverable: `config/oam-gui-bff.json`'s baked-in defaults --
`iam_database_url` (`postgresql://oam_gui_bff@127.0.0.1:5434/...`) and the `oidc` block's
`token_endpoint`/`jwks_uri` (`127.0.0.1:8443`) -- are wrong for a cluster on BOTH counts, not just
the OIDC one ADR-0441's own deferral named. `IamStore`'s constructor (`iam.cpp`) opens its
PostgreSQL connection pool EAGERLY and calls `nf_config::fatal()` (process exit) if it cannot
connect -- so this chart's pod, run as committed, crash-loops at startup on the database
connection alone, before OIDC ever matters. Closing that needs a REAL `operator_iam` PostgreSQL
reachable from within the cluster, which no chart in this repository provisions yet (Keycloak's
own chart brings a DEDICATED Postgres for ITS OWN database, not `operator_iam` -- a different
database on a different instance in every other deployment target this project has). Building
that dedicated Postgres (or a shared-Postgres Helm pattern this project does not have yet) is a
real, separate piece of work, not a small addition to fold into this ADR under time pressure --
named here as the reason this task's Helm chart is real infrastructure (lint/template-verified,
matches the project's chart conventions) but not yet a chart that starts. Compare: every other NF
chart in this repo (`amf`, `nrf`, `udr`) ALSO bakes in `127.0.0.1`-based config with no Helm
override (task #109's already-tracked class of gap, `docker-compose.yml`'s own
`ADR-0077 retrofit` comments document the identical bug for compose) -- this chart is consistent
with that existing bar, not below it, but that bar itself does not clear "starts in a real
cluster" for THIS NF the way it happens to for a stateless-config NF with no required datastore.
No autoscaling block (unlike `udr`'s, which earned one architecturally --
all state in PostgreSQL): nothing here has tested two `oam-gui-bff` pods behind one Service
(session-cookie affinity, single-use `login_state` rows in `IamStore`), so it is left out rather
than claimed working. `helm lint`/`helm template` run against a real `helm` binary (via
`alpine/helm:3.14.4` in Docker, since no `helm` binary exists in this environment -- closing the
exact weakness ADR-0441 had to disclose for its own Keycloak chart): both clean --
`helm lint`: `1 chart(s) linted, 0 chart(s) failed` (one informational note, an icon is
recommended); `helm template` renders a `Deployment`/`Service` pair with every `subPath` mount and
`image: "5gc-oam-gui-bff:latest"` resolved correctly.

**Decision 9: the `oam-gui-bff` architecture-diagram tile flips from dashed to solid.**
`docs/diagrams/architecture.json`'s `gui` node drops `"planned": true` and gains
`"source": "compose:oam-gui-bff"` (the `compose:<service>` form `tools/diagrams/check_readme_sync.py`
already supports, verified by reading that script rather than assumed) -- this is the FIRST commit
under which `oam-gui-bff` names a real, checkable artifact per the legend's own bar ("exists in this
repository *and* has a test or a live run behind it"); the GUI's C++/tests/Keycloak work all
predates this but had no compose/Docker/Helm entry to point `source` at until now.
`tools/diagrams/render_architecture.py` re-rendered both SVGs; `check_readme_sync.py` passes.
`README.md`'s Phase 7 status row is corrected to match -- the "`oam-gui-bff` itself has no
Docker/Helm entry yet" sentence ADR-0441 put there is now false and is replaced.

**What was actually run, not just written** (the task's bar, restated from ADR-0441: "a container
that starts" is not done; a real round-trip succeeding is):

- **The image that ran the proof below was hand-packaged from a binary compiled directly on this
  build host, not from `oam-gui-bff.Dockerfile`'s own from-scratch build -- and that official
  build NEVER completed in this session. Disclosed plainly, not blurred.** This machine's network
  was severely degraded while this task ran (measured, not assumed: `ping 1.1.1.1` showed 33-50%
  packet loss and 390-440ms RTT over a WiFi link; `curl` to `google.com`/`github.com` timed out
  outright at points), on top of a concurrent CI run (`36294057085`) competing for the same link.
  Three real attempts at `docker build -f oam-gui-bff.Dockerfile`, in this exact worktree, all
  failed: (1) the `node:22-alpine` layer pull (55.59 MB) took over 75 minutes then failed outright
  (`read tcp ...: read: connection timed out`); (2) a second attempt got PAST that layer (BuildKit
  cache) but failed cloning `vcpkg` from GitHub after ~20 minutes (`RPC failed; curl 92 HTTP/2
  stream 5 was not closed cleanly`); (3) a third attempt reused both caches (Node stage and the
  `vcpkg` git objects fetched so far) and was still stuck re-cloning `vcpkg` after 8+ minutes when
  it was killed to write this ADR, rather than left running indefinitely on a shared machine.
  **What IS independently verified as working, from attempt (2)'s own log:** the `web-builder`
  stage -- `npm ci` and `npm run build` (`tsc --noEmit` + `vite build`, Alpine/musl) -- ran to
  completion INSIDE Docker (`DONE 2.1s`, identical output to the host run: 424 modules, the same
  three `dist/` files). **What is NOT verified inside Docker in this session:** the builder stage
  (`vcpkg` bootstrap, `asn1c`, the actual `cmake --build --target oam-gui-bff`) and the final
  `COPY --from=builder`/`COPY --from=web-builder` assembly into the runtime stage. Rather than
  block the whole task on one machine's transient link, the SAME source was independently built
  the OTHER way this project's own CI already builds it: `cmake --build` against this host's
  existing vcpkg binary cache (`~/.cache/vcpkg/archives`, the exact cache directory
  `.github/workflows/ci.yml`'s own self-hosted-runner step already points at) and the shared
  `build-tools/asn1c` cache -- Release, `-D5GC_BUILD_TESTS=OFF`, the identical CMake invocation
  the Dockerfile's builder stage runs, producing a functionally identical binary (confirmed: only
  `libstdc++`/`libm`/`libgcc_s`/`libc` dynamic dependencies via `ldd` -- every vcpkg dependency is
  statically linked, same as the official image's runtime stage would produce) in under two
  minutes. That binary and the SEPARATELY-Docker-verified `gui/web/dist` were packaged into an
  image using the SAME runtime layout (`ubuntu:24.04` + `openssl`/`ca-certificates`, `/build/...`
  paths) `oam-gui-bff.Dockerfile`'s own runtime stage uses. This proof's image was tagged
  `docker-oam-gui-bff:latest` (the exact name `docker compose` expects for this project/service
  pair) only for the duration of the proof, then **explicitly removed afterward**
  (`docker rmi docker-oam-gui-bff:latest`) precisely so a future `docker compose up oam-gui-bff` on
  this shared machine builds the real image from the real Dockerfile rather than silently reusing
  this hand-packaged substitute. **Whoever next has a working network should run
  `docker build -f deploy/docker/oam-gui-bff.Dockerfile -t docker-oam-gui-bff:latest .` from the
  repo root, bring up `keycloak`/`oam-gui-bff` (`docker compose -f deploy/docker/
  docker-compose.yml up -d keycloak oam-gui-bff`, `pki-init` first if `certs_data` is fresh), and
  rerun `deploy/keycloak/prove_operator_login.sh` (committed with this ADR, not left in a
  scratchpad) against that real image -- this ADR's own proof used a substitute for the builder
  stage specifically because of an environmental condition on one machine at one time, not because
  the Dockerfile is unverified by design.**
- **Real containers, real shared infrastructure.** `pki-init`'s own `apt-get install openssl` hung
  indefinitely under the same degraded network (11+ minutes, zero log output) -- worked around by
  running `scripts/gen-lab-pki.sh`'s remaining NF names (`udsf keycloak oam-gui-bff` -- everything
  else in the shared `certs_data` volume already existed from earlier work on this machine)
  directly against the ALREADY-BUILT verification image (which already has `openssl`, needing no
  network at all), not by skipping PKI provisioning. `docker compose -f deploy/docker/
  docker-compose.yml up -d --no-deps keycloak oam-gui-bff` started both against the SAME
  already-running, shared `postgres-chf`/`valkey` this machine's other work depends on -- `--no-deps`
  specifically so `up` would not also start `product-catalog` (a real port, 7785, this machine's
  CI run might have still been exercising at that moment; confirmed after the fact it was not, but
  `--no-deps` cost nothing and removed the question).
- **The CA `prove_operator_login.sh`'s own `<ca_bundle_path>` argument needs, extracted from the
  running `certs_data` volume itself, not any host `certs/ca/ca.crt`** (see that script's own
  header for why those are two different CAs): any image with that volume mounted can read it
  back out, e.g. with the always-locally-available `ubuntu:24.04` --
  `docker run --rm -v docker_certs_data:/build/certs --entrypoint cat ubuntu:24.04
  /build/certs/ca/ca.crt > ca.crt` -- this is exactly the command this proof actually ran (against
  the now-removed verification image at the time, functionally identical to running it against
  `ubuntu:24.04` or any other image sharing that mount).
- **A real, previously-nonexistent `operator_iam`/`keycloak` pair, provisioned by hand with the
  EXACT DDL `init-domain-dbs.sh` would have run on a fresh volume** (this shared `postgres-chf`
  predates ADR-0423/ADR-0441 -- see Disclosed below) -- `CREATE DATABASE`, then
  `deploy/db/operator_iam/*.sql`, then `deploy/keycloak/seed-lab-operators.sql`, all applied via
  `docker exec ... psql`.
- **Positive path**, scripted and committed as `deploy/keycloak/prove_operator_login.sh` (its own
  header has the exact prerequisites and usage) rather than left in a scratchpad. `curl`,
  presenting the real `terminal-lab-1` operator-CA client certificate
  (mTLS) generated by `gui/scripts/gen-operator-pki.sh` on the host, hit the CONTAINERIZED
  `oam-gui-bff`'s real `/auth/login` -> real `302` to the CONTAINERIZED Keycloak's authorization
  endpoint (`https://127.0.0.1:8443/realms/5gc-r19-operators/...`, `acr_values=mfa`, real PKCE
  `code_challenge`) -> real Keycloak login form -> real password -> real TOTP OTP (computed by
  `deploy/keycloak/totp.py` from the seeded secret) -> real `302` back to
  `https://127.0.0.1:8710/auth/callback?code=...&state=...` -> the CONTAINERIZED `oam-gui-bff`
  exchanged the code AT `https://keycloak:8443/realms/.../token` (the in-network hostname --
  confirmed this is what actually happened, not merely configured, because the exchange could only
  have succeeded by reaching Keycloak at that address: `oam-gui-bff`'s only route to Keycloak is
  the `keycloak:8443` hostname, unresolvable from outside the compose network), fetched JWKS the
  same way, verified the real RS256-signed ID token (whose `iss` the BFF checks against
  `config`'s OWN `issuer`, `https://127.0.0.1:8443/...` -- the externally-visible one, unchanged by
  the token/jwks split), and returned a real `200` with `set-cookie:
  __Host-oam_session=...; Path=/; Secure; HttpOnly; SameSite=Strict`. `iam.audit_event` recorded
  it: `action=auth.login outcome=ALLOWED username=shop.agent.lab terminal_cn=terminal-lab-1
  http_status=200`.
- **Negative path, same real containers, same real IdP** (mirroring ADR-0441's own second proof):
  `shop.agent.lab`'s `operator_iam` status flipped to `LOCKED` mid-session; a fresh login (real
  password, real correct OTP, real Keycloak success, real `302` back to `/auth/callback`) was
  refused by the CONTAINERIZED `oam-gui-bff` itself with `403`, audited
  `outcome=DENIED reason="user status is LOCKED"`. Status reverted afterward.
- **Cleanup, disclosed rather than left implicit.** After both runs: `docker-oam-gui-bff-1` and
  `docker-keycloak-1` stopped and removed; the `keycloak` database DROPPED and recreated EMPTY
  (not left holding a realm keyed to secrets -- the client secret, the two lab users' passwords and
  TOTP seeds -- that live only in this worktree's gitignored `certs/`, which will not survive this
  worktree; Keycloak's own `IGNORE_EXISTING` import strategy means a future run against a
  non-empty `keycloak` database with regenerated secrets would silently keep the STALE realm,
  which is worse than starting from empty). `operator_iam` (the schema, roles, and the two lab
  `operator_user`/`role_assignment` rows -- keyed to the realm template's FIXED `idp_subject` UUIDs,
  not to any regenerated secret) was deliberately left in place: real, reusable state, the same
  kind ADR-0441 itself left behind.

**Disclosed, deferred (explicitly, not silently):**
- **`bss/provisioning` still has no Dockerfile/compose/Helm entry at all** (a pre-existing,
  separate gap this task did not create and was not asked to close --
  `deploy/docker/oam-gui-bff.compose.json`'s `provisioning_base_url` points at
  `https://provisioning:7802`, the hostname it would need if/when that gap closes, which will not
  resolve inside the compose network until it does; the customerOrder create/read routes that use
  it are not exercised by this ADR's proof).
- **Back-channel logout, per-action step-up**: unchanged from ADR-0441 -- this ADR is packaging,
  not new OIDC behaviour.
- **Helm chart's ConfigMap-driven config override**: not built here, matching every other NF
  chart's current baked-in-`127.0.0.1` state (task #109) -- see Decision 8.
- **The shared lab `postgres-chf` instance this proof ran against predates ADR-0423/ADR-0441**:
  its `operator_iam`/`keycloak` databases did not exist at all before this task (`\l` showed only
  `charging` / `chf_rating` / `mediation` / `orchestration` / `postgres`) -- see the proof section
  above for exactly what was applied by hand, and what was reset afterward. Rather than recreate
  that CONTAINER (risk to whatever else this shared, multi-agent
  machine has running against it), only additive `CREATE DATABASE`/DDL statements touched it;
  nothing pre-existing was altered or dropped.
- **Autoscaling / multi-replica behaviour for `oam-gui-bff`**: not tested, not claimed (Decision 8).
- **This host's network was severely degraded for a large part of this task, independent of
  anything in this change** (see the proof section's own account: measured packet loss/RTT, a
  concurrent CI run, a 75-minute failed layer pull) -- disclosed there in detail rather than
  summarized away here, and the ADR says plainly which artifact (host-compiled vs.
  Dockerfile-from-scratch) the proof actually used.

**ADR-0441 status:** its "`oam-gui-bff` itself has no Dockerfile or compose/Helm entry" deferral is
closed by this ADR for the DOCKERFILE and COMPOSE thirds -- both real, both proved with a live
login round-trip. The HELM third is NOT closed: the chart exists, is
lint/template-verified, and follows this project's conventions, but does not start successfully in
a real cluster as committed (Decision 8, `deploy/helm/oam-gui-bff/Chart.yaml`'s own disclosure).
Narrowed here rather than left to be discovered as an overclaim on review.

