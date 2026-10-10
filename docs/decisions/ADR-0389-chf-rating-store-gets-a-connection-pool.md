## ADR-0389: CHF rating store gets a connection pool

**Date:** 2026-09-26. **Status:** accepted (follow-up disclosed in ADR-0388). `chf::RatingDecisionStore`
is written on every rating decision; it held ONE `pqxx::connection` behind a mutex, serialising
every CHF worker thread's decision write. It now uses `nf_config::PgPool` (the pool the BSS stores
use), sized by required config `rating_db_pool_size` (`config/chf.json`: 8; env
`CHF_RATING_DB_POOL_SIZE`). `PgPool` exits the process if it cannot connect at startup (fail-closed,
ADR e81fa02), replacing the store's own warn-and-disable path. The MCP server keeps a pool of 1
(read-only lookups). Unchanged: a write that fails mid-run is logged and dropped (the decision is
audit data, not the charge itself) -- disclosed as before.
**Rejected.** *A connection per decision* (connect cost on the hot path). *Async writes via a queue*
-- right for scale, but it changes durability semantics; deferred to the Tier-1 data-layer work.
