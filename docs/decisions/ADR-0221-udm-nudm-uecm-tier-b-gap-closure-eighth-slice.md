## ADR-0221: UDM `Nudm_UECM` Tier-B gap-closure -- eighth slice (`SendRoutingInfoSm`)

### Context

This ADR implements `SendRoutingInfoSm`, the last open item off `Nudm_UECM`'s own CRUD-composition
backlog, confirmed by direct read of `specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`. A real custom
POST operation (`/{ueId}/registrations/send-routing-info-sm`) that retrieves addressing information
for SMS delivery, composing `RoutingInfoSmResponse` from real SMSF/IP-SM-GW registration data
rather than any new store.

### Implementation

- Composes `smsf3Gpp`/`smsfNon3Gpp` from the already-built `smsf_3gpp_registrations`/
  `smsf_non3gpp_registrations` stores, and `ipSmGw` (wrapped in the real spec's own `IpSmGwInfo`
  envelope, `{ ipSmGwRegistration, ipSmGwGuidance }`) from `ip_sm_gw_registrations`.
- **Real, disclosed simplifications**: `smsRouter` (`SmsRouterInfo`, an SMS-Router-at-UDM concept)
  is never populated -- this project has no config surface or store for it. `ipSmGwGuidance`
  (delivery-time hints) is never populated -- no data source for it exists. `mpsMsgIndication` is
  left absent, which is schema-equivalent to its own documented `false` default (not a real
  omission).
- **Real judgment call, disclosed**: a `ueId` with neither an SMSF nor an IP-SM-GW registration has
  no real addressing information to hand back, so this returns a real `404` rather than a `200`
  with an empty body. The real spec lists `404` among this operation's own general-error responses,
  and this project has no established "return a fabricated empty success body" convention for a
  genuinely-not-found subscriber to fall back on instead.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_221.cpp`, 1 test
(`ComposesRealRoutingInfoFromSeededStores`), real spawned `nrf`+`udm` process pair over real TLS
1.3 + mTLS: real `404` for a `ueId` with no SMSF/IP-SM-GW registration; after seeding real
SMSF_3GPP + IP-SM-GW sub-resources via their own already-built PUT routes, a real `200` correctly
composes both (`smsf3Gpp.smsfInstanceId`, `ipSmGw.ipSmGwRegistration.ipsmgwFqdn`) while confirming
`smsfNon3Gpp`/`smsRouter` are correctly absent.

Passes.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step; again
re-triggered a full `sbi-codegen` regeneration + full-project rebuild for the same CMake-reconfigure
reason disclosed in ADR-0219/ADR-0220). The new test passes individually. Full suite re-run clean:
447/447 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

**Real CI lint failure caught and fixed in this window**: the ADR-0219 push (run `32965047394`)
genuinely failed `.github/workflows/ci.yml`'s `lint` job -- several new lines in
`NwdafRegistrationStore`/its routes and the ADR-0220 test exceeded `clang-format-18`'s column-width
rules. Fixed via `clang-format-18 -i` on the affected files, verified clean against the exact CI
invocation (`find libs nfs tests -name '*.hpp' -o -name '*.cpp' | xargs clang-format-18 --dry-run
--Werror`, filtered to tracked files only -- an unrelated pre-existing violation in a gitignored
Python venv's vendored C++ source, `nfs/chf/training/.venv/...`, is real but out of CI's own scope
and untouched), both affected tests re-verified passing after the reformat, committed separately
(not amended, per this project's own git-safety rule) and pushed alongside this ADR. The `build` and
`sanitize (tsan)` job failures on that same run were the already-documented CI-runner shutdown
flakiness (`ninja: build stopped: interrupted by user`, exit 143) -- not a code issue, confirmed by
reading the actual failure logs rather than assuming.

### What this ADR does NOT include

`smsRouter`/`ipSmGwGuidance` population (see Context/Implementation) -- both are real, disclosed
gaps this project has no data source for yet. This closes `Nudm_UECM`'s entire remaining
CRUD/composition backlog; only `Trigger P-CSCF Restoration`, `GetLocationInfo`, and `authTrigger`
remain as `Nudm_UECM`'s last three independent single ops. `Nudm_SDM` (~33) and `Nudm_PP` (11,
already flagged deferred in ADR-0082) remain unchanged. UDM is still the only NF with real Tier-B
gaps project-wide.

