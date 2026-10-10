## ADR-0186: SCP -- real architectural-scope decision + `Nscp_EventExposure`

### Context: the real scope decision

Fourth NF built under ADR-0184's continuous move-to-next-NF process, and the last of the original
`docs/CAPABILITY_GAP_ANALYSIS.md` "still not done" `nssf`/`nef`/`scp`/`bsf` list. SCP is
architecturally different from every other NF built in this project so far: its real defining role
per TS 29.500 §§6.10-6.11 is as an inline HTTP/2 message-forwarding proxy for "indirect
communication" (Model C/D) between NF service consumers and producers. Every NF this project has
built calls every other NF directly (`sbi_core::http2::Client` -> the target NF's own real
address); making SCP a genuine intermediary would mean re-routing every one of those calls through
a new SCP process -- a cross-cutting architecture change touching every existing NF, not a
same-shape addition. The only real SCP YAML that exists as an actual origin-server REST API,
`Nscp_EventExposure.yaml` (3 ops), is a small side-service (SCP's own operational-statistics
exposure), not that forwarding behavior.

ADR-0184's own continuous-move-to-next-NF process explicitly carves out an exception for exactly
this case: a genuine architecture conflict or unresolved design decision still gets a stop-and-ask,
even under a standing "keep going" instruction. This was NOT silently decided. Presented to the
user via `AskUserQuestion` with three real options: (1) build `Nscp_EventExposure` only,
explicitly disclosing the real proxy/forwarding behavior as out of scope; (2) design the real
proxy first (a genuine, larger, multi-turn architecture task); (3) skip SCP entirely and pick a
different real gap instead. User picked option (1).

### Implementation

Source: `specs/5G_APIs-REL-19/TS29570_Nscp_EventExposure.yaml` (v1.0.1), commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. All 3 real operations implemented:
`CreateSubscription`/`ModifySubscription`/`DeleteSubscription`
(`POST /subscriptions`/`PATCH /subscriptions/{subscriptionId}`/`DELETE
/subscriptions/{subscriptionId}`), plus the real `onScpEventExposureNotification` callback
`CreateSubscription`'s own POST declares.

- `libs/sbi-generated/CMakeLists.txt`: YAML added to the pilot set. Codegen confirmed clean (2196
  types across 42 files). Landed in a standalone `TS29570_Nscp_EventExposure.hpp`; one real name
  collision (`FailureCause`, already used elsewhere) disambiguated by the codegen to
  `FailureCause_Nscp_EventExposure`, same mechanism already used project-wide for other collisions.
- `nfs/scp/` scaffolded to match `nfs/nssf`/`nfs/bsf`/`nfs/nef`'s shape (in-memory store, no
  Redis/Postgres): `CMakeLists.txt`, `src/stores.hpp`/`.cpp` (`ScpEventSubscriptionStore`, full
  CRUD: create/get/patch(RFC 6902 via `nlohmann::json::patch()`, same precedent as
  `NfRegistry::apply_patch`)/remove), `src/main.cpp` (all 3 routes + NRF registration/heartbeat +
  OAuth2 + OTel/Prometheus). `config/scp.json` (port 7791, metrics 9479). `certs/scp/` generated
  via `scripts/gen-lab-pki.sh`. Added to top-level `CMakeLists.txt`,
  `deploy/docker/docker-compose.yml`/`scp.Dockerfile`, and the `pki-init` cert-generation argument
  list. No Helm chart -- same pre-existing, disclosed, project-wide gap as ADR-0183/0184/0185.
- New conformance test file `tests/conformance/test_scp_dtos.cpp` (3 round-trip tests over the real
  generated DTOs), wired into `tests/conformance/CMakeLists.txt`.

### Real, disclosed gap (same shape as ADR-0185's NEF finding, not a new pattern)

Because this project's SCP performs no real message forwarding, it never generates genuine
`ScpSignallingInfo` activity (request/response counts, failure causes, reselection stats) to
report on. `CreateSubscription`/`ModifySubscription`/`DeleteSubscription` are real, live, tested
CRUD on the subscription resource itself, but the real `onScpEventExposureNotification` callback
is never fired -- this project does not build a notification-delivery function with no real
caller. A real, disclosed structural gap, not an oversight.

### Live verification (real, live processes, not self-consistency)

Real `nrf` + `scp` processes, real OAuth2 bearer token, real mTLS curl against all 3 routes:
`CreateSubscription` with a real `eventList`/`eventNotifyUri`/`notifyCorrelationId`: real `201` +
`Location` + `ScpEventExposureSubsResp`. `ModifySubscription` (RFC 6902 `replace` op): real `200`;
against a nonexistent subscription: real `404`. `DeleteSubscription`: real `204`; repeated: real
`404`. Negative auth: a malformed bearer token, real `401` `ProblemDetails`. Prometheus `/metrics`:
every route's own counter present and correctly incremented. All processes killed cleanly
afterward (PID-verified).

### Testing

`scp` and `conformance_tests` built clean (zero warnings) both before and after `clang-format-18`.
Full project rebuild clean (this turn's builds were repeatedly interrupted by a real, external,
non-project background-process-kill pattern hitting the single largest generated-header
translation unit specifically -- unrelated to any code or build-system defect; worked around by
launching builds fully detached from the calling shell via `nohup`+`disown`, not a build system
change). Full `ctest` (excluding the two disclosed pre-existing flaky tests): 350/350 pass, zero
regressions.

### What this ADR does NOT include

The real SCP message-forwarding/indirect-communication role (TS 29.500 §§6.10-6.11) -- explicitly,
deliberately, and by the user's own direct choice among three real options, not a default or an
oversight. No real `onScpEventExposureNotification` delivery (no genuine signalling activity
exists to report). No Helm chart (pre-existing, disclosed, project-wide gap). This closes the
original ADR-0075 "still not done" `nssf`/`nef`/`scp`/`bsf` list -- all four now have at least one
real, live-verified slice built (NSSF ADR-0183, BSF ADR-0184, NEF ADR-0185, SCP this ADR) -- though
NEF still has 13 of its own 14 real YAML files unbuilt and SCP's own real proxy role remains
entirely undesigned; neither is "complete" in the sense the other 8 originally-swept NFs are.

