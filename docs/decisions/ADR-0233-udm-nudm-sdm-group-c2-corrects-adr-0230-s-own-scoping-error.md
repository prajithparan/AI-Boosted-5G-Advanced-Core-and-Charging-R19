## ADR-0233: UDM `Nudm_SDM` group C2 -- corrects ADR-0230's own scoping error

### Context

ADR-0230 scoped group C2 (`GetTimeSyncSubscriptionData`, `GetRangingSlPosData`) as needing
"brand-new UDR-side stores + routes" before UDM could serve either op. Direct investigation this
turn found that claim was **wrong**: `nfs/udr/src/main.cpp` already has both resources fully live
-- `TimeSyncDataStore`/`RangingSlPosDataStore` (real Postgres-backed, `nfs/udr/src/stores.hpp`/
`.cpp`), real `GET /subscription-data/{ueId}/time-sync-data` and
`GET /subscription-data/{ueId}/ranging-slpos-data` routes (`nfs/udr/src/main.cpp`), and real seed
data for both real test SUPIs (`ADR-0131`/`ADR-0136`, already in `schema.postgres.sql` as
`udr_time_sync_data`/`udr_ranging_slpos_data`). This ADR's own scoping mistake is disclosed rather
than silently fixed: while implementing, a first pass mistakenly added duplicate
`CREATE TABLE IF NOT EXISTS` statements for both tables to `schema.postgres.sql` before this was
caught via `git diff` review and reverted -- no duplicate tables were committed.

Confirmed by direct read of `TS29503_Nudm_SDM.yaml`: both real ops (`GET /{supi}/time-sync-data`,
`GET /{supi}/ranging-slpos-data`) are keyed by `supi` alone -- no `servingPlmnId` path segment, no
`plmn-id` query param -- the exact same real shape `GetEcrData` (ADR-0230) already uses.

### Implementation

Refactored `GetEcrData`'s own inline UDR call into a new shared helper,
`fetch_from_udr_no_plmn(ue_id, segment)`, genuinely justified now that 3 real call sites share the
identical shape (not a premature abstraction -- `fetch_from_udr`/`fetch_from_udr_bulk_field` were
each kept as their own distinct helpers since each has a genuinely different URL shape). Two new
routes, `GET /{supi}/time-sync-data` and `GET /{supi}/ranging-slpos-data`, added to
`nfs/udm/src/main.cpp`'s Nudm_SDM section, both using the new shared helper against UDR's own
already-live `time-sync-data`/`ranging-slpos-data` segments.

### Live verification (real, live mTLS + OAuth2, not self-consistency)

New `tests/integration/test_udm_sdm_gap_closure_233.cpp`, 1 test (spawns real `nrf`+`udr`+`udm`):
`TimeSyncAndRangingSlPosReturnRealUdrSeededData` -- real `200` with the exact seeded
`afReqAuthorizations.gptpAllowedInfoList[0].gptpAllowed`/`serviceIds[0].reference` and
`rangingSlPosAuth.rgSlPosPc5Auth` for the seeded SUPI on both paths; real `404` on both for an
unseeded SUPI. Also re-ran `UdmSdmGapClosure230Integration.*` (covers the refactored `GetEcrData`)
to confirm the helper extraction introduced no regression -- still passes.

Passes.

### Testing

Full reconfigure + rebuild clean. Both the new test and the existing `GetEcrData` test pass
individually. Full suite re-run clean at `-j1` (455/455, 6 legitimately skipped for missing local
Postgres env vars, same as always). No strays before or after any run (`ps aux` checked
explicitly).

### What this ADR does NOT include

This closes `Nudm_SDM`'s entire group C in full (C1 + C2), completing the whole group-C
investigation ADR-0228 originally opened. Still open: 5 SOR/UPU/ack write ops, a 6-op shared-data
family, 2 identifier-lookup ops, and `Nudm_PP` (11 ops, ADR-0082).

