# Charging data-plane consistency/performance (ADR-0445, 2026-10-02)

Moved verbatim from CLAUDE.md (section "Charging data-plane..."); see `docs/optimization/MOVE_LOG.md`.

- An external architecture review (not an Anthropic/Claude model; see
  `docs/DECISIONS.md` ADR-0445 for provenance) of CHF/UDR/balance-management's
  persistence and performance was independently verified fact-by-fact against
  HEAD before anything in it was acted on — every claim checked out true.
  Decision: PostgreSQL stays authoritative for balance/ledger (ADR-0445's
  "Option 1"); Valkey carries only non-monetary, rebuildable session state.
  Rejected for now, revisit only with measured evidence: moving balance
  authority into Valkey (ADR-0445's Options 2/3).
- New known debt this surfaced, tracked in ADR-0445, not yet fixed: CHF's
  per-session reserved-total (`ChargingDataStore::get_reserved_total`) lives
  **only** in Valkey — PostgreSQL's `reserve_balance` ledger has no
  `chargingDataRef` correlator, so it is not currently a rebuildable
  projection; CHF rating does an N+1 SBI fetch per candidate
  `ProductOfferingPrice`; Release/CCR-T/CAP settlement is unreserve-then-debit
  as two independent calls, not one atomic operation; money is `double` in
  C++ despite `NUMERIC` columns; UDR opens one PostgreSQL connection per
  store object (~80, no pool) instead of using the `PgPool` CHF/BSS already
  share; that shared `PgPool` itself blocks forever on exhaustion (no
  deadline/backpressure/metrics); the 6-node Valkey Cluster (ADR-0443) has
  AOF disabled and no NF points at it by default; no PostgreSQL
  replication/failover in Compose; no migration/rollback framework for
  domain DDL beyond first-init-only `docker-entrypoint-initdb.d` scripts.
- Zero benchmark data exists for the charging data plane specifically
  (distinct from ADR-0329's narrow NRF-only benchmark, which this is not).
  No performance/HA claim about CHF may be made until ADR-0445's baseline
  plan runs.
