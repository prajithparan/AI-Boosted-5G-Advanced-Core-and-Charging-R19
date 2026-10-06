## ADR-0218: UDM `Nudm_UECM` Tier-B gap-closure -- fifth slice (IP-SM-GW registration resource)

### Context

Continuing UDM's own Tier-B backlog, this ADR closes the IP-SM-GW registration resource --
`IpSmGwRegistration` (PUT), `GetIpSmGwRegistration` (GET), `IpSmGwDeregistration` (DELETE) --
confirmed by direct read of `specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`. Real, confirmed:
genuinely no PATCH operation exists for this resource in the spec at all (simpler than the AMF/SMSF
registration groups closed in ADR-0216/ADR-0217). This closes the second-to-last real dependency
of the still-open `GetRegistrations` bare aggregate -- only `nwdafRegistration` remains missing
after this ADR.

### Implementation

- New `IpSmGwRegistrationStore` (`nfs/udm/src/stores.hpp`/`.cpp`), in-memory, keyed by `ueId` --
  same shape as `AmfRegistrationStore` minus `merge_patch` (no PATCH exists for this resource).
- **PUT**/**GET**/**DELETE**: same real behavior and disclosed simplification as every other
  registration group in this file -- PUT uses only `201`/`200` (not the real spec's third
  alternative bodyless `204`).

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_218.cpp`, 1 test
(`IpSmGwRegistrationFullLifecycle`), real spawned `nrf`+`udm` process pair over real TLS 1.3 +
mTLS: real `404` before create, real `201`+`Location` on first PUT, real `200` on repeat PUT, GET
round-trips the created document (`ipsmgwFqdn`), real `204` on DELETE, real `404` on a follow-up
GET and on a second DELETE of the same resource.

Passes.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step). The new
test passes individually. Full suite re-run clean: 444/444 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

`GetRegistrations`/`SendRoutingInfoSm` remain open -- only `nwdafRegistration` still blocks them
after this ADR. `Nudm_UECM`'s remaining real operations (`Trigger P-CSCF Restoration`,
`GetLocationInfo`, `authTrigger`, nwdaf-registrations group) stay open. `Nudm_SDM` (~33) and
`Nudm_PP` (11, already flagged deferred in ADR-0082) remain unchanged. UDM is still the only NF
with real Tier-B gaps project-wide.

