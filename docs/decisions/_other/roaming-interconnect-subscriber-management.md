## roaming-interconnect, subscriber-management

### Context

ADR-0084's own verification pass found this session's five Postgres-backed services besides
`bss/product-catalog` all hardcode a `localhost:5432` fallback that no longer matches their real
container's host-mapped port (`docker ps`: udr on 5437, chf on 5434, balance-management on 5433,
roaming-interconnect on 5436, subscriber-management on 5435) -- confirmed as a live, reproducing
bug, not a style concern (`UdrIntegration.SmfRegistrationLifecycle`:
`pqxx::broken_connection ... password authentication failed for user "udr"`). This closes that
batch of task #109 (ADR-0077's own standing "no hardcoded DB URL/config literal in source" rule),
following the exact retrofit shape ADR-0077 established for AMF: `nf_config::load`/`require` in
`main()`, a checked-in `config/<service>.json` holding the real default, an env var of the form
`<SERVICE>_<KEY>` still available to override at deployment time.

### Scope: 5 services, deliberately not identical depth

- **UDR**: full retrofit -- `port`/`metrics_bind_address`/`nrf_base_url`/`database_url`, all four
  fields that existed as hardcoded literals. `config/udr.json`'s `database_url` default now reads
  `localhost:5437` (the real, permanent host-side mapping `deploy/docker/docker-compose.yml`'s own
  `postgres-udr` service explicitly assigns, not a guess). New `UDR_NRF_BASE_URL` compose env
  override added (`https://nrf:7777`) -- the same real cross-container-loopback bug AMF's own
  `AMF_NRF_BASE_URL` fix (ADR-0077) already found and fixed, confirmed present here too by the same
  reasoning (127.0.0.1 inside the udr container is udr's own loopback, not nrf's), not yet actually
  broken only because UDR's `run_nrf_lifecycle` registration itself doesn't yet get exercised by
  the currently-failing-anyway compose stack's own integration path -- fixed proactively rather
  than waiting for a second live-reproduction.
- **balance-management, roaming-interconnect, subscriber-management** (bss/* TM Forum services,
  no NRF registration -- confirmed by grep, none of the three reference `kNrfBase`/NRF at all):
  full retrofit -- `port`/`metrics_bind_address`/`self_base_url`/`database_url`. `self_base_url`
  (embedded in each service's own response payloads as resource `href`/`id` prefixes) included
  even though nothing in this compose stack currently dials it cross-container -- same real
  loopback-inside-container class of bug as the NRF base URL fixes, fixed on the same principle
  rather than waiting for a live consumer to expose it. New `*_SELF_BASE_URL` compose env
  overrides added for all three, pointing at each service's own compose DNS name.
- **CHF**: **partial** retrofit, real and disclosed, not silently narrower --
  `port`/`metrics_bind_address`/`nrf_base_url`/`rating_database_url` only (the field ADR-0084's own
  verification actually found broken, plus the three companion fields sharing the exact same
  `kPort`/`kMetricsBindAddress`/`kNrfBase`/`run_nrf_lifecycle` shape every other retrofit in this
  batch used). `chf_redis_conninfo`, `chf_clickhouse_options` (5 separate getenv calls),
  `CHF_AI_QUOTA_SIZING_ENABLED`, and `CHF_QUOTA_MODEL_PATH` are explicitly NOT touched this pass --
  each already has its own env-var override (so already deployment-safe, just missing the
  config-file-default layer ADR-0077 also wants), and CHF's own config surface is large enough
  (7+ more getenv call sites) to be its own future increment rather than folding it into this
  already-5-service batch. Disclosed in a code comment at `nfs/chf/src/main.cpp`'s own
  `nf_config.hpp` include, not left implicit.

### Verification

All five rebuilt clean (`cmake --build . --target udr chf balance-management
roaming-interconnect subscriber-management -j4`). Live-started all five standalone (plus `nrf` for
udr/chf's own registration) with **zero environment variable overrides** -- every one connected to
its real Postgres container and bound its real port purely from its own `config/<service>.json`
default, confirmed via each process's own independently-generated startup log (`"connected to
PostgreSQL"`, `"registered with NRF (HTTP 201)"`, `"listening on https://0.0.0.0:<port>"`). Live
HTTP spot-check over real mTLS: UDR's `authentication-status` GET returned a real 404 (no data
seeded, expected); balance-management's `bucket` GET returned a real 200 (proves genuine
end-to-end Postgres connectivity through the new config path, not just a successful TCP connect).
Full `conformance_tests` re-run twice -- once with the five old `*_DATABASE_URL` overrides still
set (310/310), once with all five explicitly unset via `env -u` (also 310/310, zero regressions,
proving the new config-file defaults are self-sufficient without any override).

### What this ADR does NOT include

`bss/product-catalog` (not touched -- its own hardcoded `localhost:5432` default happens to
already be correct, since it was the first Postgres container created and got the real 5432
mapping before the others existed; still real hardcoded-literal style debt against ADR-0077's own
rule, left for a future turn since it isn't an active bug). CHF's remaining Redis/ClickHouse/AI-env
fields (see above). `nfs/ausf` was already retrofitted incidentally in ADR-0081 (its first new
Redis dependency), not part of this batch. `nfs/nrf`, `nfs/pcf`, `nfs/smf`, `nfs/udm`, `nfs/upf`,
`nfs/hello-nf` -- still fully untouched, task #109's own remaining backlog, tracked to continue in
future batches at this same project's established "one subsystem (or small batch) per turn" pace.

