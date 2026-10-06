## ADR-0114: gap-closure task #106 continuation -- UDR real policy-data Operator-Specific Data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (28 of free5GC's ~42+ real
TS 29.504 resources closed as of ADR-0113). Real, confirmed-by-YAML-read:
`TS29519_Policy_Data.yaml`'s `/policy-data/ues/{ueId}/operator-specific-data` (real response
shape: a map keyed by operator-specific data element name, values the same real schema
`OperatorSpecificDataContainer` already closed for the `subscription-data`-scoped resource
(ADR-0111) -- reused here via a real cross-file `$ref` back into
`TS29505_Subscription_Data.yaml`) has a real `GET`+`PATCH`-only operation set:
`ReadOperatorSpecificData`, `UpdateOperatorSpecificData` (real `application/json-patch+json`,
RFC 6902) -- confirmed by grepping this exact path (only one block, two operations), no
PUT/DELETE exists for this resource, and no POST/create operation exists either, so (same
disclosed, deliberate precedent already established) `apply_patch` is upsert-capable. Real,
genuinely distinct resource from the `subscription-data`-scoped `operator-specific-data`
(ADR-0111) -- confirmed by the real spec defining two entirely separate operationId pairs
(`ReadOperatorSpecificData`/`UpdateOperatorSpecificData` here vs `QueryOperSpecData`/
`ModifyOperSpecData` there), same "distinct resource, not a rename" precedent already established
repeatedly (AMF/SMSF 3GPP-vs-non-3GPP).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_policy_operator_specific_data` table (`ue_id` PK,
  `data` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `PolicyOperatorSpecificDataStore` class
  (`get`/`apply_patch`), byte-for-byte matching `OperatorSpecificDataStore`'s own upsert-capable
  `apply_patch` pattern.
- `nfs/udr/src/main.cpp`: two new routes (`GET`/`PATCH`) at
  `/policy-data/ues/{ueId}/operator-specific-data` and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PATCH` with a real RFC 6902 `add` operation adding a
`policyFlag` entry -> real `200` with the created document (originating via upsert, no prior
`PUT`/`POST`); `GET` immediately after -> real `200`; `PATCH` again with a real `replace` on
`/policyFlag/value` -> real `200` with the updated document; `GET` again -> real `200` confirming
the update; a direct `GET` on the sibling `subscription-data`-scoped path for the same `ueId`
independently returned real `404`, confirming the two resources are genuinely separate rather than
aliases of the same underlying store. Direct `psql` query against
`udr_policy_operator_specific_data` independently confirmed the persisted document matches the
API's final response.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions (325/325).

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #29 of free5GC's ~42+; roughly 13 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

