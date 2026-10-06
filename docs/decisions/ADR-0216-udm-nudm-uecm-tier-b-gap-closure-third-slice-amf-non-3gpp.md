## ADR-0216: UDM `Nudm_UECM` Tier-B gap-closure -- third slice (AMF non-3GPP-access registration group)

### Context

Continuing UDM's own Tier-B backlog, this ADR closes the AMF non-3GPP-access registration group
-- `Non3GppRegistration` (PUT), `GetNon3GppRegistration` (GET), `UpdateNon3GppRegistration`
(PATCH) -- confirmed by direct read of `specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`. This is one
of the sub-resource groups ADR-0215 deliberately deferred rather than build piecemeal just to
partially populate `GetRegistrations`; closing it here (a genuinely self-contained real resource,
own real `AmfNon3GppAccessRegistration` schema, mirrors the already-built `amf-3gpp-access` group's
own real shape) is a real step toward that aggregate eventually being meaningful, though
`GetRegistrations` itself is not touched by this ADR (`smsf3Gpp`/`smsfNon3Gpp`/`ipSmGw`/
`nwdafRegistration` remain missing).

### Implementation

- New `AmfNon3GppRegistrationStore` (`nfs/udm/src/stores.hpp`/`.cpp`), in-memory, keyed by `ueId`
  -- identical shape to the already-existing `AmfRegistrationStore` (3GPP-access), reflecting the
  real schema symmetry between `Amf3GppAccessRegistration` and `AmfNon3GppAccessRegistration`.
- **PUT** (`Non3GppRegistration`): real `201`+`Location`+body on first create, real `200`+body on
  update -- same real response-code convention already established for `3GppRegistration`'s own
  PUT.
- **GET** (`GetNon3GppRegistration`): real `200`+body or `404`.
- **PATCH** (`UpdateNon3GppRegistration`): real RFC 7396 merge via
  `AmfNon3GppRegistrationStore::merge_patch`, real `404` if absent. Real, disclosed: the spec's
  own PATCH documents a `200`+`PatchResult` response for a partial-failure execution report (an
  array of per-path failure items) -- this project's simple `nlohmann::json::merge_patch()` has no
  partial-apply/rollback semantics to report on, so a fully-applied merge returns the real,
  also-spec-documented `204` (no content) instead of fabricating an always-empty
  `PatchResult.report`.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_216.cpp`, 1 test (`Non3GppRegistrationFullLifecycle`),
real spawned `nrf`+`udm` process pair over real TLS 1.3 + mTLS: real `404` before create, real
`201`+`Location` on first PUT, real `200` (not `201`) on a second PUT to the same `ueId`, real GET
round-trips the created document (`guami.amfId`, `imsVoPs`), real `204` on PATCH, a follow-up GET
confirms the merge landed while untouched fields (`deregCallbackUri`) survived, and PATCH against
an unknown `ueId` returns real `404`.

Passes.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step). The new
test passes individually. Full suite re-run clean: 442/442 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

`GetRegistrations` (the bare aggregate) and `SendRoutingInfoSm` remain open -- both still
genuinely depend on `smsf3Gpp`/`smsfNon3Gpp`/`ipSmGw`/`nwdafRegistration`, none of which this ADR
builds. `Nudm_UECM`'s remaining real operations (`Trigger P-CSCF Restoration`, `GetLocationInfo`,
`authTrigger`, the smsf-3gpp/non-3gpp-access groups, ip-sm-gw-registration group,
nwdaf-registrations group) stay open. `Nudm_SDM` (~33) and `Nudm_PP` (11, already flagged deferred
in ADR-0082) remain unchanged. UDM is still the only NF with real Tier-B gaps project-wide.

