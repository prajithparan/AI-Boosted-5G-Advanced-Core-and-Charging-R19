## ADR-0062: `deploy/docker/docker-compose.yml` fixed -- CHF/balance-management never had Dockerfiles, ClickHouse network auth, and a real cross-container connectivity bug

**Date:** 2026-08-15
**Status:** Accepted, live-verified.

**Context:** the compose lab (`deploy/docker/docker-compose.yml`) had not been updated since before
Phase 4 -- it built nrf/amf/smf/udm/udr/ausf/pcf/product-catalog, but CHF and bss/balance-management
had no `Dockerfile` and no compose entry at all, and none of CHF's own real backing stores (Redis,
ClickHouse, its own dedicated PostgreSQL) were present either. The lab could not bring CHF up. Found
while looking for a way to live-verify CAP's own rating chain with product-catalog/balance-
management actually running (ADR-0061's own disclosed gap), not assumed stale from reading the file
alone.

**What was added**: `deploy/docker/chf.Dockerfile` and `deploy/docker/balance-management.Dockerfile`
(mirroring `product-catalog.Dockerfile`'s existing multi-stage shape; `chf.Dockerfile`'s runtime
stage additionally installs `libsctp1`, since `ss7_core` dynamically links the system `libsctp`, not
a vcpkg-built static one). New compose services: `postgres-balance` (bss/balance-management's own
dedicated PostgreSQL, ADR-0056), `postgres-chf` (CHF's own RatingDecision audit PostgreSQL,
ADR-0060 E5), `redis` (CHF's ChargingDataStore, ADR-0055), `clickhouse` (CHF's CDR generation,
ADR-0058), `balance-management`, `chf`. `pki-init`'s NF list extended with `chf`/`balance-
management` so they get real mTLS leaf certs from the same shared lab CA every other NF uses.

**Real bugs found and fixed, each confirmed by an actual failure before being fixed, not assumed**:

1. **ClickHouse network auth.** Even with `CLICKHOUSE_USER=default`/`CLICKHOUSE_PASSWORD=""` set
   explicitly, CHF's connection was rejected with ClickHouse's own generic "Authentication failed"
   error. Root cause found by reading the running container's own shipped
   `users.d/default-user.xml`: `<networks><ip>::1</ip><ip>127.0.0.1</ip></networks>` -- the official
   image restricts the `default` user to loopback-only access regardless of password correctness,
   so a genuinely different container (CHF) is always rejected. Fixed with a real, non-empty
   `CLICKHOUSE_PASSWORD` (which properly provisions the user without the loopback restriction,
   confirmed by re-testing), matching the same `CHF_CLICKHOUSE_PASSWORD` env var
   `chf_clickhouse_options()` (`nfs/chf/src/main.cpp`) already reads -- not a new mechanism.
2. **CHF's own `kProductCatalogBase`/`kBalanceManagementBase` hardcoded to `127.0.0.1`**
   (`nfs/chf/src/charging_engine.hpp`, present since ADR-0047/ADR-0056, long before this ADR).
   Inside a container, `127.0.0.1` is the container itself, not another container -- CHF could
   never reach product-catalog/balance-management once they became separate containers, no matter
   how the network was configured. Real, disclosed pre-existing bug, not something this ADR's own
   new code introduced -- only surfaced because this is the first time anyone actually ran CHF in a
   real multi-container deployment. Fixed the same way as every other CHF connection string
   already in this codebase: `product_catalog_base()`/`balance_management_base()` functions reading
   `CHF_PRODUCT_CATALOG_BASE`/`CHF_BALANCE_MANAGEMENT_BASE`, defaulting to the original
   `127.0.0.1` URLs so same-host (non-containerized) lab runs are unaffected.
3. **SCTP is not a valid Docker `ports:` protocol.** CAP's real port (2905) cannot be host-published
   the way tcp/udp can -- Docker's `ports:` short syntax only accepts `tcp`/`udp`, and SCTP does not
   traverse the userland-proxy/iptables NAT path either protocol uses. Not host-published; disclosed
   in the compose file itself. Container-to-container SCTP on the shared compose bridge network is
   unaffected (confirmed by the live verification below) since that's a direct L3 hop, not NAT'd.

**Live-verified, not just "the config validates"**: brought up every new service via real `docker
compose up`/`build` (not `config`-only). `postgres-balance`/`postgres-chf`/`redis`/`clickhouse` all
reached `healthy`; their real schema files (`bss/balance-management/schema.sql`,
`nfs/chf/schema.postgres.sql`, `nfs/chf/schema.clickhouse.sql`) were confirmed applied by querying
each database directly. `balance-management` and `chf` images built successfully and both
containers started cleanly with real mTLS certs from `pki-init`; CHF's own log confirms all three
real backing-store connections: `connected to Redis/Valkey`, `connected to ClickHouse (CDF)`,
`connected to PostgreSQL (RatingDecision audit, E5)`. A standalone gsmSSF test client run inside a
throwaway container on the same compose network (not host-published, per the SCTP limitation above)
sent a real `InitialDP` to `chf:2905` and received a real `ApplyChargingReport` back -- before the
`kProductCatalogBase` fix, CHF logged `could not reach bss/product-catalog for rating, granting
nothing` (the real bug); after the fix, the identical call logged `no Active/isSellable
ProductOffering with a price found, granting nothing this call` -- proving CHF now genuinely reaches
product-catalog over the real network, the remaining zero-grant result being a real, separate,
disclosed gap (no product-catalog seed data in this lab), not a connectivity failure.

**A related, real discovery while re-running the local `ctest` suite during this same verification**:
orphaned NF processes from earlier interrupted test/lab runs (`amf`, `pcf`, and even a `ctest` run's
own killed child processes) were squatting the same fixed ports (`7777`-`7786`, `9464`-`9474`) both
the Docker containers above AND the native integration-test suite's own `spawn_all()` calls use --
causing a real, misleading `SmfIntegration.CreateSMContextFailsClosedWhenPcfUnreachable` failure that
looked like a regression from this ADR's own changes but was purely port contention. Not new
information (`feedback_manual_lab_processes_pollute_tests` memory already existed for the "manually-
started process" version of this), but broadened and updated with the two new sources found here
(orphaned test-spawned processes; Docker containers on the same fixed ports) since the existing
memory's own guidance (`ps aux | grep nrf|udm|...`) doesn't catch either.

**Not yet done, stated plainly**: no seed data for product-catalog in this lab, so a real non-zero
`GrantedUnit` still cannot be demonstrated end-to-end without manually POSTing a real
`ProductOffering`/`ProductOfferingPrice` first -- a real, separate, disclosed gap (test fixture
content, not infrastructure), not fixed here. The transient `CDR write to ClickHouse failed:
NetException: Timeout exceeded` seen once during repeated restart/rebuild churn was not chased
further (not reproduced on a clean run; CHF's own graceful-degradation path already handles it,
logging and continuing rather than crashing).

No test-suite changes this update (infrastructure-only) -- full rebuild + `ctest` run (after
clearing the port-squatting artifacts above): 234/234 total tests pass, unchanged.

