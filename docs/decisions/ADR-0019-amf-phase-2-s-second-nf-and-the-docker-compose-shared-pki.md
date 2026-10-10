## ADR-0019: AMF (Phase 2's second NF) and the docker-compose shared-PKI fix it required

**Date:** 2026-08-05
**Status:** Accepted

**Context:** AMF implements `Namf_Communication` (`specs/5G_APIs-REL-19/TS29518_Namf_Communication.yaml`)
-- the procedure list agreed with the user before implementation: `ReleaseUEContext`,
`EBIAssignment`, `UEContextTransfer`, `RegistrationStatusUpdate`, `N1N2MessageTransfer`,
`N1N2MessageSubscribe`, `N1N2MessageUnSubscribe`, `NonUeN2MessageTransfer`, `NonUeN2InfoSubscribe`,
`NonUeN2InfoUnSubscribe`, `AMFStatusChangeSubscribe`, `AMFStatusChangeUnSubscribe`,
`AMFStatusChangeSubscribeModfy` -- every operationId with a real `application/json` request-body
alternative in the YAML.

**Deferred, not dropped:** `CreateUEContext`, `RelocateUEContext`, `CancelRelocateUEContext` are
`multipart/related`-ONLY per spec (checked: no `application/json` alternative exists for any of the
three), and `libs/sbi-core` has no multipart/related support. Building that is a substantial
protocol effort in its own right (RFC 2046-style parsing/encoding layered onto the hand-rolled
nghttp2 server), useful to more than just AMF, and was explicitly deferred to a later, dedicated
turn per the user's decision when this was raised. Consequence: nothing in this build can ever
populate `nfs/amf/src/ue_context_store.hpp`'s store, so the four per-`ueContextId` operations
(`ReleaseUEContext`/`EBIAssignment`/`UEContextTransfer`/`RegistrationStatusUpdate`) can only be
exercised on their "no such UE context" (404) branch until `CreateUEContext` lands -- their "found"
branches are implemented for real (correct per spec) but currently unreachable/unverified. Disclosed
in `nfs/amf/src/main.cpp`'s file header, not hidden.

**Also disclosed:** subscriptions (`N1N2Message*`, `NonUeN2Info*`, `AMFStatusChange*`) are created
and removed for real, but notification *delivery* is not implemented -- there is no trigger path
yet (no NGAP/N2, no real UE, no multi-AMF deployment) that would ever fire one.

**NRF client lifecycle runs on a dedicated thread, not the server's `io_context`:** `libs/sbi-core`'s
`http2::Client` is synchronous (ADR-0006, disclosed debt). AMF is the first NF that is
simultaneously a real inbound SBI server (serving `Namf_Communication`) and needs to make ongoing
outbound calls (NRF registration + periodic heartbeat) -- exactly the scenario ADR-0006 flagged as
not viable on the same thread. Resolved minimally: `run_nrf_lifecycle` owns its own `http2::Client`
instance and runs on a dedicated `std::thread`, leaving the server's `io_context` free to serve
inbound requests without stalling during a heartbeat call. This is not the full `curl_multi`+Asio
integration ADR-0006 names as the eventual real fix -- that remains future work once more NFs need
outbound calls from their own request handlers (not just a background lifecycle loop).

**Docker Compose PKI bug found and fixed:** `deploy/docker/docker-compose.yml` previously had each
NF's container generate its own lab PKI independently at startup (`nrf.Dockerfile`'s entrypoint
runs `scripts/gen-lab-pki.sh nrf` fresh every start) -- harmless with only one NF/container in the
compose file (all that existed before AMF), but broken the moment a second container needs to
mutually trust the first over mTLS: two independently-generated root CAs do not validate each
other's leaf certificates. A `pki-init` one-shot service now provisions `certs/` once (via
`scripts/gen-lab-pki.sh nrf amf`) into a shared `certs_data` named volume that both `nrf` and `amf`
mount at `/build/certs`; `nrf`/`amf` both `depends_on: pki-init: condition:
service_completed_successfully`. `nrf.Dockerfile`'s own entrypoint still calls
`gen-lab-pki.sh nrf` too (harmless -- the script skips regeneration when `ca.key`/the NF's cert
already exist, so it's a no-op against the shared volume; kept so the image is still self-sufficient
if ever run standalone via `docker run` outside Compose, matching ADR-0014's original usage).
`amf.Dockerfile`'s entrypoint does NOT attempt its own PKI generation -- it assumes
`/build/certs` is already populated, since running it standalone without `pki-init` having run
first would produce a cert chaining to nobody NRF trusts anyway.

**Disclosed gap, not fixed here:** the equivalent problem exists in Helm (`deploy/helm/amf/` and
`deploy/helm/nrf/` are separate releases with no shared-secret mechanism) -- see
`deploy/helm/amf/Chart.yaml`'s description for the explicit disclosure. A real fix (shared
`Secret` provisioned by a one-shot `Job`, or an external cert-manager `Issuer`) is genuine,
non-trivial scope, deferred to Phase 8 (lab packaging and conformance) per PROMPT.md rather than
attempted piecemeal per-NF-chart in this turn.

**Verification:** `docker build -f deploy/docker/amf.Dockerfile -t 5gc-amf:test .` actually run in
this environment (see `docs/TRACEABILITY.md` for the result) -- matching the bar ADR-0014 set for
NRF's own image. `docker compose up` (both containers actually mTLS-registering with each other)
was NOT run this session, same disclosed-not-silently-assumed gap ADR-0014 already recorded for
NRF alone.

**Ports:** AMF's SBI port (7778) and metrics port (9465) are NRF's (7777/9464) +1 -- a lab
convention chosen for this session, not a value from any spec (`TS29500`/`TS29501` don't mandate
specific ports for SBI services).

---

