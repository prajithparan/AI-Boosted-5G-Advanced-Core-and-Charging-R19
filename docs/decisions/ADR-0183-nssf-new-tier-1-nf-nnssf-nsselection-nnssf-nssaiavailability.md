## ADR-0183: NSSF -- new Tier 1 NF, `Nnssf_NSSelection` + `Nnssf_NSSAIAvailability`

### Context

Task #106's UDR resource-breadth gap-closure backlog is exhausted (ADR-0182 closed the last
tractable item). User explicit direction: move to a different NF/subsystem rather than keep
narrowing UDR further. `docs/CAPABILITY_GAP_ANALYSIS.md`'s own "still not done" list names
`nssf`/`nef`/`scp`/`bsf` as entirely unbuilt Tier 1 NFs -- the most literal reading of "a different
subsystem," since nothing existed for any of them before this turn. NSSF chosen over the other
three via `AskUserQuestion` alongside two narrower, existing-NF gap candidates (AMF<->SMF N2
handover depth, PCF `Npcf_UEPolicyControl`); user picked NSSF.

Source: `specs/5G_APIs-REL-19/TS29531_Nnssf_NSSelection.yaml` (v2.4.0) and
`specs/5G_APIs-REL-19/TS29531_Nnssf_NSSAIAvailability.yaml` (v1.4.0), commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. Both files were already present in `specs/` but not yet
wired into the sbi-codegen pilot set. Procedure list shown and approved before implementation
(including the user's explicit choice to include `NSSAIAvailabilityOptions`, a bare capability-
discovery endpoint this project has otherwise avoided building for other NFs): all 8 real
operations across both services --
`NSSelectionGet`/`NSSAIAvailabilityPut`/`Patch`/`Delete`/`Post`/`Unsubscribe`/`SubModifyPatch`/
`Options`, plus the real `nssaiAvailabilityNotification` callback POST's own subscription creation
declares (`{$request.body#/nfNssaiAvailabilityUri}`).

### Implementation

- `libs/sbi-generated/CMakeLists.txt`: both YAML files added to `SBI_CODEGEN_PILOT_FILES`/
  `_PATHS`. Codegen confirmed clean (2155 types across 36 files, up from the prior pilot set);
  every NSSF-specific type (`AuthorizedNetworkSliceInfo`, `SliceInfoForRegistration`,
  `NssaiAvailabilityInfo`, `NssfEventSubscriptionCreateData`, etc.) landed in the existing merged
  `TS29122_CommonData_grp.hpp` with no name collisions against the ~2000 types already there.
- `nfs/nssf/` scaffolded to match `nfs/pcf`'s shape exactly (in-memory stores, no Redis/Postgres --
  neither YAML has a resource whose scope this turn needs persistence for): `CMakeLists.txt`,
  `src/stores.hpp`/`.cpp` (`NssaiAvailabilityStore` keyed by `nfId`,
  `NssaiAvailabilitySubscriptionStore` keyed by a generated `subscriptionId`, both plain-JSON-value
  stores per the same precedent `nfs/pcf/src/stores.hpp`'s own header comment documents), `src/
  main.cpp` (all 8 routes + NRF registration/heartbeat + OAuth2 + OTel/Prometheus, same skeleton as
  every other NF). `config/nssf.json` (port 7785, metrics 9473 -- the two lowest unused values in
  the existing per-service port/metrics ranges). `certs/nssf/` generated via the existing
  `scripts/gen-lab-pki.sh`. Added to top-level `CMakeLists.txt`'s `add_subdirectory` list and
  `deploy/docker/docker-compose.yml`/`nssf.Dockerfile` (mirroring `pcf.Dockerfile`) and the
  `pki-init` service's cert-generation argument list. No Helm chart: matches the pre-existing state
  of udm/udr/ausf/pcf/upf/chf, which also don't have one yet -- a real, disclosed, project-wide gap
  this NF doesn't newly introduce, not something silently skipped only for NSSF.
- New conformance test file `tests/conformance/test_nssf_dtos.cpp` (6 round-trip tests over the
  real generated DTOs, same construct/to_json/from_json/compare pattern as `test_round_trip.cpp`),
  wired into `tests/conformance/CMakeLists.txt`.

### Real, disclosed simplifications (also documented at the top of `nfs/nssf/src/main.cpp`)

1. `NSSelectionGet`'s real slice-selection DECISION (subscriber entitlement via UDM, network-slice-
   instance load/NRF discovery, NSAG-to-TA mapping) is out of scope. This implementation seeds a
   fixed "network-supported NSSAI" catalog at startup (`seed_catalog()`: SST 1 = eMBB, SST 2 =
   URLLC, both real TS 23.501 Table 5.15.2.2-1 standardized values, not fabricated) and splits
   whatever S-NSSAI list the caller asks about into allowed/rejected by catalog membership. Neither
   YAML has a write route for this catalog at all -- a real deployment's NSSF gets it from local
   configuration, which this project models as a startup seed, same precedent as several of UDR's
   own `seed()`-only stores.
2. `AllowedNssai.accessType` is REQUIRED in the response schema, but direct read of
   `TS29531_Nnssf_NSSelection.yaml` confirms `NSSelectionGet`'s own query parameters never actually
   convey which access type the caller means. This project always fills `"3GPP_ACCESS"` -- a real,
   disclosed default, not an invented field.
3. `NSSAIAvailabilityPut`/`Patch`'s own "authorization" of submitted per-TA S-NSSAI data echoes
   every submitted `SupportedNssaiAvailabilityData` back as authorized unchanged -- no real
   restriction/rejection logic (`restrictedSnssaiList`) is modeled, since this project has no
   roaming-partner/PLMN-restriction data source to check against yet.
4. `nssaiAvailabilityNotification` delivery only fires from `Put`/`Patch` (a real change to what's
   authorized). `Delete` does NOT trigger it: `NssfEventNotification`'s own real fields
   (`unavailableNsiList`/`altNssai`) are NSI/S-NSSAI-replacement shaped, not "an NF instance
   deregistered" -- a real, disclosed gap rather than an invented mapping onto fields that don't
   mean that.
   Delivery itself is synchronous, best-effort, non-blocking-to-the-caller -- reuses the same
   precedent as `nfs/udr`'s own `onDataChange` webhook delivery (ADR-0171).

### Live verification (real, live processes, not self-consistency)

Real `nrf` + `nssf` processes started, real OAuth2 bearer token fetched via `POST /oauth2/token`,
real mTLS curl against every route:
- `NSSelectionGet` with `slice-info-request-for-registration={"requestedNssai":[{"sst":1},
  {"sst":99}]}`: real `200`, `sst:1` correctly landed in `allowedNssaiList` (`accessType:
  3GPP_ACCESS`), `sst:99` correctly landed in `rejectedNssaiInPlmn`. No slice-info param at all:
  real `200`, falls back to the whole seeded catalog. Missing `nf-id`: real `400`.
- `NSSAIAvailabilityPut` with a real `SupportedNssaiAvailabilityData` (TAI 310-410-000001, SST 1):
  real `200` with the echoed `AuthorizedNssaiAvailabilityInfo`.
- `NSSAIAvailabilityPost` (subscribe, `nfNssaiAvailabilityUri` pointed at a real local HTTPS
  receiver stood up for this test, presenting a lab-CA-signed cert via a new `certs/callback-test/`
  identity): real `201` + `Location` header + generated `subscriptionId`.
- Real `nssaiAvailabilityNotification` delivery confirmed end-to-end: the PUT above triggered a
  real mTLS POST to the subscriber's `nfNssaiAvailabilityUri`, received and captured by the test
  receiver, containing the correct `subscriptionId` and the matching
  `authorizedNssaiAvailabilityData`.
- `NSSAIAvailabilityPatch` (RFC 6902 `replace` on the seeded SST): real `200`, echoed value
  updated. Patch against a nonexistent `nfId`: real `404`.
- `NSSAIAvailabilityDelete`: real `204`; repeated delete: real `404`.
- Subscription `NSSAIAvailabilitySubModifyPatch`: real `200`. `NSSAIAvailabilityUnsubscribe`: real
  `204`; repeated: real `404`.
- `NSSAIAvailabilityOptions`: real `200`.
- Negative auth: a malformed bearer token: real `401` `ProblemDetails`.
- Prometheus `/metrics`: every route's own counter present and correctly incremented.
All processes killed cleanly afterward (PID-verified), including a stray, days-old manually-started
`udm` process found squatting port 7780 from an earlier session -- killed by explicit PID before
`ctest` ran, per this project's own established practice.

### Testing

`nssf` and `conformance_tests` built clean (zero warnings) both before and after `clang-format-18`
(`find nfs/nssf tests/conformance -name '*.hpp' -o -name '*.cpp' | xargs clang-format-18 --dry-run
--Werror` passes). Full project rebuild clean. Full `ctest` (excluding the two disclosed pre-
existing flaky tests): 338/338 pass, zero regressions (6 additional skips are the pre-existing
Postgres-container-dependent tests, unrelated to this NF).

### What this ADR does NOT include

No Helm chart (matches the pre-existing gap shared by 6 other already-built NFs, see
Implementation above). No real slice-selection decisioning beyond catalog-membership filtering
(simplification 1). No `accessType` disambiguation (simplification 2). No availability
restriction/rejection logic (simplification 3). No `nssaiAvailabilityNotification` delivery on
`Delete` (simplification 4). NEF/SCP/BSF remain entirely unbuilt, same as before this ADR --
choosing NSSF from that list does not imply the other three are next by default; that is a real,
separate decision for a future turn.

