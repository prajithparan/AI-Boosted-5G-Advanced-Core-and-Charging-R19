## ADR-0245: CHF's remaining Redis/Doris/AI config fields moved into `config/chf.json` (task #109)

### Context

ADR-0077 made it a project-wide, user-directed rule that no connection string or deployment
parameter may be a hardcoded literal default inside a source file: the default lives in
`config/<service>.json`, an env var may override any single key, and there is deliberately no
third in-source fallback. `libs/nf-config` implements it and every NF was retrofitted -- except
CHF's own Redis, Doris and AI-inference fields, which several ADRs (ADR-0085, ADR-0089's own
scope note, and the ADR-0192 migration) each explicitly disclosed as deferred to a later
CHF-focused turn, tracked as task #109. This ADR closes that item.

Before this change, `nfs/chf/src/main.cpp` still carried in-source literal defaults:
`"tcp://127.0.0.1:6379"` for Redis; `"127.0.0.1"` / `9030` / `"root"` / `""` / `"chf_cdr"` for
Doris; and, for AI quota sizing, an `== "true"` string comparison plus `kDefaultAiQuotaLatencyBudget`
read against a raw `getenv`. Four of the five were exactly the "hardcoded connection parameter"
shape ADR-0077 forbids -- and the CHF `port`/`nrf_base_url`/`rating_database_url` fields sitting
directly above them had already been converted, so the file was internally inconsistent.

### Decision

All eight remaining values move into `config/chf.json`: `redis_url`, `doris_host`, `doris_port`,
`doris_user`, `doris_password`, `doris_database`, `ai_quota_sizing_enabled`, `quota_model_path`,
`ai_quota_latency_budget_us`. `chf_redis_conninfo()` and `chf_doris_options()` now take the loaded
config; the AI block reads through `nf_config::require` too.

**Every existing env var keeps working unchanged** -- `CHF_REDIS_URL`, `CHF_DORIS_HOST`/`_PORT`/
`_USER`/`_PASSWORD`/`_DATABASE`, `CHF_AI_QUOTA_SIZING_ENABLED`, `CHF_QUOTA_MODEL_PATH`,
`CHF_AI_QUOTA_LATENCY_BUDGET_US` -- because `nf_config::require`'s own `env_name` parameter is the
same override mechanism. This was verified against the real setters rather than assumed:
`deploy/docker/docker-compose.yml` and `.github/workflows/ci.yml` between them set
`CHF_DORIS_PORT: "9030"`, `CHF_AI_QUOTA_SIZING_ENABLED: "false"` and
`CHF_AI_QUOTA_LATENCY_BUDGET_US: "50000"`, all of which parse correctly as JSON for their
non-string target types. Neither file needed editing.

### Real, disclosed behaviour changes (two, both deliberate)

1. **A malformed `CHF_AI_QUOTA_SIZING_ENABLED` now fails fast instead of meaning "disabled".**
   The old code was `env != nullptr && std::string(env) == "true"`, so `TRUE`, `1`, or a typo all
   silently disabled AI quota sizing. It is now parsed as JSON, so `true`/`false` behave exactly
   as before but anything else throws at startup. That matches `nf_config`'s own stated
   no-silent-fallback discipline, and a silently-disabled AI path is precisely the kind of thing
   that should not be discoverable only by noticing the feature never ran.
2. **`chf::kDefaultAiQuotaLatencyBudget` is no longer this binary's deployment default.** It
   remains `AiQuotaSizer`'s own library-level default for callers constructing one directly (the
   conformance tests do), but `chf`'s deployed value now comes from the config file. The shipped
   value is `50000` us, identical to the constant, so behaviour is unchanged -- what moved is
   *where the number lives*, which is the entire point of ADR-0077.

### Testing

Full build clean, clang-format clean, conformance 333/333, `ctest` 469/469. CHF started
standalone with zero env overrides to confirm the config file alone is sufficient.

