## ADR-0215: UDM `Nudm_UECM` Tier-B gap-closure -- second slice (`PeiUpdate`, `UpdateRoamingInformation`)

### Context

Continuing UDM's own Tier-B backlog (the only NF with one left project-wide), this ADR closes 2
more of `Nudm_UECM`'s ~24 undisclosed operations, confirmed by direct read of
`specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`. Per the background audit from ADR-0214, most of
`Nudm_UECM`'s remaining ~24 operations (`GetRegistrations` bare aggregate, `SendRoutingInfoSm`,
the non-3gpp-access triple, smsf-3gpp/non-3gpp-access groups, ip-sm-gw-registration group,
nwdaf-registrations group) genuinely depend on OTHER not-yet-implemented sub-resources
(`amfNon3Gpp`/`smsf3Gpp`/`smsfNon3Gpp`/`ipSmGw`/`nwdafRegistration`) -- confirmed by direct read of
`RegistrationDataSets`' own real field list, not assumed. Rather than build several new
sub-resource stores just to make an aggregate meaningful, this slice picks the two operations
confirmed genuinely independent of that dependency chain:

- **`PeiUpdate`** (`POST /{ueId}/registrations/amf-3gpp-access/pei-update`): real
  `PeiUpdateInfo` request (`pei` required), real `204` -- merges into the ALREADY-implemented AMF
  3GPP-access registration document's own `pei` field.
- **`UpdateRoamingInformation`** (`POST /{ueId}/registrations/amf-3gpp-access/roaming-info-update`):
  real `RoamingInfoUpdate` request (`servingPlmn` required, `roaming`/`contextInfo` optional) --
  genuinely its own real resource (real `201`-with-`Location`-vs-`204` pair, not a field of the AMF
  registration document itself, confirmed by its own real response schema being `RoamingInfoUpdate`
  itself, not `Amf3GppAccessRegistration`).

### Implementation

- **`PeiUpdate`**: real RFC 7396 merge (`{"pei": ...}`) via the already-existing
  `AmfRegistrationStore::merge_patch`, reused verbatim -- no new store. Real `404` if no AMF
  3GPP-access registration exists yet for the `ueId`.
- **`UpdateRoamingInformation`**: new `RoamingInfoUpdateStore` (in-memory, keyed by `ueId`, same
  shape as `AmfRegistrationStore` itself) -- real `201`+`Location`+body on first create, real `204`
  on update, matching the spec's own documented response pair exactly (same precedent as the
  already-existing `3GppRegistration` PUT route).

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_215.cpp`, 2 tests, real spawned `nrf`+`udm`
process pairs over real TLS 1.3 + mTLS:
- `PeiUpdateMergesIntoExistingAmfRegistration`: real `404` against a `ueId` with no registration,
  seeds a real AMF 3GPP-access registration, real `204` on `PeiUpdate`, then a real GET confirms
  the `pei` field landed in the registration document while other fields set at creation
  (`guami.amfId`) survived the merge.
- `UpdateRoamingInformationRealCreateThenUpdate`: real `201`+`Location`+round-tripped body on
  first call, real `204` on a second call to the same `ueId`.

Both pass.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step). Both new
tests pass individually. Full suite re-run clean: 441/441 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

`Nudm_UECM`'s remaining ~22 operations stay open, most genuinely blocked on new sub-resource
stores (`amfNon3Gpp`/`smsf3Gpp`/`smsfNon3Gpp`/`ipSmGw`/`nwdafRegistration`) this ADR deliberately
did not build just to make `GetRegistrations` partially meaningful -- that aggregate is better
built once its real constituent sub-resources exist, not as a live-view-over-mostly-missing-data
resource. `Nudm_SDM` (~33) and `Nudm_PP` (11, already flagged deferred in ADR-0082) remain
unchanged. UDM is still the only NF with real Tier-B gaps project-wide.

