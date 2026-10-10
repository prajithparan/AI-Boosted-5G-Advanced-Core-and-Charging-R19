## ADR-0073: CHF-in-CI (Redis/ClickHouse/its own Postgres) + real full N28 loop closed as an automated test

### Context

ADR-0072 explicitly disclosed, under "What this ADR does NOT include": "CHF-in-CI
(Redis/ClickHouse services in `.github/workflows/ci.yml`) -- a real, pre-existing, separate gap,
not newly introduced here." CHF has never been a participant in this project's automated `ctest`
suite at all -- it needs Redis, ClickHouse, and its own PostgreSQL (`chf_rating`), none of which
`.github/workflows/ci.yml` provisioned (confirmed by direct read, not assumed). As a direct
consequence, the real full N28 loop (UDR seed -> PCF real CHF subscribe -> CHF admin status
change -> real `statusNotification` push -> PCF receipt -> real unsubscribe) that ADR-0072
live-verified manually was never covered by an automated test -- only the fail-open,
CHF-unreachable path was. The user asked to wire up the CHF-in-CI services next; this ADR closes
that gap.

### What changed

**`.github/workflows/ci.yml`** (both the `build` and `sanitize` jobs, identically): three new
`services:` entries mirroring `deploy/docker/docker-compose.yml`'s already-established real
service definitions -- `postgres-chf` (postgres:16-alpine, `POSTGRES_HOST_AUTH_METHOD: trust`,
`POSTGRES_DB: chf_rating`, port 5434), `redis` (redis:7-alpine, port 6379), `clickhouse`
(clickhouse/clickhouse-server:latest, ports 8123+9000). Two new post-checkout steps apply
`nfs/chf/schema.postgres.sql` (via `psql`) and `nfs/chf/schema.clickhouse.sql` (via `curl` against
the HTTP interface) -- GitHub Actions `services:` containers do not support
docker-compose-style `/docker-entrypoint-initdb.d/` auto-init (they start before checkout), so an
explicit apply step is required, the same pattern this project's other Postgres services (UDR,
product-catalog, subscriber-management, roaming-interconnect) already use. Six new `CHF_*` env
vars added to the `Test` step, matching the exact `getenv` names `nfs/chf/src/main.cpp` already
reads (confirmed by direct read, not guessed): `CHF_RATING_DATABASE_URL`, `CHF_REDIS_URL`,
`CHF_CLICKHOUSE_HOST`/`_PORT`/`_USER`/`_PASSWORD`/`_DATABASE`.

**Real ClickHouse-in-CI auth gotcha, re-confirmed by direct testing this session** (already
disclosed as a comment in `deploy/docker/docker-compose.yml` from an earlier session; re-verified
here rather than taken on faith): the official `clickhouse/clickhouse-server` image's shipped
`users.d/default-user.xml` restricts the `default` user to loopback (`127.0.0.1`/`::1`) only --
even with a correctly-set, non-empty `CLICKHOUSE_PASSWORD` -- rejecting any connection from a
genuinely different container. A real, non-empty `CLICKHOUSE_PASSWORD` (this project's own
`chf_clickhouse_lab` for local dev/CI, not a production credential) is what actually lifts the
loopback restriction, not password correctness per se. Separately confirmed and worth recording:
ClickHouse's own `/ping` endpoint does **not** check authentication at all -- `curl -u
wrong:wrong http://host:8123/ping` still returns `"Ok."`, giving a false sense of connectivity;
the CI healthcheck and any manual verification must instead run a real authenticated query
(`SELECT 1`) to actually prove auth works, which is what this ADR's own local verification did
before trusting the CI wiring.

**`tests/integration/CMakeLists.txt`**: `chf` added to `add_dependencies(integration_tests ...)`
and `CHF_PATH="$<TARGET_FILE:chf>"` added alongside the other NF path macros -- CHF was never
spawnable by any integration test before this.

**`tests/integration/test_n28_spending_limit.cpp`**: new
`PcfChfN28Integration.FullLoopSubscribeStatusChangeNotifyUnsubscribe` -- spawns real
nrf+udr+pcf+chf, seeds UDR `SmPolicyData` (`subscSpendingLimits=true`), performs a real
`CreateSMPolicy` at PCF (asserts `pcf_spending_limit_subscribe_total` increments via a real
Prometheus `/metrics` scrape), a real CHF admin `PUT
/chf-admin/v1/policy-counters/{id}` status change (asserts `pcf_spending_limit_notify_total`
increments, proving the real `statusNotification` push->receipt round-trip happened), then a real
`DeleteSMPolicy`. This is the real automated closure of the exact gap ADR-0072 disclosed as
manual-only.

**Real bug found and fixed during this ADR's own test development** (disclosed per this
project's established practice of reporting real bugs found via actual execution, not just
successes): the new test's `scrape_metric_value` helper (a small raw-socket Prometheus scraper,
needed because `sbi_core::http2::Client` is TLS/mTLS-only and cannot hit the deliberately
plain-HTTP `/metrics` endpoint) originally used a bare `body.find(metric_name)`. Prometheus text
exposition format precedes every metric with `# HELP <name> <description>` and `# TYPE <name>
<type>` comment lines whose own free-text also contains the metric name -- the naive search
matched the `# HELP` line first, then tried (and failed) to parse a number out of prose, silently
returning -1 forever. Fixed by anchoring the search to `"\n<metric_name> "` (or `"\n<metric_name>{"`
for a labeled series) so only the real value line matches. Root-caused via direct evidence, not
guessed: the real underlying subscribe/notify/unsubscribe loop was independently confirmed correct
via live process log lines (`pcf: opened real CHF spending-limit subscription...`, `chf: policy
counter ... pushed to 1 subscription(s)`) even while the test's own assertion was failing --
proving the bug was in the test's metric-parsing, not the system under test. Also newly confirmed
this session, informing the fix: OpenTelemetry's Prometheus exporter does not emit an
application-defined counter in `/metrics` at all until it has been incremented at least once (a
freshly-started NF's `/metrics` shows only the exporter's own internal metrics).

### Testing and verification

All three N28 tests (`UdrSmPolicyDataIntegration.*`, `PcfN28Integration.*`,
`PcfChfN28Integration.*`) pass in isolation against local `postgres-chf`/`redis`/`clickhouse`
containers matching the CI service definitions exactly (verified with the corrected UDR
connection string for this local container -- `udr:udr@127.0.0.1:5437/udr`, not `trust`-auth
`postgres@.../5433`, a local-only credential mismatch unrelated to the CI wiring itself, which
uses `POSTGRES_HOST_AUTH_METHOD: trust` for every service consistently). A full local `ctest -j4`
run reached 291/293 before being stopped: the only real failure was the identical, already-disclosed
`SubscriberManagementPostgresTest.SubscriberIsFindableBySupi` test-data-pollution issue flagged in
ADR-0072 (own scope, not touched here); the remaining two tests
(`UdmIntegration.SdmDataRetrievalAndSubscriptions`, `UdrIntegration.AmfContextLifecycle`) hung
without completing -- the identical pre-existing environmental flakiness already disclosed in
ADR-0071/ADR-0072, not a regression from this ADR's changes (this ADR touches only
`.github/workflows/ci.yml`, `tests/integration/CMakeLists.txt`, and
`tests/integration/test_n28_spending_limit.cpp`; none of the hung tests' own files). Every other
test in the suite -- all TAP3, SS7 (M3UA/SCCP/TCAP/MAP/CAP), Diameter, PFCP, NAS, AUSF, AMF, SMF,
UDM, PCF, and BSS-layer tests -- passed cleanly. The CI YAML change itself was validated for
syntactic correctness (`python3 -c "import yaml; yaml.safe_load(...)"`) but has not yet been
exercised by a real GitHub Actions run (that requires pushing this commit).

### What this ADR does NOT include

A fix for `UdrIntegration.AmfContextLifecycle`/`UdmIntegration.SdmDataRetrievalAndSubscriptions`'s
pre-existing environmental flakiness or `SubscriberManagementPostgresTest.SubscriberIsFindableBySupi`'s
test-data-pollution issue -- both real, open, disclosed, out of this ADR's actual scope. Any change
to CHF's own runtime behavior -- this ADR is CI/test-infrastructure wiring only. A real GitHub
Actions run proving the new services/steps work identically to the local reproduction -- disclosed
as not yet exercised, not claimed as verified.

