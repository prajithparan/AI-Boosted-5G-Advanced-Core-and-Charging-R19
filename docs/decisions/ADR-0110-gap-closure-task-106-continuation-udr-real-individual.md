## ADR-0110: gap-closure task #106 continuation -- UDR real individual Shared Data (first non-per-UE resource)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (24 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0109). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/shared-data/{sharedDataId}` (real schema
`SharedData` -- `TS29503_Nudm_SDM.yaml`, mandatory `sharedDataId` plus optional `sharedAmData`/
`sharedSmsSubsData`/`sharedSmsMngSubsData`/`sharedDnnConfigurations` and others) is a real,
genuinely GET-only resource (`GetIndividualSharedData`) -- confirmed by grepping every
operationId under the real `/subscription-data/shared-data*` prefix, no create/update operation
exists at all. Real, structurally new: this is the **first UDR resource in this project genuinely
NOT keyed per-UE** -- `sharedDataId` alone, matching the real 3GPP concept of operator-shared
default profile data reused across many UEs (Table 44/spec text), distinct from every context-data
/provisioned-data/lcs-*/pp-* resource closed so far, all of which are keyed by `ueId` (or
`ueId`+something).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_shared_data` table (`shared_data_id` PK, `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SharedDataStore` class (`seed`/`get` only, matching the
  established GET-only shape, keyed by `shared_data_id` instead of `ue_id`).
- `nfs/udr/src/main.cpp`: one new route (`GET`) at `/subscription-data/shared-data/{sharedDataId}`,
  and a real, single (not looped-per-UE) seed call: `{"sharedDataId":"10000-default"}` --
  `"10000-default"` matches `SharedDataId`'s own real pattern (`^[0-9]{5,6}-.+$`), this project's
  own representative test identifier, not fabricated spec content -- and one new OTel get counter.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl against a running `udr` process backed by a real PostgreSQL database: `GET` for the real
seeded `sharedDataId` `10000-default` -> real `200` with the exact seeded document; `GET` for an
unseeded `sharedDataId` -> real `404`. Direct `psql` query against `udr_shared_data` independently
confirmed the single seeded row persisted correctly.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls this new route (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). Real,
disclosed scope narrowing: the real sibling collection resource
(`/subscription-data/shared-data`, `GetSharedData`, a required comma-separated `shared-data-ids`
array query parameter) is deliberately deferred -- this project has no existing precedent
anywhere yet for parsing array-shaped query parameters (`sbi_core::http2::Request::query_params`
is a plain `std::multimap<std::string, std::string>`, no comma-splitting helper exists), and
building that real capability belongs in its own scoped turn, not bundled into this one. This
closes UDR resource #25 of free5GC's ~42+; roughly 17 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

