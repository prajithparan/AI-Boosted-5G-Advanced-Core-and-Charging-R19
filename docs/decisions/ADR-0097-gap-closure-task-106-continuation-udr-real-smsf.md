## ADR-0097: gap-closure task #106 continuation -- UDR real SMSF Registration context-data (3GPP + non-3GPP access)

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md;
10 of free5GC's ~42+ real TS 29.504 resources closed as of ADR-0094). free5GC's real UDR treats
SMSF 3GPP-access and non-3GPP-access registration as two distinct resources, matching the same
real pattern already closed for AMF context-data (ADR-0093). Real, confirmed-by-YAML-read:
`TS29505_Subscription_Data.yaml`'s `/subscription-data/{ueId}/context-data/smsf-3gpp-access` and
`.../smsf-non-3gpp-access` are two genuinely separate real paths/operationIds
(`CreateSmsfContext3gpp`/`QuerySmsfContext3gpp`/`DeleteSmsfContext3gpp` vs. the `Non3gpp` triple),
even though -- unlike the AMF pair -- both real resources share the IDENTICAL real schema
(`SmsfRegistration`). Real, confirmed: GET+PUT+DELETE (no PATCH), matching
`authentication-status`'s own operation shape more closely than `amf-3gpp-access`'s GET+PUT-only.

### Implementation

- `nfs/udr/schema.postgres.sql`: two new tables, `udr_smsf_3gpp_context` and
  `udr_smsf_non3gpp_context` (`ue_id` PK, `data` JSONB each) -- deliberately separate tables for
  the identical real reason `udr_amf_non3gpp_context`'s own comment already gives: two real,
  distinct spec resources, not a shared document, even when the schema happens to be identical.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `SmsfContext3gppStore`/`SmsfNon3GppContextStore` classes
  (`put`/`get`/`remove`), matching `AuthenticationStatusStore`'s own GET+PUT+DELETE shape exactly
  -- deliberately two separate classes, not templated on table name, matching this file's own
  established one-class-per-real-resource convention.
- `nfs/udr/src/main.cpp`: six new routes (`GET`/`PUT`/`DELETE` × 2 resources) at
  `/subscription-data/{ueId}/context-data/smsf-3gpp-access` and `.../smsf-non-3gpp-access`
  (`QuerySmsfContext3gpp`/`CreateSmsfContext3gpp`/`DeleteSmsfContext3gpp` and their non-3GPP
  counterparts), reusing the already-vendored `sbi_gen::SmsfRegistration` DTO (already generated
  from the R19 YAML, confirmed by direct grep before writing any application code -- no new
  codegen work needed) and four new OTel counters.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl round-trip against a running `udr` process backed by a real PostgreSQL database, for
`smsf-3gpp-access`: `GET` on an unseeded `ueId` → real `404`; `PUT` with the real mandatory field
set (`smsfInstanceId`, `plmnId` -- both mandatory per the already-generated DTO, `plmnId` itself
requiring nested `mcc`/`mnc`) → real `204`; `GET` immediately after → real `200` with the
identical document; `DELETE` → real `204`; `GET` again → real `404` (full real CRUD lifecycle, all
four states). Separately confirmed the two resources are genuinely independent, not accidentally
sharing storage despite the identical schema: `PUT` on `smsf-3gpp-access` with
`smsfInstanceId="aaaa0000..."` and `PUT` on `smsf-non-3gpp-access` with a different
`smsfInstanceId="bbbb1111..."` for the SAME `ueId`, then `GET` on both -- each returned its own,
distinct value, not the other's. Independently confirmed via a direct `psql` query against both
`udr_smsf_3gpp_context` and `udr_smsf_non3gpp_context` -- two separate real rows, genuinely
persisted in PostgreSQL, not held only in process memory. Test data cleaned up after verification.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass, same disclosed manual-live-verification precedent ADR-0090/ADR-0091/ADR-0092/ADR-0094
already established), zero regressions.

### What this ADR does NOT include

No NF's own existing SMSF-adjacent logic (there is none yet in this project -- SMSF itself is a
whole, separate, not-yet-built NF, Tier 2 per CLAUDE.md's own scope list) calls these new routes;
same disclosed "surface first, wire consumers in a dedicated later turn" precedent already used
for `provisioned-data` (ADR-0069) and the AMF context resources (ADR-0093). This closes UDR
resources #11-12 of free5GC's ~42+; roughly 30 remain a real, open, disclosed gap
(docs/CAPABILITY_GAP_ANALYSIS.md). Task #106 remains open (not fully closed).

