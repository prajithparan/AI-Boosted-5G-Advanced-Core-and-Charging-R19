## ADR-0450: Diameter/CAP connection-thread exception safety + PgPool metrics

Two real, related pieces of follow-up found while finishing ADR-0449, both closing debt rather than
adding anything speculative.

**Piece 1: Diameter and CAP connections had no exception boundary of their own.** Found while
confirming ADR-0449's throw-on-exhaustion design was safe for CHF's Diameter path: `DiameterServer::
accept_loop` (`nfs/chf/src/diameter_server.cpp`) spawns `handle_connection` -- ~775 lines of real
protocol logic (`charging_engine`, `rating_decision_store`, HTTP clients to product-catalog/
balance-management) -- on its own detached thread with no `try`/`catch` anywhere around it. An
uncaught exception from any of that previously called `std::terminate()` and killed the **entire
CHF process** over one connection's one bad message, not just that connection -- a materially worse
failure mode than the "answer this one request with an error" boundary every HTTP route already has
(ADR-0360).

**Correction to something said earlier in this session:** CAP was believed safe because
`CapServer::accept_loop` (`cap_server.cpp`) has its own `try`/`catch`. Re-checked properly this time:
that `try`/`catch` only ever wrapped `listener_.accept()` itself (SCTP's own throwing API, unlike
Boost.Asio TCP's `error_code` style) -- the `std::thread(&CapServer::handle_connection, ...)
.detach()` call sits *inside* that `try`, but detaching starts an independent thread stack that the
calling frame's `catch` cannot see once control returns. `CapServer::handle_connection` has zero
`try`/`catch` of its own (confirmed by grep, not assumed) -- it had exactly the same exposure as
Diameter, not better. The earlier claim was wrong and is corrected here rather than left standing.

**Fix, identical shape for both:** wrap the handler call in `try { handle_connection(...); } catch
(const std::exception&) { ... } catch (...) { ... }` *at the thread-spawn site*, not by threading a
`try`/`catch` through the ~775-line function body itself. This is deliberate, not just less typing:
C++ stack unwinding runs every local RAII destructor along the way regardless of where the nearest
enclosing `catch` sits, so `handle_connection`'s own `SessionCleanupGuard` (erases this connection's
entries from `session_to_connection_` under lock) and the `socket`'s own close still run correctly
before the new outer `catch` is ever reached -- nothing is skipped by catching one frame up instead
of deep inside.

**Piece 2: PgPool metrics, the half of ADR-0449 left open.** `PgPool::in_use()`/`exhaustion_count()`
existed on the class already; nothing read them. Added thin accessors (`pool_in_use()`/
`pool_exhaustion_count()`, matching each store's existing one-line-accessor style, e.g.
`is_connected()`) to every `PgPool`-owning class: `RatingDecisionStore` (CHF),
`BalanceStore` (balance-management), `ProductOfferingStore`/`ProductOfferingPriceStore`/
`ProductSpecificationStore` (product-catalog, 3 separate pools), `ProvisioningStore` (2 pools --
orchestration DB, charging DB), `IamStore` (gui/bff). Wired into real `OpenTelemetry`
`ObservableGauge`s, same `AddCallback`/`static_cast<T*>(state)` pattern `catalog_snapshot`'s own
gauges already established in `nfs/chf/src/main.cpp`:

- CHF: `chf_rating_db_pool_in_use` / `chf_rating_db_pool_exhaustion_total`.
- balance-management: `balance_management_db_pool_in_use` / `..._exhaustion_total`.
- product-catalog: `product_catalog_db_pool_in_use` / `..._exhaustion_total` -- **summed across all
  three internal stores into one pair of gauges**, not three separate pairs: an operator's real
  question is "is this NF's DB pool under pressure," not which of its three internal stores hit it.
- provisioning: `provisioning_orchestration_db_pool_in_use`/`..._exhaustion_total` and
  `provisioning_charging_db_pool_in_use`/`..._exhaustion_total` reported **separately** (not
  summed) -- these are two different databases, and collapsing them would hide which one is
  actually under pressure.
- gui/bff: `IamStore` got the accessor methods but **no gauge wiring in `main.cpp`**, a deliberate,
  disclosed scope boundary -- confirmed by grep that `gui/bff/src/main.cpp` has never once called
  `sbi_core::get_meter(...)`, so this NF has emitted zero OTel metrics of any kind before now.
  Giving it its first metric as a side effect of a PgPool-observability pass is a bigger, more
  speculative step than extending an existing pattern in the other four NFs; left for its own
  future increment rather than done half-attentively here.

**Verification.** `chf`, `balance-management`, `product-catalog`, `provisioning`, `oam-gui-bff`, and
`integration_tests` all rebuilt clean (zero new warnings) after both pieces, clang-formatted, and
rebuilt clean again. Regression run,
`--gtest_filter="ChfProtocolCeilings.*:CapScopedCharging.*:ChaosCharging.*:BalanceLossless.*:
ChfDiameterRar.*"`: **15/15 passed** -- the same real, repeated CI-port-collision pattern ADR-0449
hit (this session's own push for that ADR was still being tested by CI on this shared machine) cost
a couple of retries along the way, not a code regression; one retry caught the exact same
"terminate called... bind: Address already in use" / "failed to obtain an NRF-issued token"
signature already diagnosed and disclosed in ADR-0449, confirming it as the same known environmental
cause rather than a new one.

