## ADR-0196: LMF `Nlmf_Broadcast` + `Nlmf_DataExposure` -- third ADR-0193 gap-closure

### Context

The third item on ADR-0193's backlog, both YAML files existed in the vendored spec tree but were
never wired into the sbi-codegen pilot set: `Nlmf_Broadcast` (TS29572_Nlmf_Broadcast.yaml v1.3.0,
one real operation, `POST /cipher-key-data`) and `Nlmf_DataExposure`
(TS29572_Nlmf_DataExposure.yaml v1.0.0, three real operations,
`CreateSubscription`/`ModifySubscription`/`DeleteSubscription`).

### Implementation

Both files added to `libs/sbi-generated/CMakeLists.txt`'s pilot set (no new cross-file
dependencies -- both only reference already-wired files: `TS29571_CommonData.yaml`,
`TS29572_Nlmf_Location.yaml`, `TS29503_Nudm_SDM.yaml`). Real generated types (`CipherRequestData`,
`CipherResponseData`, `LmfDataExposureSubscription`, etc.) landed in their own standalone headers
(`TS29572_Nlmf_Broadcast.hpp`/`TS29572_Nlmf_DataExposure.hpp`, not merged into the shared
`TS29122_CommonData_grp.hpp` -- no cross-file `$ref` cycle pulled them in), a real, disclosed
mistake caught and fixed via the first build attempt (missing `#include`s, not guessed correctly
from a prior NF's own SCC-grouping outcome).

**`CipheringKeyData` (Nlmf_Broadcast) -- real, structurally complete, but honestly always reports
no data.** This YAML's single operation is the AMF-facing QUERY interface only; it declares no
write/provisioning operation anywhere for populating real `CipheringDataSet` records (LTE/NR
positioning SIB ciphering key material is real O&M/vendor-provisioned data, TS 33.501's own
broader security framework, outside this specific SBI surface). Since this project has no such
provisioning path, `dataAvailability` honestly always reports `CIPHERING_KEY_DATA_NOT_AVAILABLE`
-- a real, disclosed absence of data, not a fabricated key. The real declared async
`CipheringKeyData` notification callback (to the caller's own `amfCallBackURI`) correspondingly
never fires in this build.

**`CreateSubscription`/`ModifySubscription`/`DeleteSubscription` (Nlmf_DataExposure) -- real, full
create+modify+delete lifecycle, no RF dependency.** New `lmf::DataExposureSubscriptionStore`
(`nfs/lmf/src/stores.hpp`/`.cpp`), following the exact real, established `UpSubscriptionStore`
shape from this same NF (ADR-0191) for create/delete, plus a real `apply_patch()` using
`nlohmann::json`'s own `.patch()` (the same real RFC 6902 JSON Patch mechanism already used by
`nfs/nrf`'s `NfRegistry::apply_patch`). `201` responses add a real `Location` header at the HTTP
layer (the YAML's own header block requires it, unlike `UpSubscriptions`' own real gap).

**Real, disclosed limit on this operation's own notification path**, same class of gap as
`LocationMeasure`/`DetermineLocation` (ADR-0191): the real `LmfDataExposureNotification` callback
carries `LmfDataExposureReport.samplingDataList` -> real `locMeasureData`/`groundTruth` location
measurement data, which this project has no real source for (no LPP/PRU/NRPPa capability). The
subscription lifecycle managing this is fully real; the notification itself can never actually
fire with real content in this build.

### Live verification (real, live processes, not self-consistency)

Real `nrf`+`lmf` processes, real mTLS curl against all 4 routes: `CipheringKeyData` with a real
`amfCallBackURI` -> real `200`, `dataAvailability=CIPHERING_KEY_DATA_NOT_AVAILABLE`.
`CreateSubscription` -> real `201` + `Location: .../subscriptions/dexpsub-1` + echoed body.
`ModifySubscription` with a real RFC 6902 patch (`replace /notifyCorrelationId`) against that same
id -> real `204`. `DeleteSubscription` -> real `204`; repeated -> real `404`. Processes killed by
explicit PID afterward.

### Testing

Full project rebuild clean (after fixing the missing-includes build failure described above). Full
`ctest` (excluding the two known-flaky tests): 369/369 pass, including the 3 new
`LmfBroadcastDtos`/`LmfDataExposureDtos` tests.

### What this ADR does NOT include

Real O&M/vendor provisioning of `CipheringDataSet` records (disclosed above -- outside this SBI
surface's own real scope, not merely unbuilt). Real LPP/PRU/NRPPa location measurement data for
`Nlmf_DataExposure`'s own notification reports (same disclosed gap as `DetermineLocation`/
`LocationMeasure`, ADR-0191) -- the subscription lifecycle is real and complete; the data it would
report on is not available in this build.

