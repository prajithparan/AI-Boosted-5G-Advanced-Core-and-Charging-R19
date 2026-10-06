## ADR-0195: AUSF `Nausf_UPUProtection` -- second ADR-0193 gap-closure

**Status:** Closed (work pushed; last citing commit 130dfb6 on origin/main, 2026-10-06).

### Context

The second smallest-and-clearest item on ADR-0193's backlog: `Nausf_UPUProtection`
(TS29509_Nausf_UPUProtection.yaml v1.4.0, TS 29.509, one real operation --
`POST /{supi}/ue-upu` -> `UpuSecurityInfo`) existed in the vendored spec tree but was never wired
into the sbi-codegen pilot set. Real UE Parameters Update (UPU) security, TS 33.501 clause 6.15:
the AUSF MAC-protects UE-parameter-update data the UDM pushes down via NAS, using the same
KAUSF-keyed derivation family as the already-built `Nausf_SoRProtection` (ADR-0081).

### Implementation

**Real spec citations, confirmed by direct read of the vendored `specs/TS_33_501.pdf` v19.6.0
Release 19** (same release this project's other AUSF crypto citations already use, ADR-0081):
- **Annex A.19, UPU-MAC-IAUSF generation** (page 243): `FC=0x7B`, `KDF(KAUSF, P0=UE Parameters
  Update Data [TS 24.501 §9.11.3.53A, starting octet 23], P1=CounterUPU)`, 128 LSBs.
- **Annex A.20, UPU-MAC-IUE/UPU-XMAC-IUE generation** (pages 243-244): `FC=0x7C`,
  `KDF(KAUSF, P0=0x01, P1=CounterUPU)`, 128 LSBs -- one function computes both, same real naming
  convention as A.18's own SoR-MAC-IUE/SoR-XMAC-IUE.
- **Clause 6.15.2.2, UE Parameters Update Counter** (page 125): CounterUPU is a real, SEPARATE
  16-bit counter from CounterSoR (clause 6.14.2.3) -- "associated" with the same KAUSF but
  maintained independently, same real state-machine shape (init `0x0001` when a fresh KAUSF is
  stored, increment per computation, AUSF suspends the service if the counter is about to wrap,
  reset to `0x0001` on a fresh KAUSF).

`aka_crypto::derive_upu_mac_iausf`/`derive_upu_mac_iue` added to `libs/aka-crypto/` (kdf.hpp/cpp),
mirroring `derive_sor_mac_iausf`/`derive_sor_mac_iue`'s own real shape. `ausf::KausfStore` (Redis-
backed, `nfs/ausf/src/kausf_store.hpp`/`.cpp`) extended with `counter_upu`/`suspended_upu` fields
and `use_upu_counter()`, alongside the existing `counter_sor`/`suspended`/`use_counter()` -- both
counters reset together in `store_fresh_kausf()` (both clauses state the same real reset rule), but
tracked and consumed independently, since they are real, distinct counters, not aliases.

`TS29509_Nausf_UPUProtection.yaml` and its real cross-file dependency
`TS29544_Nspaf_SecuredPacket.yaml` (referenced by `UpuData.routingId`) added to
`libs/sbi-generated/CMakeLists.txt`'s pilot set. Real codegen name collisions found and resolved
(both `UpuInfo` and `UpuData` are also declared, independently, by `Nudm_SDM.yaml`'s own schema) --
the real generated types are `sbi_gen::UpuInfo_Nausf_UPUProtection` and
`sbi_gen::UpuData_Nausf_UPUProtection`, not bare `UpuInfo`/`UpuData`, confirmed by reading the
actual generated header, not assumed from a narrower dry-run.

Real route registered in `nfs/ausf/src/main.cpp`, `POST /nausf-upuprotection/v1/{supi}/ue-upu`
(base path per the YAML's own `servers: url: '{apiRoot}/nausf-upuprotection/v1'`). Real behavior,
in order: auth check (`401`); real structural validation of the declared `minItems: 1` on
`upuDataList` (`400` -- the codegen's plain `std::vector` doesn't enforce this JSON-Schema
constraint on its own, same class of gap as LMF's own `ecgi`/`ncgi` check, ADR-0191); real KAUSF
lookup via `KausfStore::get` (`404` if none on record -- distinguishes "wrong AUSF instance" from
"feature gap"); then a real, disclosed `501`.

**Real, disclosed scope limit, unlike SoR's own gap.** Nausf_SoRProtection's own real dependency
gap (TS 24.501 §9.11.3.51 NAS encoding) has a real escape hatch: the YAML's `sorHeader`/
`steeringContainer` fields let the requester supply already-NAS-encoded opaque bytes directly, so
the SecuredPacket-form case is genuinely computable without this project ever needing a NAS
encoder. `Nausf_UPUProtection`'s own YAML gives no equivalent pre-encoded-bytes alternative to the
structured `upuDataList` -- every real path requires the TS 24.501 §9.11.3.53A NAS-layer encoder
this project doesn't have, genuinely out of this session's spec material. `CounterUPU` is
deliberately NOT consumed on the `501` path (no MAC is actually produced), matching the real spec
intent that the counter is a freshness input to an actual computation, not a request-attempt tally.

New conformance test `tests/conformance/test_ausf_upu_dtos.cpp` (2 round-trip tests over the real
generated, disambiguated DTOs), wired into `tests/conformance/CMakeLists.txt`. New integration test
`tests/integration/test_ausf_upu_protection.cpp` (real, separate nrf+udm+ausf OS processes, real
5G-AKA authentication to establish a real KAUSF, then the real 404/400/501 sequence), wired into
`tests/integration/CMakeLists.txt`.

### Live verification (real, live processes, not self-consistency)

Real `nrf`+`udm`+`ausf` processes (via the new integration test, not a manual curl session): (a)
`POST .../ue-upu` for a SUPI with no KAUSF on record -> real `404`; (b) real 5G-AKA
initiate+confirm (independently re-derived HXRES*/KSEAF, same cross-check pattern as
`test_ausf_ue_authentication.cpp`) to establish a real KAUSF for the SUPI; (c) empty `upuDataList`
against that SUPI -> real `400`; (d) a real, structurally-valid request against that same SUPI ->
real, disclosed `501`. A first run of this test passed only because a stray, unrelated `udm`
process left over from earlier in this session happened to be squatting the real UDM port -- caught
before treating that run as valid, both stray processes killed by explicit PID, and the test
re-run cleanly against its own freshly-spawned trio with no contamination, same real result.

### Testing

Full project rebuild clean. Full `ctest` (excluding the two known-flaky tests): 366/366 pass,
including the 2 new `AusfUpuDtos` tests and the new `AusfUpuProtectionIntegration` test.

### What this ADR does NOT include

A real TS 24.501 §9.11.3.53A NAS-layer encoder for UE Parameters Update Data (disclosed above --
genuinely out of this session's spec material, same class of gap as `Nausf_SoRProtection`'s own
§9.11.3.51 limitation). The real `501` is the honest consequence, not a workaround.

