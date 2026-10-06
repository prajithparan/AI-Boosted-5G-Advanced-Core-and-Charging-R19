## ADR-0013: Prometheus metrics via opentelemetry-cpp's own exporter, not a separate prometheus-cpp dependency

**Date:** 2026-08-05
**Status:** Accepted

**Context:** CLAUDE.md's Definition of Done requires "OpenTelemetry spans + Prometheus metrics
emitted" per NF. Tracing (spans) already exists via opentelemetry-cpp (`otel.hpp`, Phase 0).
Metrics export does not exist yet anywhere in the project.

**Decision:** Enable opentelemetry-cpp's `prometheus` vcpkg feature
(`opentelemetry-cpp[prometheus]`) rather than adding `prometheus-cpp` as an independent top-level
dependency. `libs/sbi-core/include/sbi_core/metrics.hpp` mirrors `otel.hpp`'s shape
(`init_metrics(bind_address)` / `get_meter(name)`). Pull model: `PrometheusExporterFactory`
starts a small HTTP listener (civetweb, bundled transitively) that Prometheus scrapes directly at
`/metrics` -- no push/collector step, unlike the OTLP tracing path.

**Rejected alternative:** `prometheus-cpp` directly. It's what opentelemetry-cpp's Prometheus
feature uses internally anyway (visible in the vcpkg dependency resolution:
`prometheus-cpp[compression,core,pull]` gets pulled in transitively) -- depending on it directly
would mean two ways to create/register metrics in the same codebase (raw prometheus-cpp registry
API vs. the OTel Meter API already used for consistency with tracing) for no benefit.

**Verification:** NRF exposes `nrf_registrations_total`, `nrf_deregistrations_total`,
`nrf_heartbeats_total`, `nrf_tokens_issued_total` (counters) and `nrf_registered_nf_count` (an
`ObservableGauge` reading the live registry size via callback). Confirmed live:
`curl http://127.0.0.1:9464/metrics` returns real Prometheus text-format output with
`nrf_registered_nf_count{otel_scope_name="nrf"} 0` visible before any registration.

**API note for future NFs using this:** `opentelemetry::metrics::Counter<T>`/`ObservableInstrument`
are only forward-declared in `meter.h` (used for the `Meter` interface's return types); a caller
needs `#include <opentelemetry/metrics/sync_instruments.h>`,
`<opentelemetry/metrics/async_instruments.h>`, and `<opentelemetry/metrics/observer_result.h>`
directly to get complete types before calling `->Add(...)` or `->AddCallback(...)` -- otherwise it
fails to compile with "invalid use of incomplete type". `metrics.hpp` already pulls these in so
callers of `sbi_core::get_meter()` don't hit this themselves.

---

