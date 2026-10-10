## ADR-0219: UDM `Nudm_UECM` Tier-B gap-closure -- sixth slice (NWDAF registration group)

### Context

Continuing UDM's own Tier-B backlog, this ADR closes the NWDAF registration group --
`NwdafRegistration` (PUT), `GetNwdafRegistration` (GET, collection), `NwdafDeregistration`
(DELETE), `UpdateNwdafRegistration` (PATCH) -- confirmed by direct read of
`specs/5G_APIs-REL-19/TS29503_Nudm_UECM.yaml`. Real, confirmed: no individual GET operation exists
for a single NWDAF registration at all -- the individual path
(`/{ueId}/registrations/nwdaf-registrations/{nwdafRegistrationId}`) defines only `put`/`delete`/
`patch`; the only GET is the collection-level `GetNwdafRegistration`
(`/{ueId}/registrations/nwdaf-registrations`). Also confirmed: `GetNwdafRegistration`'s own real
response schema is a *bare* JSON array of `NwdafRegistration` with `minItems: 1` (not wrapped in
`NwdafRegistrationInfo` the way the already-built SMF registration group's own collection GET
wraps in `SmfRegistrationInfo`) -- the two collection GETs genuinely use different response
shapes in the real spec, not an inconsistency introduced here; the `minItems: 1` constraint is why
an empty result is real `404`, not a fabricated empty-array `200`. This closes the last remaining
real dependency of the still-open `GetRegistrations` bare aggregate -- every field of
`RegistrationDataSets` (`amf3Gpp`, `amfNon3Gpp`, `smfRegistration`, `smsf3Gpp`, `smsfNon3Gpp`,
`ipSmGw`, `nwdafRegistration`) is now backed by real store data.

### Implementation

- New `NwdafRegistrationStore` (`nfs/udm/src/stores.hpp`/`.cpp`), in-memory, keyed by
  `(ueId, nwdafRegistrationId)` -- same nested-map + `list_for_ue` shape as the already-built
  `SmfRegistrationStore` (a UE can be served by multiple NWDAF instances concurrently, each under
  its own `nwdafRegistrationId`).
- **GET** (collection, `/{ueId}/registrations/nwdaf-registrations`): real `404` when the list is
  empty (per the schema's own `minItems: 1`, see Context), else a bare JSON array.
- **PUT**/**DELETE**/**PATCH** (individual, `/.../nwdaf-registrations/{nwdafRegistrationId}`): same
  real behavior and disclosed simplifications as every other registration group in this file --
  PUT uses only `201`/`200` (not the real spec's third alternative bodyless `204`), PATCH returns
  real `204` (a fully-applied `merge_patch()` has no partial-failure report to put in the real
  spec's alternative `PatchResult` body), ETag/`If-Match` accepted but not honored (no per-resource
  versioning layer exists anywhere in this project).
- No individual GET route was added, matching the real spec's own omission (see Context) --
  verification for PUT/PATCH/DELETE state changes is done through the collection GET instead, both
  in the implementation's own design and in the test below.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_uecm_gap_closure_219.cpp`, 1 test
(`NwdafRegistrationFullLifecycle`), real spawned `nrf`+`udm` process pair over real TLS 1.3 + mTLS:
real `404` on the collection GET before create, real `201`+`Location` on first PUT, real `200` on
repeat PUT, collection GET returns a 1-element array with the created `nwdafInstanceId`, real `204`
on PATCH (verified via a follow-up collection GET confirming the merged `nwdafSetId`), real `204`
on DELETE, real `404` on a follow-up collection GET and on a second DELETE of the same resource.

Passes.

### Testing

Full reconfigure + rebuild clean (`EXIT=0` verified directly from each build-log step; this
particular reconfigure re-triggered a full `sbi-codegen` regeneration + full-project rebuild since
the CMake reconfigure step bumped every generated file's mtime, invalidating every NF's own
compile -- confirmed via `ninja -n` showing a genuine 74-step plan, not a hang; the earlier
attempts that the harness reported as "killed" were `run_in_background`/`Bash` timeout artifacts,
not build failures -- `ninja`'s own incremental state was never lost across them, confirmed by
each subsequent attempt resuming from where the last one left off). The new test passes
individually. Full suite re-run clean: 445/445 passing (`ctest --timeout 120 -E
"UdrIntegration.AmfContextLifecycle|UdmIntegration.SdmDataRetrievalAndSubscriptions"`, matching
`.github/workflows/ci.yml`'s own exclusion). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

`GetRegistrations` and `SendRoutingInfoSm` themselves are still not implemented -- this ADR closes
their last missing *dependency* (every `RegistrationDataSets` field now has real backing data), not
the two operations themselves; both remain open Tier-B items and are the natural next slice.
`Nudm_UECM`'s three remaining independent single ops (`Trigger P-CSCF Restoration`,
`GetLocationInfo`, `authTrigger`) stay open. `Nudm_SDM` (~33) and `Nudm_PP` (11, already flagged
deferred in ADR-0082) remain unchanged. UDM is still the only NF with real Tier-B gaps
project-wide.

