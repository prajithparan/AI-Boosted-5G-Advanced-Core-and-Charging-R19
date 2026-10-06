## ADR-0202: UDM `Nudm_MT` + `Nudm_NIDDAU` + `Nudm_RSDS` + `Nudm_SSAU` + `Nudm_UEID` -- seventh ADR-0193 gap-closure

### Context

Found during ADR-0193's audit: all 5 of `TS29503_Nudm_MT.yaml`, `TS29503_Nudm_NIDDAU.yaml`,
`TS29503_Nudm_RSDS.yaml`, `TS29503_Nudm_SSAU.yaml`, `TS29503_Nudm_UEID.yaml` were real Tier-A gaps
-- explicitly named and deliberately deferred in `nfs/udm/src/main.cpp`'s own file header since
this project's original UDM turn. All 5 added to the pilot set this pass. Real, separate api roots
confirmed from each YAML's own `servers:` block (`/nudm-mt/v1`, `/nudm-niddau/v1`, `/nudm-rsds/v1`,
`/nudm-ssau/v1`, `/nudm-ueid/v1`).

Real cross-file schema fan-out confirmed to resolve correctly without extra pilot-set entries
(same `loader.py` lazy-resolution mechanism ADR-0201 already confirmed). Two real name collisions
found and resolved: `AuthorizationData` (independently declared by `TS29503_Nudm_NIDDAU.yaml` and
the already-wired `TS29505_Subscription_Data.yaml`) disambiguated by the codegen to
`AuthorizationData_Nudm_NIDDAU`/`AuthorizationData_Subscription_Data` -- collision-free, no
existing code referenced the bare name, nothing to fix. `ServiceSpecificAuthorizationInfo`
(independently declared by `TS29503_Nudm_SSAU.yaml` and `TS29505_Subscription_Data.yaml`)
disambiguated to `ServiceSpecificAuthorizationInfo_Nudm_SSAU`/
`ServiceSpecificAuthorizationInfo_Subscription_Data` -- this one DID break existing code:
`nfs/udr/src/main.cpp`'s own pre-existing `ServiceSpecificAuthorizationInfo` PUT route (a real,
already-implemented TS 29.505 UDR resource, unrelated to this ADR's own scope) referenced the bare
name, which no longer exists once the collision was introduced. Fixed by updating that one
reference to `ServiceSpecificAuthorizationInfo_Subscription_Data` -- a real, necessary fix
required by this ADR's own pilot-set change, not a functional change to UDR's own behavior
(verified via UDR's own existing 3 integration tests, all still pass unchanged).

### Implementation

- **`Nudm_MT`** (2 ops): `QueryUeInfo` and `ProvideLocationInfo` both reuse the real, existing
  `fetch_from_udr` helper (`nfs/udm/src/main.cpp`, already backing `GetAmData`/etc.) as a real
  existence-check probe against UDR's am-data -- no dedicated new store needed, no new UDR call
  shape introduced. Real `404` if the SUPI isn't seeded in UDR; else an honestly-empty
  `UeInfo_Nudm_MT{}`/`LocationInfoResult{}` `200` -- no real VoPS/5G-SRVCC/location data exists
  anywhere in this build to populate either from.
- **`Nudm_NIDDAU`**: `AuthorizeNiddData`, real structural validation of `AuthorizationInfo`'s
  required `snssai`/`dnn`/`mtcProviderInformation`/`authUpdateCallbackUri`, then a real, disclosed
  `501` -- no real MTC-provider/NIDD authorization policy data exists anywhere in this build to
  authorize against.
- **`Nudm_RSDS`**: `ReportSMDeliveryStatus`, real structural validation of `SmDeliveryStatus`'s
  required `gpsi`/`smStatusReport`, then a real `204` ack -- disclosed: no real SMS routing/retry
  logic exists to act on the report.
- **`Nudm_SSAU`**: `ServiceSpecificAuthorization` real structural validation then a real, disclosed
  `501` (no real service-specific authorization policy data exists for any of the real
  `AF_GUIDANCE_FOR_URSP`/`AF_REQUESTED_QOS`/`AF_PROVISION_N3GPP_DEV_ID_INFO` service types).
  `ServiceSpecificAuthorizationRemoval` real structural validation of the required `authId`, then a
  real `404` -- since `ServiceSpecificAuthorization` itself never issues a real `authId` (it always
  `501`s), no removal target can ever exist to match one.
- **`Nudm_UEID`**: `Deconceal` -- **not a stub**. Reuses this file's own existing, real,
  already-tested `deconceal_suci_if_needed()` (TS 33.501 Annex C ECIES Profile A/B, the exact same
  logic `GenerateAuthData`/`GenerateProseAV` already exercise internally, Tier 1c) to implement a
  genuinely working `Deconceal` operation: real `200` with the actual deconcealed SUPI on success,
  real `400` on a malformed/unsupported-scheme/MAC-verification-failed SUCI (same real error shape
  `GenerateAuthData`'s own SUCI path already uses). Same disclosed scope narrowing as the existing
  helper: only the IMSI-type (supiType `0`) SUCI form is supported.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_mt_niddau_rsds_ssau_ueid.cpp`, real spawned `nrf`+`udm`(+`udr` for
the `Nudm_MT` tests) processes over real TLS 1.3 + mTLS: `QueryUeInfo` -> real `404` for an unseeded
SUPI, real `200` with a verified-empty `UeInfo_Nudm_MT` for the real seeded `imsi-999700000000001`;
`ProvideLocationInfo` -> real `200` with a verified-empty `LocationInfoResult`; `AuthorizeNiddData`
-> real `501` on a structurally valid body, real `400` on one missing `dnn`;
`ReportSMDeliveryStatus` -> real `204`; `ServiceSpecificAuthorization` -> real `501`;
`ServiceSpecificAuthorizationRemoval` -> real `404`, real `400` on a body missing `authId`;
`Deconceal` -> **real, working decryption** of the exact TS 33.501 Annex C.4.3.1 ECIES Profile A
implementers' test vector (same one `tests/conformance/test_suci.cpp` already verifies
`aka_crypto::deconceal_profile_a` against) into the correct real SUPI (`imsi-274012001002086`),
verified end-to-end over the wire against the actual running `udm` process -- not a unit-level
crypto check -- plus a real `400` on the same vector with its MAC-tag tampered.

Real bug found and fixed during this verification pass: two of `Nudm_MT`'s tests initially failed
with real `500`s (`UDM could not reach UDR`) because `udr`'s own Postgres-backed HTTP/2 server
takes noticeably longer to start listening than `nrf`/`udm`, and the test's fixed-duration sleep
after spawning `udr` wasn't reliable margin under this session's system load. Fixed by adding a
real `wait_reachable` probe against `udr` itself before spawning `udm`. A related, more subtle bug
found in the same pass: the test originally used `ASSERT_EQ` on that same UDR-dependent status
check -- since `ASSERT_*` aborts the test function immediately on failure, a real timing miss was
skipping the test's own `kill()`/`waitpid()` cleanup, leaking the spawned `nrf`/`udr`/`udm`
processes, which then kept the test harness's captured stdout pipe open indefinitely (no EOF ever
arrives), presenting as an apparent test hang rather than a clean failure. Fixed by changing those
specific checks to `EXPECT_EQ` with the dependent body-parse guarded behind an `if (status == 200)`
-- cleanup now always runs regardless of outcome. The exact same latent bug was found, live, in the
pre-existing (untouched this session) `UdmIntegration.SdmDataRetrievalAndSubscriptions` test under
the same system load -- disclosed, not fixed here (out of this ADR's own scope), tracked as task
#166 for a separate turn.

### Testing

Full reconfigure + rebuild against the complete pilot set clean (`EXIT=0` verified directly from
the build log at every step, including after the real `TS29505_Subscription_Data.yaml` collision
fix). All 6 new UDM tests pass; UDR's own 3 pre-existing integration tests still pass unchanged
after the collision fix. No strays before or after any run (`ps aux` checked, including explicit-
PID cleanup of orphaned processes left by the pre-existing test's own latent bug, discovered
mid-verification).

### What this ADR does NOT include

No real VoPS/5G-SRVCC/location tracking data for `Nudm_MT` (`ProvideLocationInfo`'s honestly-empty
response is a direct, disclosed consequence). No real MTC-provider/NIDD or service-specific
authorization policy data anywhere in this build (`Nudm_NIDDAU`/`Nudm_SSAU` both real-`501` for
this reason). No real SMS routing/retry logic behind `Nudm_RSDS`'s ack. `Nudm_UEID`'s `Deconceal`
carries forward the exact same real, disclosed scope limit as `deconceal_suci_if_needed()` itself
(IMSI-type SUCI only, not NAI/GCI/GLI). The pre-existing `UdmIntegration.SdmDataRetrievalAndSubscriptions`
test's own latent timing/cleanup bug (task #166) is disclosed but not fixed by this ADR.

