## ADR-0449: `PgPool` hardening -- bounded `acquire()`, `PoolExhaustedError`, real 503s

ADR-0445's known-debt list named this directly: "that shared `PgPool` itself blocks forever on
exhaustion (no deadline/backpressure/metrics)." Confirmed in the actual source
(`libs/nf-config/include/nf_config/pg_pool.hpp`) before touching anything: `acquire()` called
`cv_.wait(lk, [this] { return !free_.empty(); })` with no bound at all -- a fully checked-out pool
hung the calling thread indefinitely, with nothing anywhere to show it had happened.

**Scope-finding before committing to a design.** `PgPool` has ~50 call sites across 5 classes in
5 files (`bss/balance-management/src/store.cpp`, `bss/product-catalog/src/store.cpp`,
`bss/provisioning/src/provisioning_store.cpp`, `gui/bff/src/iam.cpp`,
`nfs/chf/src/rating_decision_store.cpp`). The obvious-looking fix -- change `acquire()` to return
`std::optional<Lease>` -- would have forced every one of those ~50 call sites to grow new
error-handling, several of them widening their own function's return type just to carry the
failure through (most are bare `std::optional<T>`/`std::vector<T>`/value-type returns today, with
no existing error channel, and `std::nullopt` from "pool exhausted" is not the same real outcome as
`std::nullopt` from "legitimately not found" -- conflating them would misreport a 503-shaped
problem as a 404-shaped one). That is a large, multi-file, genuinely risky refactor to rush, and was
set aside as not what this increment needed to be.

**Design actually used: bounded wait, throw on exhaustion, use the exception boundary this codebase
already has.** `acquire()` keeps its original call-site shape (`auto lease = pool_.acquire(); pqxx::
work txn(lease.conn());` -- unchanged at all ~50 sites) but now does `cv_.wait_for(lk,
acquire_timeout_, pred)`; on timeout it throws a new `nf_config::PoolExhaustedError` (a
`std::runtime_error`) instead of returning. This is safe, not reckless, because of a real
call-site audit done before relying on it:

- `nfs/chf/src/rating_decision_store.cpp`'s two methods (`find_by_charging_data_ref`, `record`)
  each already wrap their own `pool_->acquire()` call in a local `try { ... } catch (const
  std::exception& e) { spdlog::warn(...); }`, with an existing comment stating the discipline
  explicitly: "best-effort... must never block or fail the real charging response CHF already
  committed to." A thrown `PoolExhaustedError` lands in that existing catch, exactly as any other
  pqxx exception from that same call already did -- no new code needed here at all. This matters
  specifically because this store is the one `PgPool` consumer reachable from CHF's Diameter
  connection thread (`diameter_server.cpp`'s `handle_connection`), which has **no** top-level
  exception handler of its own (confirmed by inspection -- unlike `cap_server.cpp`, which does) --
  an exception that escaped uncaught there would call `std::terminate()` and take down the whole
  CHF process, not just one request. Throwing was only safe here because this specific backstop
  was checked to already exist, not assumed.
- Every other call site (`balance-management`, `product-catalog`, `provisioning`, `gui/bff/iam`) is
  reachable only from inside an HTTP route handler. `libs/sbi-core/src/http2_server.cpp` already
  has a generic handler-exception boundary (ADR-0360): any uncaught `std::exception` from a handler
  becomes a TS 29.500 ProblemDetails 500 for that one request, not a crashed process. Widened here
  with a `catch (const nf_config::PoolExhaustedError&)` placed *before* the generic
  `catch (const std::exception&)` (derived-class catch must precede the base-class one), answering
  a real 503 ("Service Unavailable", `cause: SYSTEM_FAILURE` per TS 29.500 Table 5.2.7.2-1) instead
  of the generic 500 -- a caller sees "back off and retry," the materially correct signal for a
  transient capacity condition, not "something is broken."

**Timeout default, disclosed as a real decision, not hidden.** `acquire_timeout` is a trailing
constructor parameter defaulting to 5000ms, not a new required parameter threaded through the ~10
existing `PgPool`-constructing call sites (and their config files/JSON schemas). Reasoning: this is
a library-level safety bound against a pathological condition (full exhaustion), not a
per-deployment tuning knob any operator has asked to set differently per NF yet -- adding config
plumbing for a value nobody has needed to vary yet is exactly the speculative abstraction
`CLAUDE.md` says not to build. Every existing two-argument `PgPool(conninfo, size)` call compiles
unchanged. Extend to a configured value, per NF, when a real operator need for a different bound
actually shows up.

**Metrics.** `PgPool::in_use()` (locked point-in-time snapshot: `conns_.size() - free_.size()`) and
`PgPool::exhaustion_count()` (lock-free atomic counter, incremented on every timeout) are now real,
queryable accessors on the pool itself -- the other half of ADR-0445's "no deadline/backpressure/
**metrics**" complaint. Not yet wired into an OpenTelemetry gauge in any of the 5 consuming NFs'
`main.cpp` (that's 5 more files, each needing its own `AddCallback`/`static_cast<PgPool*>(state)`
boilerplate, the same pattern `catalog_snapshot`'s gauges in `nfs/chf/src/main.cpp` already use) --
disclosed as the one piece of ADR-0445's debt list this increment leaves genuinely open, not quietly
dropped.

**Build-graph consequence, found and disclosed, not hidden.** `sbi_core` did not previously depend
on `nf_config`; it now does (`PUBLIC`, for the `PoolExhaustedError` catch in `http2_server.cpp`).
`nf_config` is the lower-level, header-only, dependency-free of the two (confirmed before linking
it the other direction), so this is acyclic -- but `add_subdirectory(libs/nf-config)` had to move
*before* `add_subdirectory(libs/sbi-core)` in the top-level `CMakeLists.txt` (it was previously
after, line 75 of ~900; `sbi_core`'s own `target_link_libraries` would otherwise reference a target
that doesn't exist yet at configure time). A `PUBLIC` link means `nf_config`'s usage requirements
propagate to every target that links `sbi_core` -- which, confirmed empirically rather than assumed,
forced a one-time full rebuild of `sbi_generated` (247 generated OpenAPI DTO files) and the entire
`integration_tests` binary (~100+ test files), since their own compile command lines changed even
though no header content they `#include` directly changed. A real, disclosed one-time build cost
from this change, not a hidden one.

**Verification.** `chf`, `sbi_core`, `product-catalog`, `oam-gui-bff`, `provisioning`, and
`integration_tests` all rebuilt clean from the reordered `CMakeLists.txt` with zero new warnings.
Regression run, `./build/tests/integration/integration_tests --gtest_filter=
"ChfProtocolCeilings.*:CapScopedCharging.*:ChaosCharging.*:BalanceLossless.*:ChfDiameterRar.*"`
(13+2 tests spanning every `PgPool`-adjacent path this change touches: CHF's rating-decision audit
store, balance-management's reserve/release ledger, and the RAR/RAA work from ADR-0448) -- first
attempt hit a real, repeated collision with a concurrently-running CI job's own `sanitize (tsan)`
job on this shared machine's loopback ports (the exact, already-documented
`feedback_ci_tests_collide_with_local` pattern, confirmed via `gh run view` showing that job's
`Test` step actively running and a live `nrf` process from the runner's own checkout path on port
7777 at the same moment), not a real regression -- that CI run (37091533936) ran unusually long
(~90 minutes across its `build`/`sanitize (tsan)`/`sanitize (asan-ubsan)` jobs, each serialized on
the one shared self-hosted runner, each re-occupying the same ports during its own `Test` step),
so clearing it took several real wait cycles rather than one. Its own `Stop containers` step also
tore down the shared local `docker compose` project (`postgres-chf`, `valkey`, `postgres-udr`,
`doris`) once it finally finished, which the first post-CI retry caught as a second, unrelated
failure (`FATAL: PostgreSQL connection pool could not connect at startup... port 5434... Connection
refused`) -- a real local-environment side effect of sharing one machine with CI, not a `PgPool`
bug; fixed by `docker compose up -d postgres-chf valkey postgres-udr doris` and waiting for health.
With both now actually clear: **15/15 tests passed** (`BalanceLossless` x7, `ChfProtocolCeilings`
x3, `ChaosCharging` x2, `CapScopedCharging` x1, `ChfDiameterRar` x2), confirming no regression from
the bounded-`acquire()`/`PoolExhaustedError`/CMake-reorder change across every path it touches.

