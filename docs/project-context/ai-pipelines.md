# AI pipelines (NWDAF-centric)

Moved verbatim from CLAUDE.md (section "AI pipelines (NWDAF-centric)"); see `docs/optimization/MOVE_LOG.md`.

- NWDAF split into AnLF (analytics logic) and MTLF (model training logic)
  per TS 23.288: `Nnwdaf_EventsSubscription`, `AnalyticsInfo`,
  `DataManagement`, `MLModelProvision`, `MLModelTraining`,
  `MLModelMonitor`.
- Data plane: NFs emit events -> Kafka -> feature store -> training
  (Python sidecar, training ONLY) -> ONNX artifact -> in-process C++
  inference in AnLF via ONNX Runtime.
- At least three working analytics: (1) NF load prediction, (2)
  abnormal-behaviour/anomaly detection, (3) slice SLA / service-experience
  prediction. R19 additions: energy-efficiency analytics, a
  vertical-federated-learning (VFL) hook.
- Every model versioned in MLflow with training-data lineage and an
  explicit drift-monitoring path via `Nnwdaf_MLModelMonitor`.
- Optional agentic layer: an MCP server exposing read-only NF state and
  analytics as tools. Read-only by default; any write/config action
  requires explicit human approval in the loop.
