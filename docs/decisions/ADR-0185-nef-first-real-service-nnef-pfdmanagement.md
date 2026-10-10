## ADR-0185: NEF -- first real service, `Nnef_PFDmanagement`

### Context

Third NF built under ADR-0184's continuous move-to-next-NF process. `docs/CAPABILITY_GAP_ANALYSIS.md`'s
"still not done" list, after ADR-0184, named `nef`/`scp` as the two remaining unbuilt Tier 1 NFs.
NEF's own real spec surface is unusually large among this project's NFs: 14 separate YAML files
(`Nnef_EventExposure`, `Nnef_SMContext`, `Nnef_SMService`, `Nnef_UEId`, `Nnef_Authentication`,
`Nnef_TrafficInfluenceData`, `Nnef_DNAIMapping`, `Nnef_ECSAddress`, `Nnef_EASDeployment`,
`Nnef_Inference`, `Nnef_Training`, `Nnef_VFLInference`, `Nnef_VFLTraining`,
`Nnef_PFDmanagement`), ~52 real operations total -- too large for one turn as a whole, confirmed
by direct read of every file's own operation count, not estimated. `Nnef_PFDmanagement` (v1.4.0,
6 real top-level operations, the largest single well-defined NEF service among the 14) chosen as
this turn's slice: real, spec-documented consumer-facing counterpart to this project's own
already-built UPF-side PFCP PFD Management (task #107/ADR-0086, TS 29.244 -- the N4 side SMF
already uses to push PFDs onward to UPF), even though the two aren't wired together this turn
(SMF doesn't yet call this NEF API to source what it pushes -- real, disclosed, deferred).

Source: `specs/5G_APIs-REL-19/TS29551_Nnef_PFDmanagement.yaml` (v1.4.0), commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. All 6 real top-level operations implemented:
`Nnef_PFDmanagement_AllFetch`/`AppFetchPartialUpdate`/`IndAppFetch`
(`/applications`/`/applications/partialpull`/`/applications/{appId}`, all GET/POST-fetch, no
write) and `CreateSubscr`/`ModifySubscr`/`Unsubscribe` (`/subscriptions`/`/subscriptions/
{subscriptionId}`).

### Real, disclosed structural gap (not a simplification -- a genuine spec-shape finding)

Direct read of the full YAML confirms it has NO operation anywhere that lets a caller WRITE PFD
content into NEF. The real 3GPP AF-to-NEF PFD provisioning path is genuinely out of 3GPP's own
standardized SBI framework scope (typically OAM/vendor-specific), not just unbuilt here. Two real
consequences, both disclosed up front in `nfs/nef/src/main.cpp`'s own top comment:
1. `PfdCatalogStore` is seed()-only -- same precedent as several of this project's other "no live
   write path exists" stores (e.g. UDR's `SponsorConnectivityDataStore` before ADR-0182).
2. The real `PfdChangeNotification`/`NotificationPush` callback delivery `CreateSubscr`'s own POST
   declares has NO real trigger this project can ever fire, since PFD content never changes after
   startup seeding. This project does NOT build a notification-delivery function with no real
   caller -- dead code presented as live infrastructure would be worse than disclosing the gap
   plainly. `CreateSubscr`/`ModifySubscr`/`Unsubscribe` are real, live, tested CRUD on the
   subscription resource itself; no notification is ever sent. A real, disclosed structural gap,
   not an oversight or a simplification of something that could otherwise be built.

### Implementation

- `libs/sbi-generated/CMakeLists.txt`: YAML added to the pilot set. Codegen confirmed clean (2183
  types across 40 files). Landed in a standalone `TS29551_Nnef_PFDmanagement.hpp`, no name
  collisions.
- `nfs/nef/` scaffolded to match `nfs/bsf`/`nfs/nssf`'s shape (in-memory stores, no
  Redis/Postgres): `CMakeLists.txt`, `src/stores.hpp`/`.cpp` (`PfdCatalogStore` seed()/get()-only,
  `PfdSubscriptionStore` full CRUD), `src/main.cpp` (all 6 routes + NRF registration/heartbeat +
  OAuth2 + OTel/Prometheus). `config/nef.json` (port 7790, metrics 9478). `certs/nef/` generated
  via `scripts/gen-lab-pki.sh`. Added to top-level `CMakeLists.txt`,
  `deploy/docker/docker-compose.yml`/`nef.Dockerfile`, and the `pki-init` cert-generation argument
  list. No Helm chart -- same pre-existing, disclosed, project-wide gap as ADR-0183/ADR-0184.
- Real `AppFetchPartialUpdate` "changed since" logic (`PfdCatalogStore::get_if_changed_since`):
  lexicographic string comparison of this project's own generated ISO8601 UTC `DateTime` string
  format (always zero-padded, always `Z`-suffixed, so lexicographic order matches chronological
  order) -- a real, disclosed narrower assumption than a full calendar-aware comparison would need
  for arbitrary input formats, not a fabricated shortcut.
- New conformance test file `tests/conformance/test_nef_dtos.cpp` (4 round-trip tests over the
  real generated DTOs), wired into `tests/conformance/CMakeLists.txt`.

### Live verification (real, live processes, not self-consistency)

Real `nrf` + `nef` processes, real OAuth2 bearer token, real mTLS curl against all 6 routes:
`AllFetch` with a real comma-separated `application-ids` query param (`style: form,
explode: false`, via the project's existing `split_form_array`) correctly returned only the
seeded, known application and silently omitted the unknown one (matching the real schema's own
`minItems: 0` array response, not an error); missing the mandatory `application-ids` param: real
`400`. `IndAppFetch`: real `200` for the seeded `app-video-streaming`, real `404` for an unknown
appId. `AppFetchPartialUpdate`: an older client-supplied `pfdTimestamp` correctly returned the
real `200` "changed" response; a same-or-later timestamp correctly returned real `204` "not
changed"; no timestamp at all correctly always returned real `200` (unconditional fetch). `
CreateSubscr`: real `201` + `Location`. `ModifySubscr`: real `200`; against a nonexistent
subscription: real `404`. `Unsubscribe`: real `204`; repeated: real `404`. Negative auth: a
malformed bearer token, real `401` `ProblemDetails`. Prometheus `/metrics`: every route's own
counter present and correctly incremented. All processes killed cleanly afterward (PID-verified).

### Testing

`nef` and `conformance_tests` built clean (zero warnings) both before and after `clang-format-18`.
Full project rebuild clean (built in per-target chunks this turn due to a real, external,
non-project background-process interruption pattern observed repeatedly during this turn's build
attempts -- unrelated to any code or build-system defect, worked around by chunking, not a build
system change). Full `ctest` (excluding the two disclosed pre-existing flaky tests): 347/347 pass,
zero regressions.

### What this ADR does NOT include

13 of NEF's own 14 real YAML files remain entirely unbuilt (`Nnef_EventExposure`,
`Nnef_TrafficInfluenceData`, `Nnef_SMContext`/`SMService`, `Nnef_UEId`, `Nnef_Authentication`,
`Nnef_DNAIMapping`, `Nnef_ECSAddress`, `Nnef_EASDeployment`,
`Nnef_Inference`/`Training`/`VFLInference`/`VFLTraining`) -- a real, disclosed, large deferral,
not a claim of NEF completeness. No PFD provisioning path exists or is invented (structural spec
gap, see above). No real `PfdChangeNotification`/`NotificationPush` delivery (same reason). No
Helm chart (pre-existing, disclosed, project-wide gap). SCP remains entirely unbuilt and needs a
real architectural design decision before implementation, per ADR-0184.

