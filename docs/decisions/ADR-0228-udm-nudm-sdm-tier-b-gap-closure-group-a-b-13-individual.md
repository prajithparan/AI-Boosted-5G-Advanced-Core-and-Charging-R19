## ADR-0228: UDM `Nudm_SDM` Tier-B gap-closure -- group A+B (13 individual-resource GET ops)

### Context

`Nudm_UECM` fully closed (ADR-0227) left `Nudm_SDM` as UDM's largest remaining Tier-B item.
Re-scoped in real detail against `specs/5G_APIs-REL-19/TS29503_Nudm_SDM.yaml` directly (37
operationIds confirmed by grep, each one's own real path/parameters/response schema then read in
full) rather than relying on the earlier rough "~33" estimate carried in
`docs/CAPABILITY_GAP_ANALYSIS.md`. Of the 37: 3 already wired (`GetAmData`/`GetSmfSelData`/
`GetSmData`, ADR-0069). Of the remaining 34, real data splits into three groups:

- **Group A** (4 ops): `GetSmsData` (`GET /{supi}/sms-data`), `GetSmsMngtData`
  (`GET /{supi}/sms-mng-data`), `GetTraceConfigData` (`GET /{supi}/trace-data`), `GetLcsBcaData`
  (`GET /{supi}/lcs-bca-data`) -- each has a real individual UDR REST route of its own
  (`nfs/udr/src/main.cpp`'s `provisioned_data_path_pattern + "/<segment>"`, ADR-0106/ADR-0125/
  ADR-0126/ADR-0127), mechanically identical shape to the 3 already-wired ops.
- **Group B** (9 ops): `GetLcsPrivacyData` (`GET /{ueId}/lcs-privacy-data`), `GetLcsMoData`
  (`GET /{supi}/lcs-mo-data`), `GetLcsSubscriptionData` (`GET /{supi}/lcs-subscription-data`),
  `GetV2xData` (`GET /{supi}/v2x-data`), `GetProseData` (`GET /{supi}/prose-data`), `GetMbsData`
  (`GET /{supi}/5mbs-data`), `GetUcData` (`GET /{supi}/uc-data`), `GetA2xData`
  (`GET /{supi}/a2x-data`), `GetRangingSlPrivacyData` (`GET /{ueId}/rangingsl-privacy-data`) --
  each op's own real response schema (`LcsPrivacyData`/`LcsMoData`/etc) maps 1:1 to a field UDR's
  own bulk `ProvisionedDataSets` aggregate (`QueryProvisionedData`, ADR-0212) already composes
  (`lcsPrivacyData`/`lcsMoData`/`lcsSubscriptionData`/`v2xData`/`proseData`/`mbsSubscriptionData`/
  `ucData`/`a2xData`/`rangingSlPrivacyData`, confirmed by direct read of both
  `TS29505_Subscription_Data.yaml`'s `ProvisionedDataSets` schema and UDR's own aggregate-composing
  handler), but none of these 9 has an individual UDR REST route of its own -- only the bulk
  aggregate carries the data.
- **Group C** (~7 ops, deferred, not touched by this ADR): `GetNSSAI`, `GetUeCtxInAmfData`,
  `GetUeCtxInSmfData`, `GetUeCtxInSmsfData`, `GetEcrData`, `GetTimeSyncSubscriptionData`,
  `GetRangingSlPosData` -- real data source not yet confirmed (may be UDM-local computed data, may
  need genuinely new UDR-side work); needs its own investigation turn.

Also confirmed still open, untouched by this ADR: `Subscribe`/`Unsubscribe`/`Modify` completion for
SDM subscriptions (partial CRUD already exists), 5 SOR/UPU/ack write ops
(`am-data/sor-ack`/`am-data/upu-ack`/`am-data/subscribed-snssais-ack`/`am-data/cag-ack`/
`am-data/update-sor`), a 6-op non-per-UE shared-data family (`GetSharedData`/
`SubscribeToSharedData`/`UnsubscribeForSharedData`/`ModifySharedDataSubs`/`GetIndividualSharedData`/
`GetGroupIdentifiers`), and 2 identifier-lookup ops (`GetSupiOrGpsi`/`GetMultipleIdentifiers`).

User-approved slicing: close groups A and B together in this single turn (13 ops), leaving group C
and the remaining unscoped families for later turns.

### Implementation

- **Group A**: identical shape to `fetch_from_udr`'s existing 3 callers (`GetAmData`/
  `GetSmfSelData`/`GetSmData`) -- 4 new routes, each calling `fetch_from_udr(id,
  resolve_serving_plmn_id(req), "<segment>")` with the real UDR segment name
  (`sms-data`/`sms-mng-data`/`trace-data`/`lcs-bca-data`). No new helper needed.
- **Group B**: a new shared helper, `fetch_from_udr_bulk_field`, genuinely distinct from
  `fetch_from_udr` -- calls UDR's bulk `{kUdrApiRoot}/subscription-data/{ueId}/{servingPlmnId}/
  provisioned-data` route (no segment suffix) once, parses the returned `ProvisionedDataSets` JSON,
  and extracts one named field. Real, disclosed: UDR's bulk route always returns `200` (ADR-0212 --
  a live view), so the real per-op `404` for a UE with no data for that specific field is produced
  locally in this new helper (`!parsed.contains(field_name)`), not by UDR.
- **Real, disclosed spec quirk confirmed by direct read**: none of the 9 group-B operations expose
  a `plmn-id` query parameter in `TS29503_Nudm_SDM.yaml` (unlike `am-data`/`sm-data`/`lcs-bca-data`/
  etc, which do), yet UDR's bulk aggregate route requires `servingPlmnId` as a path parameter. This
  project's existing `resolve_serving_plmn_id` helper already falls back to
  `kDefaultServingPlmnId` when no `plmn-id` query param is present (used elsewhere in this file for
  other plmn-id-less callers) -- reused here rather than inventing new special-casing, consistent
  with this lab's already-established single-PLMN scope.
- **Real path-parameter-name fidelity**: `GetLcsPrivacyData`/`GetRangingSlPrivacyData` use a real
  `{ueId}` path parameter (`VarUeId`) in the spec, genuinely different from the other 11 ops' real
  `{supi}` parameter -- both route patterns are registered exactly as the spec defines them, not
  normalized to a single naming convention project-wide.
- **Real, disclosed simplification**: `GetUcData`'s real optional `uc-purpose` query parameter (a
  filter over `UcSubscriptionData`'s own per-purpose entries) is accepted-but-unhonored -- same
  established precedent UDR's own bulk aggregate route already documents for its other real,
  optional, unhonored filter params (`single-nssai`/`dnn`/`adjacent-plmns`/`ext-group-ids`).
- Extended the existing `udm_sdm_data_get_total` counter's description to cover all 16 real
  individual-resource GET ops it now backs (was documented as covering only the original 3).

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_sdm_gap_closure_228.cpp`, 2 tests (spawns real `nrf`+`udr`+`udm`
processes, same 3-process shape `test_udm_uecm_sdm.cpp`'s own `SdmDataRetrievalAndSubscriptions`
already uses):

- `GroupAIndividualUdrRoutesReturnRealSeededData`: real `200` + exact seeded field values
  (`smsSubscribed`/`mtSmsSubscribed`/`traceRef`/`locationAssistanceType`) for the real seeded test
  SUPI (`imsi-999700000000001`, UDR's own existing seed data, ADR-0069/ADR-0106/ADR-0125/ADR-0126/
  ADR-0127) on all 4 group-A paths; real `404` on all 4 for a genuinely unseeded SUPI.
- `GroupBBulkExtractedFieldsReturnRealSeededData`: real `200` (with a field-value spot-check on
  `GetLcsPrivacyData`'s `lpi.locationPrivacyInd`) for the same seeded SUPI on all 9 group-B paths;
  real `404` on all 9 for an unseeded SUPI -- proving the per-field-absence `404` genuinely lives in
  UDM's own new `fetch_from_udr_bulk_field` helper, since UDR's bulk aggregate route itself never
  `404`s.

Both pass.

### Testing

Full reconfigure + rebuild clean. Both new tests pass individually. Full suite re-run clean (see
commit for exact pass count). No strays before or after any run (`ps aux` checked explicitly).

### What this ADR does NOT include

Group C's ~7 ops (real data source unconfirmed), `Subscribe`/`Unsubscribe`/`Modify` completion, the
5 SOR/UPU/ack write ops, the 6-op shared-data family, and the 2 identifier-lookup ops -- all still
open, all deferred to later turns. `Nudm_PP` (11 ops, ADR-0082) remains UDM's other open Tier-B
item.

