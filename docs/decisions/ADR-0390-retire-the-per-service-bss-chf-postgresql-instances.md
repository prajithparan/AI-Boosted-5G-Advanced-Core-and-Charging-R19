## ADR-0390: retire the per-service BSS/CHF PostgreSQL instances

**Date:** 2026-09-26. **Status:** accepted (follows the completed consolidation, ADR-0384..0388).
Nothing reads or writes the old per-service databases any more, so compose loses the `postgres`
(product-catalog), `postgres-balance`, `postgres-subscriber` and `postgres-roaming` services and
their volumes, `postgres-chf` stops loading `nfs/chf/schema.postgres.sql` into its legacy
`chf_rating` database, and both CI jobs lose the four matching service containers and the five
schema-apply steps -- 234 lines, four fewer PostgreSQL containers per CI job on the single
self-hosted runner (whose memory pressure has been killing background work). Kept on purpose:
`postgres-chf` (hosts `charging` + `orchestration`), `postgres-udr`, `postgres-adrf`.
The legacy `bss/*/schema.sql` and `nfs/chf/schema.postgres.sql` files stay in the tree as history
(referenced by earlier ADRs); nothing applies them. **Rejected:** keeping the containers "just in
case" -- they would silently mask any regression back to a per-service URL.
