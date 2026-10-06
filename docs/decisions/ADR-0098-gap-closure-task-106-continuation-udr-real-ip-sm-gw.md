## ADR-0098: gap-closure task #106 continuation -- UDR real IP-SM-GW Registration context-data

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md; 12
of free5GC's ~42+ real TS 29.504 resources closed as of ADR-0097). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/context-data/ip-sm-gw` is the
richest context-data resource closed so far -- real `PUT` (`CreateIpSmGwContext`), `GET`
(`QueryIpSmGwContext`), `PATCH` (`ModifyIpSmGwContext`, real `application/json-patch+json` --
RFC 6902, confirmed by reading the YAML directly, same standard `AmfContextStore`'s own patch
already uses, NOT the RFC 7396 merge-patch style `SmPolicyDataStore`/`AmPolicyDataStore` use), and
`DELETE` (`DeleteIpSmGwContext`) -- all four real operations, confirmed per-operation from the
YAML, not assumed uniform across the group.

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_ip_sm_gw_context` table (`ue_id` PK, `context` JSONB).
- `nfs/udr/src/stores.hpp`/`.cpp`: new `IpSmGwContextStore` class (`put`/`get`/`apply_patch`/
  `remove`), combining `AmfContextStore`'s own RFC 6902 `apply_patch` pattern with
  `AuthenticationStatusStore`'s own `remove` pattern -- the first UDR resource in this project
  needing all four real operations together.
- `nfs/udr/src/main.cpp`: four new routes (`PUT`/`GET`/`PATCH`/`DELETE`) at
  `/subscription-data/{ueId}/context-data/ip-sm-gw`, reusing the already-vendored
  `sbi_gen::IpSmGwRegistration` DTO (confirmed by direct grep before writing any application
  code -- no new codegen work needed; every field in the real schema is optional, so an empty-body
  `PUT` is real and spec-valid, not a gap) and two new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl lifecycle against a running `udr` process backed by a real PostgreSQL database: `GET` on
an unseeded `ueId` -> real `404`; `PUT` with a real partial document (`ipsmgwFqdn`,
`unriIndicator`, both real optional fields) -> real `204`; `GET` immediately after -> real `200`
with the identical document; `PATCH` with a real RFC 6902 `replace` operation on `/ipsmgwFqdn` ->
real `204`; `GET` again -> real `200` with the patched value correctly reflected (confirming the
patch was genuinely applied server-side, not just accepted); `DELETE` -> real `204`; `GET` again ->
real `404` -- the full real four-operation lifecycle, all correctly chained.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent already established), zero
regressions.

### What this ADR does NOT include

No NF's own existing logic calls these new routes (same disclosed "surface first, wire consumers
later" precedent already used repeatedly for UDR's own resource-breadth gap-closure). This closes
UDR resource #13 of free5GC's ~42+; roughly 29 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

