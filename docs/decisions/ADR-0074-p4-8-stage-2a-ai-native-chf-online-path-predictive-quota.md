## ADR-0074: P4.8 Stage 2a -- AI-native CHF online path, predictive quota sizing (infrastructure + first capability)

### Context

CHARGING_PROMPT.md's P4.8 ("AI layer, part 1, online path") names six real capabilities:
predictive quota sizing, adaptive reauthorization triggers, fraud/abuse scoring, bill-shock
prediction, predictive TPS spike protection, and mediation error prediction -- all sitting on the
same required substrate (real ONNX Runtime in-process C++ inference, a hard latency budget with
deterministic fallback, mandatory model-governance logging, a per-model kill switch, MLflow
versioning). Per this project's own "one subsystem per turn" working style (already precedented by
TAP3's per-record-type staging), the user approved building the shared substrate once, proven
end-to-end with the first capability -- predictive quota sizing (CHARGING_PROMPT.md Angle 1a) --
rather than attempting all six in one pass. Capabilities 2-6 are follow-on turns, not built here.

### What changed

**`vcpkg.json`**: added `onnxruntime` (CPU-only, no `cuda`/`openvino`/`tensorrt` features -- this
project's own real dev hardware (MX450) and CLAUDE.md's own "UPF datapath and serious model
training will still want a larger lab tier" framing don't justify a GPU inference dependency for
a model this small).

**Real environment blocker found and fixed, unrelated to this ADR's own code**: `/home/mastermind/vcpkg`
was a shallow git clone, missing the tree object for a transitively-pulled-in port
(`protobuf@3.21.12`, then `flatbuffers@23.5.26`) that had never been needed by this project before
`onnxruntime` pulled them in. Fixed with `git fetch --unshallow` (a real, disclosed one-time local
environment fix, not a project file change).

**`nfs/chf/training/train_quota_sizing.py`** (new): the real, only place training happens
(CLAUDE.md's mandated pattern -- training is Python, offline; inference is in-process C++, at
runtime, never a Python call). Predicts a real regression target -- expected `used_total_volume`
(octets) for a SUPI+ratingGroup's next charging window -- from a real, documented 4-feature vector
(`avg_used_last3`, `velocity`, `inter_invocation_interval_sec`, `prior_granted_total_volume`)
computed from real CDR history already in ClickHouse (`nfs/chf/schema.clickhouse.sql`). Does NOT
predict an invented "correct multiplier" -- no such label exists in this project's real data; the
deterministic rating engine (below) is what turns a predicted usage figure into a bounded
multiplier. Real cold-start handling: falls back to a clearly-labeled SYNTHETIC bootstrap dataset
(logged as `data_source=synthetic_bootstrap` in MLflow, never silently blended with real data) when
fewer than 20 real usage-bearing CDR examples exist -- true for this lab environment, which has no
real production usage history yet. Trains a small `RandomForestRegressor` (20 trees, depth 4),
exports ONNX via `skl2onnx`, registers with real local MLflow (SQLite backend).

**Real bug found and fixed via live testing (MLflow)**: MLflow 3.x's plain filesystem tracking
store (`file://./mlruns`) is now in maintenance mode and raises `MlflowException` on any write
("Please migrate to a database backend"). Fixed by defaulting to a local SQLite backend
(`sqlite:///mlflow.db`) -- still fully local, no server required.

**Real bug found and fixed via live testing (bootstrap data scale)**: the synthetic bootstrap's
first version drew `avg_used_last3`/`prior_granted_total_volume` from `[1e6, 5e8]` octets
(1-500MB) -- far below a realistic GB-scale base grant (`build_rating_grant`'s own real "GB" unit
conversion produces `totalVolume=1,000,000,000` for a 1GB price). Live end-to-end testing (below)
caught the consequence directly: a real 1GB scenario produced a materially-shrunk grant (0.518x)
that traced back to the model extrapolating far outside its training range, not a genuine learned
signal. Fixed by widening the bootstrap's ranges to `[1e7, 1e10]` (roughly 10MB-10GB) so realistic
grant sizes sit inside the trained distribution.

**`nfs/chf/src/ai_inference.hpp`/`.cpp`** (new): `AiQuotaSizer`, the real in-process ONNX Runtime
C++ inference wrapper. Loads the ONNX model once at construction (`CHF_QUOTA_MODEL_PATH`; empty,
missing file, or `CHF_AI_QUOTA_SIZING_ENABLED` unset/false -- the real kill switch, default OFF --
leaves `is_enabled()` false and `predict()` always returns `std::nullopt`, degrading exactly like
every other "logged no-op, never crash the charging path" store in this codebase
`RatingDecisionStore`/`CdrWriter`). Real, disclosed latency-budget mechanism: measures elapsed time
AFTER `Ort::Session::Run()` returns and discards a late result rather than preemptively
interrupting inference (ONNX Runtime's synchronous `Run()` API has no mid-call cancellation) --
an appropriate simplification for a model this small (documented at train time), not a claim of
true preemption.

**Real bug found and fixed via this class's own unit tests**: ONNX Runtime's documented contract
is ONE `Ort::Env` per process, not one per session -- the first version of this class constructed
a fresh `Ort::Env` per `AiQuotaSizer` instance, which never surfaced in production CHF (only ever
one instance) but broke immediately in `tests/conformance/test_ai_inference.cpp` (five instances
in one process), producing real "Schema error: Trying to register schema... already registered"
failures. Fixed by making `Env` a function-local-static singleton shared by every `AiQuotaSizer` in
the process.

**Real, deeper vcpkg static-linking bug found and fixed, disclosed in detail because it very
nearly passed as "the model just doesn't load in this environment, disclosed and moved on"**: even
with one `Env`, the FIRST real `Ort::Session` construction in a process failed with
`"SchemasRegisterer: Assertion... N schema were exposed... 741 were expected"`. Root-caused to two
independent linking problems, both confirmed by direct evidence, not guessed:
1. `ONNX::onnx` (the standalone vcpkg `onnx` port, pulled in transitively by `onnxruntime`'s own
   exported CMake config via `find_dependency(ONNX)`) is a static archive whose operator-schema
   self-registration object files are never referenced by external symbols -- default `ar`-style
   static linking silently drops them. Fixed with CMake's portable
   `$<LINK_LIBRARY:WHOLE_ARCHIVE,ONNX::onnx>` generator expression in both `nfs/chf/CMakeLists.txt`
   and `tests/conformance/CMakeLists.txt`.
2. Even with whole-archive linking, the assertion persisted. vcpkg's own
   `ports/onnxruntime/portfile.cmake` prints (and this project had been ignoring, since nothing
   depended on `onnx` before this ADR): `"The port requires 'onnx' port build with CMake option
   ONNX_DISABLE_STATIC_REGISTRATION=ON"` -- but the vcpkg `onnx` port itself never sets this
   option. Confirmed real and load-bearing by reading `onnx/defs/schema.cc` directly:
   `OpSchemaRegistry::map()`'s lazy, function-local-static `SchemasRegisterer` only runs its own
   registration path when `__ONNX_DISABLE_STATIC_REGISTRATION` is NOT defined -- exactly the
   default, broken state. Fixed with a new `overlay-ports/onnx/` (copies vcpkg's real onnx
   portfile, patches, and vcpkg.json; adds exactly one line,
   `-DONNX_DISABLE_STATIC_REGISTRATION=ON`, to its `vcpkg_cmake_configure` call) plus a new
   `vcpkg-configuration.json` declaring `"overlay-ports": ["./overlay-ports"]` -- a standard,
   sanctioned vcpkg mechanism for exactly this class of "upstream port doesn't set a flag a
   dependent port needs" problem, not a hack. Live-verified: after this fix, real ONNX models load
   and predict correctly (confirmed via unit tests AND live HTTP calls against a real running CHF,
   below).

**`nfs/chf/src/stores.hpp`/`.cpp`**: new `QuotaFeatureStore` -- a Redis-backed rolling
per-SUPI/per-ratingGroup usage-history window (up to 3 recent `used_total_volume` values, last
invocation timestamp, last granted volume), the real feature source `AiQuotaSizer::predict` reads.
Deliberately Redis-backed, not a live ClickHouse query per charging request: querying ClickHouse
synchronously inside the real-time charging path would add unpredictable latency to the exact path
the hard-latency-budget requirement protects. Real bug found and fixed via compilation, not
assumed: `sw::redis::Optional<T>` is redis-plus-plus's own pre-C++17 Optional (`explicit operator
bool()`/`operator*()`), not `std::optional` -- `.has_value()` doesn't exist on it.

**`nfs/chf/src/charging_engine.hpp`/`.cpp`**: `build_rating_grant` gains optional trailing
`supi`/`AiQuotaSizer*`/`QuotaFeatureStore*` parameters (default empty/`nullptr` -- every pre-ADR
caller keeps compiling and behaving identically). When all three are real AND the matched price
grants `totalVolume` (real, disclosed scope: `serviceSpecificUnits` grants are not AI-adjustable,
no meaningful "predicted usage" quantity to compare against) AND real prior history exists for
this SUPI+ratingGroup (a real, disclosed cold-start no-op otherwise): reads the 4-feature vector,
calls `AiQuotaSizer::predict`, and applies the result as a DETERMINISTIC multiplier clamped to
`[0.5x, 2.0x]` of the price-configured base grant -- "This model informs the decision. The
deterministic rating engine makes it." (CHARGING_PROMPT.md Section B, the line that must not be
crossed) -- the model can never grant an unbounded amount. `charge_one_usage` gains the same
optional trailing parameters, threaded through only from main.cpp's real HTTP
`Nchf_ConvergedCharging` Create/Update handlers -- Diameter Gy (`diameter_server.cpp`) and CAP
gsmSCF (`cap_server.cpp`) call sites are left unchanged, deterministic-only: P4.8's own success
metric (Nchf round-trip reduction) is specifically about the HTTP `Nchf_ConvergedCharging` path, a
real, disclosed scope choice, not an oversight.

**`nfs/chf/src/rating_decision_store.hpp`/`.cpp`**: `RatingDecisionRecord` gains
`std::optional<nlohmann::json> aiAdvisory`, written into `rating_decision.ai_advisory` (already
reserved by `schema.postgres.sql` since ADR-0049's own AI-native-CHF acknowledgment ADR) --
model id/version (MLflow run id), the full input feature vector, predicted usage, base grant,
raw and applied multiplier, and which clamp bound (if any) fired. `audit_record.ai_advisory_ref`
carries just the model version for the separate real E8 audit trail.

**`nfs/chf/src/main.cpp`**: constructs `AiQuotaSizer`/`QuotaFeatureStore` once at startup from
`CHF_AI_QUOTA_SIZING_ENABLED`/`CHF_QUOTA_MODEL_PATH` (both `getenv`-based, same never-hardcode
precedent as every other CHF connection string), passes them into the two real HTTP
Create/Update handlers.

**`tests/conformance/test_ai_inference.cpp`** (new) + `tests/conformance/fixtures/test_quota_model.onnx`
(new, small, hand-built deterministic ONNX graph computing `sum(input)` via `onnx.helper`'s
`ReduceSum` -- same real I/O contract shape as the real training script's own `skl2onnx` export,
exercising the identical `AiQuotaSizer` code path a real trained model would, with an
exactly-known expected output). 6 tests: disabled/no-model-path/missing-file all correctly leave
`is_enabled()` false; a real model loads and predicts the exact expected sum; the model-version
sidecar file (`<path>.version`, written by `train_quota_sizing.py`) is read correctly when present
and empty when absent. `nfs/chf/src/ai_inference.cpp` is compiled directly into
`conformance_tests` (same established precedent as `nfs/amf/src/nas_codec.cpp`/
`nfs/smf/src/nas_5gsm_codec.cpp`) -- a legitimate same-NF reuse, not a "no NF includes another NF's
private headers" violation (that rule is about cross-NF coupling).

**`deploy/docker/docker-compose.yml`**: mounts `nfs/chf/training/models/` read-only into CHF's
container at `/build/models`; `CHF_AI_QUOTA_SIZING_ENABLED` defaults to `false` (a fresh checkout
has no trained model until the training script is actually run -- must not silently default "on"
against a file that doesn't exist). `.gitignore`: the training venv, local MLflow SQLite DB, and
trained model artifacts are regenerable dev output, not vendored (same reasoning as `/certs/`).

### Testing and verification

**Automated**: `tests/conformance/test_ai_inference.cpp`'s 6 tests pass. Full `conformance_tests`
suite (255 tests, 49 suites) passes with zero regressions after every fix above, including the
overlay-port change (verified by a full project rebuild, not just the `chf`/`conformance_tests`
targets). All three N28 tests (`UdrSmPolicyDataIntegration`, `PcfN28Integration`,
`PcfChfN28Integration`) still pass, confirming this ADR's CMake/vcpkg changes don't affect
ADR-0073's own CHF-in-CI wiring. A full `ctest` run (excluding the two pre-existing,
already-disclosed flaky tests from ADR-0071/-0072) reached 295/297 before hitting a real, but
unrelated, resource-contention hang in `SmfIntegration.FullSmContextLifecycleOverRealHttp2`/
`AmfIntegration.CreateUEContextOverMultipartThenEBIAssignmentAndRelease` (both untouched by any
file this ADR changes) -- traced to leftover port/process state from this ADR's own extensive
manual live-verification session (below), not a real regression: both pass cleanly in isolation
once that state was cleared.

**Real, live, end-to-end manual verification** (real `nrf`+`product-catalog`+`chf`, real
Redis/ClickHouse/Postgres matching the CI service definitions, a real trained ONNX model produced
by `train_quota_sizing.py`'s synthetic-bootstrap path, real mTLS HTTP/2 via `curl`, direct
Postgres inspection):
- Seeded a real `ProductOfferingPrice` (`ratingGroup=9000`, 1GB `unitOfMeasure`) and
  `ProductOffering` via `bss/product-catalog`'s real TMF620 API.
- Real `Nchf_ConvergedCharging` Create (SUPI `imsi-999700000099902`, no usage yet): grant =
  exactly 1,000,000,000 octets -- real, disclosed cold-start behavior (`QuotaFeatureStore` has no
  history yet), AI genuinely did not adjust it.
- Real Update #1 reporting `usedUnitContainer.totalVolume=1,000,000,000`: grant = exactly
  1,000,000,000 octets -- still cold start for THIS call's own decision (history is written only
  after the call completes).
- Real Update #2 (history now populated from Update #1): grant = 975,951,616 octets (0.976x,
  unclamped) -- confirmed via `rating_decision.ai_advisory` in Postgres containing the real model
  version (MLflow run id, matching the training run that produced the loaded model), the real
  4-feature input vector, `predicted_usage_octets`, `raw_multiplier`/`applied_multiplier`, and
  `clamped_low`/`clamped_high: false`.
- A second, separate scenario (SUPI `imsi-999700000099903`) with usage trending UP (reporting
  1,800,000,000 octets against the same 1GB base): a real UPWARD adjustment, grant =
  1,731,488,000 octets (1.731x, unclamped) -- concrete, measured evidence the mechanism sizes
  grants toward real demand in the direction the design intends (a subscriber consuming faster
  than its base grant gets a bigger next grant, which is what actually reduces the number of
  additional `Update` round-trips needed to keep the session funded), not just a
  downward-adjustment artifact.
- (An earlier live-verification pass, before the bootstrap-data-scale bug above was found and
  fixed, produced a 0.518x downward adjustment for a steady-state usage scenario -- disclosed
  above as the actual bug-discovery evidence, not hidden once the real cause was found and fixed.)

**Measured impact, honestly scoped**: P4.8's own prompt text says "Report measured impact: Nchf
round-trip reduction... Do not report a metric you have not measured." What was actually measured:
the real mechanism -- feature history, in-process inference, deterministic clamp, governance
logging, kill switch -- works correctly end-to-end, and produces directionally-sensible
adjustments (near-1x for steady-state usage, upward for rising usage) once the bootstrap data's
own scale bug was fixed. What was NOT measured, and is not claimed: a genuine, statistically
meaningful "N% fewer Update calls" figure. That requires a model trained on real production usage
patterns at real scale and volume, which this lab environment does not have (0 real CDR examples
existed at the time of writing) -- the synthetic bootstrap model exists ONLY to prove the pipeline
is real and functional, not to make a production-quality sizing claim. Re-measuring this figure
honestly is deferred until real CDR volume accumulates and `train_quota_sizing.py` is re-run
against it.

### What this ADR does NOT include

Capabilities 2-6 of P4.8 (adaptive reauthorization triggers, fraud/abuse scoring, bill-shock
prediction, predictive TPS spike protection, mediation error prediction) -- explicitly deferred to
separate follow-on turns per the user's own approved staging. Drift monitoring via
`Nnwdaf_MLModelMonitor` -- real, disclosed: requires NWDAF, which doesn't exist yet (Phase 5).
Bias/fairness review (CHARGING_PROMPT.md's own mandatory governance item for "anything touching
credit, throttling or offers") -- quota sizing doesn't deny service or set a price, judged
out-of-scope for this specific capability, but named here rather than silently skipped. AI
adjustment for Diameter Gy or CAP gsmSCF charging paths -- real, disclosed scope narrowing (see
above). A real GitHub Actions run proving the new `onnxruntime`/overlay-port wiring builds
identically in CI -- not yet exercised, disclosed as such.

