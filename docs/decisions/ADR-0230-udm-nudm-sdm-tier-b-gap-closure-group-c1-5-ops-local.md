## ADR-0230: UDM `Nudm_SDM` Tier-B gap-closure -- group C1 (5 ops, local composition + existing UDR routes)

### Context

Continues `Nudm_SDM`'s Tier-B closure after ADR-0228's group A+B (13 ops). Group C (7 ops, real
data source unconfirmed at ADR-0228 time) re-scoped in full against
`specs/5G_APIs-REL-19/TS29503_Nudm_SDM.yaml`, splitting into two real sub-groups:

- **C1** (5 ops, closed by this ADR): `GetNSSAI`, `GetUeCtxInAmfData`, `GetUeCtxInSmfData`,
  `GetUeCtxInSmsfData`, `GetEcrData` -- each has real data already available in this file or in
  UDR, confirmed by direct schema cross-reference, no new stores needed.
- **C2** (2 ops, deferred, not touched by this ADR): `GetTimeSyncSubscriptionData`
  (`TimeSyncSubscriptionData`) and `GetRangingSlPosData` (`RangingSlPosSubscriptionData`) -- real,
  confirmed by direct schema read: zero existing data source anywhere in this codebase for either
  schema's own fields (`afReqAuthorizations`/`serviceIds` and `rangingSlPosAuth`/`rangingSlPosPlmn`/
  `rangingSlPosQos` respectively). Would need brand-new UDR-side Postgres-backed stores + routes
  before UDM could serve either at all -- a genuinely larger lift, deferred.

Real, confirmed schema findings for each C1 op:

- **`GetNSSAI`** (`GET /{supi}/nssai` -> `Nssai`): `Nssai` is literally the same schema
  `AccessAndMobilitySubscriptionData.nssai` already uses (`$ref
  '#/components/schemas/Nssai'` in both places) -- a real subset view into the already-fetched
  am-data, not a new resource.
- **`GetUeCtxInAmfData`** (`GET /{supi}/ue-context-in-amf-data` -> `UeContextInAmfData` =
  `{epsInterworkingInfo, amfInfo[]}`): `AmfInfo`'s own `amfInstanceId`/`guami` fields are an exact
  match for fields already on the stored `Amf3GppAccessRegistration`/`AmfNon3GppAccessRegistration`
  records (same real composition pattern ADR-0227's `GetLocationInfo` already established;
  `accessType` derived the same way). `epsInterworkingInfo` has no data source anywhere in this
  file -- real, disclosed gap, omitted rather than fabricated.
- **`GetUeCtxInSmfData`** (`GET /{supi}/ue-context-in-smf-data` -> `UeContextInSmfData` =
  `{pduSessions, pgwInfo[], emergencyInfo}`): `PduSession`'s own `dnn`/`smfInstanceId`/`plmnId`/
  `singleNssai` fields are a real, near-complete subset of `SmfRegistration`'s own fields (both
  confirmed by direct read), composable from the already-stored `SmfRegistrationStore`. Real,
  disclosed narrowing found during implementation: `PduSession.dnn` is required while
  `SmfRegistration.dnn` is optional -- a stored registration with no `dnn` (a real, valid
  `SmfRegistration` per its own schema, and in fact this project's own `SmfRegistrationLifecycle`
  test data has none) cannot be represented as a valid `PduSession` without fabricating the field,
  so that entry is skipped rather than invented. `pgwInfo`/`emergencyInfo` have no data source --
  real, disclosed gap.
- **`GetUeCtxInSmsfData`** (`GET /{supi}/ue-context-in-smsf-data` -> `UeContextInSmsfData` =
  `{smsfInfo3GppAccess, smsfInfoNon3GppAccess}`): `SmsfInfo`'s own `smsfInstanceId`/`plmnId`/
  `smsfSetId` fields are an exact 1:1 match with the already-stored `SmsfRegistration` records'
  fields (both confirmed by direct read) -- real, COMPLETE composition, no gap.
- **`GetEcrData`** (`GET /{supi}/am-data/ecr-data` -> `EnhancedCoverageRestrictionData` =
  `{plmnEcInfoList}`): an exact match for UDR's own already-live `coverage-restriction-data`
  resource (ADR-0102/ADR-0106) -- genuinely distinct URL shape from the existing `fetch_from_udr`
  helper's own (ueId, servingPlmnId, segment) pattern (UDR's coverage-restriction-data resource is
  keyed by ueId alone, no servingPlmnId in its path), so implemented as a small, dedicated call
  rather than reusing that helper.

User-approved slicing (via `AskUserQuestion`): do C1 now (5 ops); defer C2 (new UDR stores needed)
to a later turn.

### Implementation

All 5 routes added to `nfs/udm/src/main.cpp`'s Nudm_SDM section. `GetUeCtxInAmfData`/
`GetUeCtxInSmfData`/`GetUeCtxInSmsfData` are all real, honest `200`-always ops (none of their three
real response schemas has any required top-level field) -- composed sub-fields (`amfInfo`/
`pduSessions`/`smsfInfo3GppAccess`/`smsfInfoNon3GppAccess`) are omitted, not fabricated as empty
placeholders, when nothing is known; `amfInfo` specifically is omitted rather than emitted as an
empty array since the real schema's own `minItems: 1` forbids an empty array for that field.
`GetNSSAI`/`GetEcrData` retain real `404` semantics (sourced from UDR, which does 404 for missing
data), matching the existing group A/B precedent.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_sdm_gap_closure_230.cpp`, 2 tests:

- `UdrBackedOpsReturnRealData` (spawns real `nrf`+`udr`+`udm`): real `200` with the exact seeded
  `nssai.defaultSingleNssais`/`ecr.plmnEcInfoList` for the seeded SUPI on `GetNSSAI`/`GetEcrData`;
  real `404` on both for an unseeded SUPI.
- `LocalCompositionOpsReturnRealSeededData` (spawns real `nrf`+`udm`): confirms all three
  composition ops return real `200` with no composed sub-field present before any registration
  exists; after seeding real AMF-3GPP-access, SMF (with a real `dnn`), and both SMSF 3GPP/non-3GPP
  registrations via their own existing PUT routes, confirms each op's real composed response
  matches the seeded data exactly (`amfInfo[0].amfInstanceId`/`guami.amfId`/`accessType`;
  `pduSessions["7"].dnn`/`smfInstanceId`/`plmnId.mcc`/`singleNssai.sst`;
  `smsfInfo3GppAccess.smsfInstanceId`/`smsfInfoNon3GppAccess.smsfInstanceId`).

Both pass.

### Testing

Full reconfigure + rebuild clean. Both new tests pass individually. Full suite re-run clean at
`-j1` (deliberately serial, avoiding the port-contention pollution seen in an earlier `-j4` local
run that was traced to leaked processes from unrelated prior manual test invocations, not this
ADR's own code) -- see commit for exact pass count. No strays before or after any run (`ps aux`
checked explicitly).

### What this ADR does NOT include

C2's 2 ops (`GetTimeSyncSubscriptionData`/`GetRangingSlPosData`, need brand-new UDR stores + routes
first) -- deferred. Also still open, untouched by this ADR: `Subscribe`/`Unsubscribe`/`Modify`
completion, the 5 SOR/UPU/ack write ops, the 6-op shared-data family, and the 2 identifier-lookup
ops. `Nudm_PP` (11 ops, ADR-0082) remains UDM's other open Tier-B item.

