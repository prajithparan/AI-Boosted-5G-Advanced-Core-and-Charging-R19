# Mandated tech stack -- remaining bullets

Moved verbatim from CLAUDE.md (section "Mandated tech stack" (bullets not kept in CLAUDE.md)); see `docs/optimization/MOVE_LOG.md`.

- **Codegen**: openapi-generator (cpp-restsdk / cpp-pistache targets) OR a
  custom Jinja generator — evaluated with evidence in Phase 1, not guessed.
- **JSON**: nlohmann/json (ergonomics/GUI/config) + simdjson (hot parse
  paths/telemetry) — benchmark before choosing per path.
- **Async/runtime**: Boost.Asio (or libuv), lock-free MPMC queue,
  thread-per-core where it measurably helps.
- **PFCP/N4 + UP**: libpfcp-style codec (implement if none suitable);
  DPDK, VPP, or eBPF/XDP for the UPF datapath — evaluate and justify.
- **Storage**: Redis/Valkey (UDSF, session cache), PostgreSQL (UDR),
  Apache Doris (CDR/analytics — ADR-0192 records the migration and the
  full engine comparison), Kafka or Redpanda (event bus).
- **Observability**: OpenTelemetry C++, Prometheus exporter, spdlog,
  Grafana.
- **GUI**: JSON-schema-driven. Backend exposes REST+WebSocket JSON; front
  end renders dynamically from JSON Schema (React + JSON Forms, or Dear
  ImGui + nlohmann/json for a native console) — both proposed, one
  recommended in Phase 7.
- **AI/ML**: ONNX Runtime (in-process C++ inference), optionally NVIDIA
  Triton, MLflow (tracking), Kubeflow or Flyte (pipelines), Kafka
  (feature stream). Training may use a Python sidecar; inference must be
  in-process C++, never a Python call at runtime.
