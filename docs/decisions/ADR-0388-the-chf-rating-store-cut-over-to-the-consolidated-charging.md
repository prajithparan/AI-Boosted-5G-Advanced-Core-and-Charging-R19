## ADR-0388: the CHF rating store cut over to the consolidated charging DB -- the consolidation is complete

**Date:** 2026-09-26. **Status:** accepted (user: "Please proceed fast"). Last per-service store onto
`charging` (after ADR-0384..0387).

**Decision.** `chf::RatingDecisionStore` writes schema `chf_rating` of `charging`: each decision
into the time-partitioned `chf_rating.rating_decision` with `charging_data_ref` and
`subscriber_identifier` (SUPI) as first-class columns (per-customer inquiry at Tier-1 volume; the
lookup by chargingDataRef now uses `idx_rating_ref` instead of a JSONB expression), and its TMF678
AppliedCustomerBillingRate as a row of `chf_rating.applied_customer_billing_rate` sharing the id.
New idempotent `61-rating-lossless.sql` adds what the CHF records but `60-rating.sql` dropped: tariff
id + pinned version, rule fired, AI advisory; unbounded NUMERIC amounts (was 4 decimals). The single
writer (`charging_engine.cpp write_rating_decision`) now passes the SUPI. Audit rows use the
`chf_rating.audit_record` shape (actor/action/detail with entityType/entityId/afterSnapshot/
aiAdvisoryRef). Config (chf, mcp-server), compose, CI (`CHF_RATING_DATABASE_URL`,
`MCP_RATING_DATABASE_URL`), the CAP test's direct query and `scripts/pipeline-clean.sh` ->
`charging`/`chf_rating`.

**Found and fixed.** `config/mcp-server.json` pointed its rating store at the UDR's Postgres
(`udr:udr@...:5437/chf_rating`), so the MCP rating-decision tool could not have worked locally.

**Disclosed.** The store is still ONE connection behind a mutex and is written on every rating
decision -- a throughput limit on the CHF hot path (pre-existing; a pool is the follow-up before any
scale run). `taxExcluded == taxIncluded` (no tax engine) as before. The old `chf_rating` database
and CI's step applying `nfs/chf/schema.postgres.sql` to it are now unused (retire with the other
per-service DBs). **With this, every charging/BSS service persists in the consolidated `charging`
DB** (+ `orchestration`); UDR and ADRF keep their own DBs by design.

**Tests.** Locally 2026-09-26: CapScopedCharging (the CHF rates an InitialDP and the test finds the
decision in `chf_rating.rating_decision` of `charging`), plus ProductCatalogLossless, BalanceLossless
-- 10/10; SQL check: decision row carries chargingDataRef, SUPI, tariff id; ACBR and audit rows
written.

