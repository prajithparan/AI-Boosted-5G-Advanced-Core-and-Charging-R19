## ADR-0217: UDM `Nudm_UECM` Tier-B gap-closure -- fourth slice (SMSF registration groups)

### Context

Continuing UDM's own Tier-B backlog, this ADR closes the SMSF registration groups -- both
3GPP-access (`3GppSmsfRegistration`/`Get3GppSmsfRegistration`/`UpdateSmsf3GppRegistration`/
`3GppSmsfDeregistration`) and non-3GPP-access (`Non3GppSmsfRegistration`/
`GetNon3GppSmsfRegistration`/`UpdateSmsfNon3GppRegistration`/`Non3GppSmsfDeregistration`) --
confirmed by direct read of `specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`. Both real spec
resources share the identical real `SmsfRegistration`/`SmsfRegistrationModification` schemas
(unlike the AMF 3GPP/non-3GPP-access groups, whose schemas genuinely differ). This is a real step
toward `GetRegistrations` (still not built) eventually being meaningful -- its own real
`smsf3Gpp`/`smsfNon3Gpp` fields are now backed by real data, though `ipSmGw`/`nwdafRegistration`
remain missing.

### Implementation

- New `SmsfRegistrationStore` (`nfs/udm/src/stores.hpp`/`.cpp`), in-memory, keyed by `ueId`. Real,
  deliberate: kept as two distinct instances of this one class (`smsf_3gpp_registrations`/
  `smsf_non3gpp_registrations`) rather than a single shared store, even though the schema is
  identical -- same "real, distinct resource, not a rename" precedent `nfs/udr`'s own
  `SmsfContext3gppStore`/`SmsfNon3GppContextStore` already established.
- **PUT**/**GET**/**DELETE**/**PATCH**, both groups: identical real behavior and disclosed
  simplifications to the AMF non-3GPP-access group closed in ADR-0216 -- PUT uses only `201`/`200`
  (not the real spec's third alternative bodyless `204`), the real optional `ETag` response header
  and `If-Match`/`smsf-set-id` DELETE parameters are accepted but not populated/honored (no
  per-resource ETag layer exists), and PATCH returns the real `204` rather than a fabricated
  `PatchResult.report`.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_217.cpp`, 1 test exercising both groups via a
shared helper (`exercise_smsf_group`, called once per real path segment): real `404` before
create, real `201`+`Location` on first PUT, real `200` on repeat PUT, GET round-trips the created
document, real `204` on PATCH with the merge confirmed via a follow-up GET (untouched fields
survive), real `204` on DELETE, and a real `404` on a second DELETE of the same resource.

Passes.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step). The new
test passes individually. Full suite re-run clean: 443/443 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

`GetRegistrations`/`SendRoutingInfoSm` remain open -- still genuinely depend on `ipSmGw`/
`nwdafRegistration`, neither built here. `Nudm_UECM`'s remaining real operations (`Trigger
P-CSCF Restoration`, `GetLocationInfo`, `authTrigger`, ip-sm-gw-registration group,
nwdaf-registrations group) stay open. `Nudm_SDM` (~33) and `Nudm_PP` (11, already flagged
deferred in ADR-0082) remain unchanged. UDM is still the only NF with real Tier-B gaps
project-wide.

