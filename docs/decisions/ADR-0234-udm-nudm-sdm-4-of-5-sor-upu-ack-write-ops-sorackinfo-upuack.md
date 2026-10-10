## ADR-0234: UDM `Nudm_SDM` -- 4 of 5 SOR/UPU/ack write ops (SorAckInfo/UpuAck/S-NSSAIs Ack/CAG Ack)

### Context

Confirmed by direct read of `TS29503_Nudm_SDM.yaml`: this real 5-op group splits into 4
structurally identical ops and 1 genuinely different one.

- **`SorAckInfo`** (`PUT /{supi}/am-data/sor-ack`), **`UpuAck`**
  (`PUT /{supi}/am-data/upu-ack`), **`S-NSSAIs Ack`** (`PUT /{supi}/am-data/subscribed-snssais-ack`
  -- real operationId literally contains a space, not normalized here, same precedent as
  ADR-0227's own `Trigger P-CSCF Restoration`), and **`CAG Ack`**
  (`PUT /{supi}/am-data/cag-ack`, same real space-in-operationId quirk): all 4 share the identical
  real shape -- body `AcknowledgeInfo` (required `provisioningTime`), real `204` on success.
- **`Update SOR Info`** (`POST /{supi}/am-data/update-sor`): genuinely different -- body
  `SorUpdateInfo`, real `200` response with a constructed `SorInfo` (`ackInd`/`provisioningTime`
  required). Investigated whether this project has the real crypto material to build a real
  `SorInfo`: `libs/aka-crypto/include/aka_crypto/kdf.hpp` already has `derive_sor_mac_iausf`/
  `derive_sor_mac_iue` (TS 33.501 Annex A.17/A.18, confirmed real, pre-existing -- not the same
  "missing KDF material" blocker ADR-0104's AUSF ProSe deferral hit). What's genuinely missing:
  a real per-UE `CounterSoR` state machine (TS 33.501 clause 6.14.2.3) and real
  steering-of-roaming list content -- neither exists anywhere in this build. Fabricating either
  would violate this project's own no-invention rule, so `Update SOR Info` is left open rather
  than stubbed with invented state.

### Implementation

4 routes added to `nfs/udm/src/main.cpp`'s Nudm_SDM section via a real loop over the 4 real path
segments (`sor-ack`/`upu-ack`/`subscribed-snssais-ack`/`cag-ack`), each parsing a real
`AcknowledgeInfo` body and existence-checking the UE via the same `fetch_from_udr(..., "am-data")`
probe `GetAmData`/`Nudm_MT`'s own ops already use. Real, disclosed non-relay, same class as
ADR-0227's own `Trigger P-CSCF Restoration`/`authTrigger` disclosure: no real onward relay to
AUSF's own SoR-Protection/UPU-Protection service exists in this build to actually consume these
acks, so this project accepts and validates (real `404` for an unknown UE, real `204` otherwise)
without forwarding.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_sdm_gap_closure_234.cpp`, 1 test
(`AckOpsReturn204ForKnownUeAnd404ForUnknown`): real `204` for all 4 ack ops against the seeded test
SUPI; real `404` for all 4 against an unseeded SUPI; real `400` for a malformed body (missing
required `provisioningTime`).

Passes.

### Testing

Full reconfigure + rebuild clean. New test passes individually. Full suite re-run clean at `-j1`
(456/456, 6 legitimately skipped for missing local Postgres env vars, same as always). No strays
before or after any run (`ps aux` checked explicitly).

### What this ADR does NOT include

`Update SOR Info` (the 5th op in this group) remains open, for the real reasons disclosed above --
a genuinely separate, larger initiative (real CounterSoR state machine + real steering-list
content), not bundled into this ADR's own accept-and-validate slice. Still open after this ADR: a
6-op shared-data family, 2 identifier-lookup ops, `Update SOR Info`, and `Nudm_PP` (11 ops,
ADR-0082).

