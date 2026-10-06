## ADR-0220: UDM `Nudm_UECM` Tier-B gap-closure -- seventh slice (`GetRegistrations` bare aggregate)

### Context

With every `RegistrationDataSets` field now backed by a real store (ADR-0215 through ADR-0219),
this ADR implements `GetRegistrations` itself -- the `/{ueId}/registrations` bare aggregate GET,
confirmed by direct read of `specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`. Real, confirmed: the
operation's `registration-dataset-names` query param is required and its own schema
(`RegistrationDatasetNames`) declares `minItems: 2` -- a real constraint this ADR enforces as a
real `400`, not a silently-ignored detail. The operation also accepts `single-nssai`/`dnn` query
params, documented as narrowing the `SMF_PDU_SESSIONS` dataset's own PDU-session list by slice/DNN
-- **not implemented**, disclosed below.

### Implementation

- New route composes `sbi_gen::RegistrationDataSets` from the six already-built per-group stores
  (`amf_registrations`, `amf_non3gpp_registrations`, `smf_registrations`, `smsf_3gpp_registrations`,
  `smsf_non3gpp_registrations`, `ip_sm_gw_registrations`, `nwdaf_registrations`), gated by the real
  `registration-dataset-names` query param, reusing `sbi_core::http2::split_form_array()` (ADR-0161
  infra, the same real pattern UDR's own `QueryProvisionedData`/`ReadPolicyData` dataset-name
  filters use).
- A requested dataset with no data for the `ueId` is simply omitted from the response (all
  `RegistrationDataSets` fields are optional in the real schema) -- no fabricated `404` for a
  partially-empty result, matching this project's own established `QueryProvisionedData`/
  `ReadPolicyData` convention.
- **Real, disclosed simplification**: `single-nssai`/`dnn` query params are accepted (parsed by the
  generated request-parameter machinery) but not honored -- `SMF_PDU_SESSIONS` always returns every
  PDU session's SMF registration for the `ueId` unfiltered. Filtering correctly would need
  per-session S-NSSAI/DNN inspection inside each stored `SmfRegistration`'s own PDU-session map,
  which this project hasn't built. Flagged here rather than silently dropped.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_220.cpp`, 1 test
(`ComposesAndFiltersRealRegistrationDataSets`), real spawned `nrf`+`udm` process pair over real TLS
1.3 + mTLS: seeds real AMF_3GPP, SMSF_3GPP, and NWDAF sub-resources via their own already-built PUT
routes; confirms a single `registration-dataset-names` value is a real `400` (the real spec's own
`minItems: 2`); confirms a 4-name request (3 seeded + 1 unseeded `AMF_NON_3GPP`) returns exactly
the 3 seeded fields with the correct data and omits the unseeded one; confirms narrowing to 2 of
the 3 seeded names correctly excludes the third -- a real filtering check, not just an aggregation
check.

Passes.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step; again
re-triggered a full `sbi-codegen` regeneration + full-project rebuild for the same CMake-reconfigure
reason disclosed in ADR-0219 -- confirmed via `ninja -n` showing a genuine 107-step plan). The new
test passes individually. Full suite re-run clean: 446/446 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

`single-nssai`/`dnn` filtering of the `SMF_PDU_SESSIONS` dataset (see Context/Implementation).
`SendRoutingInfoSm` remains unimplemented -- a genuinely separate operation (SMS routing-info
lookup, not a `RegistrationDataSets` composition) and the natural next slice.
`Nudm_UECM`'s three remaining independent single ops (`Trigger P-CSCF Restoration`,
`GetLocationInfo`, `authTrigger`) stay open. `Nudm_SDM` (~33) and `Nudm_PP` (11, already flagged
deferred in ADR-0082) remain unchanged. UDM is still the only NF with real Tier-B gaps
project-wide.

