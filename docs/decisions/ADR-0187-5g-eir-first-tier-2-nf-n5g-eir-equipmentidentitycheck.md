## ADR-0187: 5G-EIR -- first Tier 2 NF, `N5g-eir_EquipmentIdentityCheck` (complete, not a slice)

### Context: Tier 2 survey and why 5G-EIR first

Fifth NF built under ADR-0184's continuous move-to-next-NF process, and this project's first Tier
2 NF (CLAUDE.md's own scope list), following the close of the original Tier 1 `nssf`/`nef`/`scp`/
`bsf` "still not done" list (ADR-0183 through ADR-0186). Before picking a candidate, a real survey
of every Tier 2 NF's actual YAML file count was performed by direct read of
`specs/5G_APIs-REL-19/`, not estimated: NWDAF 10 files, DCCF 3, ADRF 3, MFAF 3, NSACF 2, TSCTSF 3,
EASDF 2, UCMF 2, SMSF 1 file/5 ops, **5G-EIR 1 file/1 op**, LMF 3, GMLC 1 file/5 ops, NSSAAF 2,
AAnF 2, UDSF 2, SEPP 2. 5G-EIR chosen as this turn's candidate specifically because its real API
surface (`N5g-eir_EquipmentIdentityCheck.yaml` v1.4.0) is exactly 1 operation -- confirmed by
direct read of the 120-line YAML, not estimated -- meaning this NF could be built **complete**
this turn, not another disclosed partial slice like NEF (1 of 14 files) or SCP (proxy role
undesigned).

### Implementation

Source: `specs/5G_APIs-REL-19/TS29511_N5g-eir_EquipmentIdentityCheck.yaml` (v1.4.0), commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. The one real operation, `GetEquipmentStatus`
(`GET /equipment-status?pei={pei}&supi={supi}&gpsi={gpsi}`), fully implemented.

- `libs/sbi-generated/CMakeLists.txt`: YAML added to the pilot set (2198 types across 44 files
  after adding). Landed in `TS29511_N5g-eir_EquipmentIdentityCheck.hpp`; no name collisions.
- `nfs/eir/` scaffolded to match `nfs/nssf`/`nfs/bsf`/`nfs/nef`/`nfs/scp`'s shape (in-memory store,
  no Redis/Postgres): `CMakeLists.txt`, `src/stores.hpp`/`.cpp` (`EquipmentStatusStore`, seed()/get()
  only -- see disclosed gap below), `src/main.cpp` (the one route + NRF registration/heartbeat +
  OAuth2 + OTel/Prometheus). `config/eir.json` (port 7792, metrics 9480). `certs/eir/` generated
  via `scripts/gen-lab-pki.sh`. Real, deliberate naming choice: directory/binary/config use `eir`
  (not `5g-eir`) since CMake target names and generated identifiers starting with a digit are a
  real, avoidable source of tooling friction -- the real NF identity is unaffected, see the
  real-bug note directly below for what that identity actually is. Added to top-level
  `CMakeLists.txt`, `deploy/docker/docker-compose.yml`/`eir.Dockerfile`, and the `pki-init`
  cert-generation argument list. No Helm chart -- same pre-existing, disclosed, project-wide gap.
- New conformance test file `tests/conformance/test_eir_dtos.cpp` (2 round-trip tests over the real
  generated `EirResponseData`/`EquipmentStatus` DTOs), wired into `tests/conformance/CMakeLists.txt`.

### Real bug found and fixed during live verification (not a self-consistency pass)

`kNfType` was initially written as `"5G-EIR"` (hyphen) -- NRF's own real `NFProfile` validation
(ADR-0102's `known_nf_types()`) correctly rejected this with a live, real `400 Invalid NFProfile`
(`"nfType '5G-EIR' is not a recognized NFType"`), causing the registration retry loop to spin.
Direct read of `specs/5G_APIs-REL-19/TS29510_Nnrf_NFManagement.yaml` confirmed the real TS 29.510
`NFType` enum value is `5G_EIR` (underscore) -- `nfs/eir/src/main.cpp`'s `kNfType` fixed to match,
with the discrepancy (TS 29.511's own service naming uses a hyphen; TS 29.510's own `NFType` enum
uses an underscore -- both real, not a typo either way) documented in a code comment at the fix
site. This is exactly the class of catch this project's own live-verification-over-self-consistency
discipline exists for: a unit/round-trip test on `EirResponseData` alone would never have caught
NRF rejecting the registration profile.

### Real, disclosed simplification (same structural shape as ADR-0185/ADR-0186's findings)

The real YAML has no operation anywhere that lets a caller WRITE an equipment's status into the
5G-EIR -- real IMEI-database provisioning (whitelist/blacklist/greylist) is genuinely OAM/GSMA
scope, out of 3GPP's own standardized SBI framework here, not just unbuilt. `EquipmentStatusStore`
is therefore seed()-only, seeded with 3 illustrative PEIs (real IMEISV format, TS 23.003) covering
all 3 real `EquipmentStatus` enum values. Real, disclosed, deferred wiring: TS 23.502 §4.2.2.2.2's
Registration procedure has AMF optionally invoke this service to verify a UE's PEI -- this
project's own `nfs/amf` does not yet call it (same pattern as NEF's own PFD-management/UPF
non-wiring, ADR-0185).

### Live verification (real, live processes, not self-consistency)

Real `nrf` + `eir` processes, real OAuth2 bearer token, real mTLS curl against
`GET /n5g-eir-eic/v1/equipment-status`: `pei=imeisv-3510000000000010` -> real `200`
`{"status":"WHITELISTED"}`; `...020` -> real `200` `BLACKLISTED`; `...030` -> real `200`
`GREYLISTED`; unknown `pei` -> real `404` `"PEI Not Found"`; missing `pei` param entirely -> real
`400` `"Missing mandatory query parameter"`; malformed bearer token -> real `401`
`ProblemDetails`. Prometheus `/metrics`: `eir_get_equipment_status_total` correctly at 4 (3
successful lookups + the 1 unknown-`pei` 404, matching where the counter increments in the real
route handler, before the not-found check). Both processes killed cleanly afterward
(PID-verified).

### Testing

`eir` and `conformance_tests` built clean (zero warnings) both before and after
`clang-format-18`. Full project rebuild clean (39/39 targets, only the pre-existing onnxruntime
static-lib linker warning, unrelated to this change). Full `ctest` (excluding the two disclosed
pre-existing flaky tests): 352/352 pass, zero regressions (2 new `EirDtos` tests +
`structural_conformance` covering the new DTOs).

### What this ADR does NOT include

The real write/provisioning path into the equipment database (genuinely out of 3GPP's own SBI
framework scope, not unbuilt). AMF wiring to call this NF during Registration (real, disclosed,
deferred -- same class of gap as NEF/UPF non-wiring). No Helm chart (pre-existing, disclosed,
project-wide gap). Unlike NEF/SCP, this ADR's own real API surface IS fully closed -- 5G-EIR's
entire real spec (1 file, 1 operation) is now built and live-verified, not a partial slice.

