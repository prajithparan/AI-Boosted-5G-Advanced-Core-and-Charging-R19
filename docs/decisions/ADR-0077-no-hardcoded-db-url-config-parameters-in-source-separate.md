## ADR-0077: no hardcoded DB URL/config parameters in source -- separate config files, mandatory project-wide (user-directed)

### The decision

User-directed, standing, mandatory coding decision, given mid-turn while ADR-0076's AMF work
above was still uncommitted: **no NF or BSS service may hardcode a DB URL, connection string, or
other deployment parameter as a literal default inside a `.cpp` file.** The real value belongs in
a separate, checked-in config file; an env var may still override a given key at deployment time
(container/k8s convenience -- the same override mechanism this project already used piecemeal,
e.g. `AMF_REDIS_URL`, `CHF_REDIS_URL`), but there is no third, in-source literal fallback.

This is a real, standing engineering-practice decision (a coding *decision*, in the same durable
category as ADR-0001's greenfield rule or ADR-0006's synchronous-client debt), not scoped to AMF
specifically. A sweep run while implementing it found the pattern this decision targets already
present, unaddressed, in every NF and BSS service built so far: `grep -rl "tcp://\|postgresql://
\|127.0.0.1\|localhost" nfs/*/src/main.cpp bss/*/src/main.cpp` matched all of `amf`, `ausf`,
`chf`, `hello-nf`, `nrf`, `pcf`, `smf`, `udm`, `udr`, `upf`, and all four `bss/*` services --
i.e. this was universal project practice up to this point, not an isolated oversight.

### What's built and applied this turn (AMF only -- see task #109 for the rest)

- **`libs/nf-config`** (new, header-only): `nf_config::load(service_name, config_dir)` resolves
  and parses `<config_dir>/<service_name>.json` (or the path in a `<SERVICE_NAME>_CONFIG_FILE`
  env var, for the rare case a deployment needs a wholesale different file, not just one key).
  `nf_config::require<T>(config, key, env_name = nullptr)` returns the env var's value if
  `env_name` is given and set, else the config file's own value for `key`, else throws --
  deliberately no third fallback, so a missing key fails loudly at startup rather than silently
  reverting to an undocumented default.
- **`config/amf.json`** (new, checked in, non-secret lab defaults -- same class of file as
  `simulators/ransim/config/{gnb,ue}.yaml`, already an established precedent for checked-in lab
  config): `port`, `metrics_bind_address`, `nrf_base_url`, `redis_url`, `ngap_bind_address`,
  `ngap_bind_port`, `amf_region_id`, `amf_set_id`, `amf_pointer` -- every one of these was a
  `constexpr`/hardcoded-`getenv`-default literal in `nfs/amf/src/main.cpp` before this ADR.
- **`nfs/amf/src/main.cpp`**: loads `config/amf.json` at the top of `main()`, threads the real
  values through to the HTTP/2 server bind, the metrics exporter, `run_nrf_lifecycle`, and
  `run_ngap_lifecycle` (all previously hardcoded `constexpr` values or a single getenv-with-
  literal-default helper, `amf_redis_conninfo()`, now removed). `redis_url` keeps its
  `AMF_REDIS_URL` env-var override name (unchanged behavior for anyone already using it);
  `nrf_base_url` gained a new `AMF_NRF_BASE_URL` override for the same reason (see next
  paragraph). `kNrfInstanceId` (a fixed protocol-identity constant, ADR-0018) and `CERTS_DIR` (a
  CMake-supplied build-time path, same class as the new `CONFIG_DIR`) are explicitly NOT in
  scope -- neither is a runtime deployment parameter in the sense this ADR targets.
- **`nfs/amf/CMakeLists.txt`**: new `CONFIG_DIR="${CMAKE_SOURCE_DIR}/config"` compile definition
  (same pattern as the existing `CERTS_DIR`), links the new `nf_config` interface library.
- **`deploy/docker/amf.Dockerfile`**: `COPY config/amf.json /build/config/amf.json` into the
  runtime stage (checked-in, non-secret, so copied at build time -- unlike `certs_data`, which
  must come from the shared `pki-init` volume since it's generated, not checked in).
- **Real, additional bug found and fixed while wiring this, not part of the original ask**:
  `config/amf.json`'s own default `nrf_base_url` (`https://127.0.0.1:7777`, carried over
  unchanged from the pre-existing hardcoded value) does not actually work across separate
  `docker compose` containers -- compose's default bridge network gives each container its own
  loopback, so AMF's container could never have reached NRF's container this way. This was
  **already broken before this ADR's own change** (the literal was `127.0.0.1` in source before
  today too); it surfaced only because implementing the override mechanism made it visible.
  Fixed for AMF specifically: `deploy/docker/docker-compose.yml`'s `amf` service now sets
  `AMF_NRF_BASE_URL: https://nrf:7777` and `AMF_REDIS_URL: tcp://redis:6379` (compose DNS names),
  plus a new `redis: {condition: service_healthy}` entry in `depends_on` (AMF's Redis dependency,
  ADR-0076, had no compose wiring at all yet). **Every other already-composed NF
  (`smf`/`udm`/`udr`/`ausf`/`pcf`, all confirmed via grep to hardcode the identical
  `https://127.0.0.1:7777`) likely has the same latent bug** -- not fixed here, not silently
  dropped either: recorded as a real, concrete finding in task #109's own description, to be
  fixed as each of those services gets its own config-file retrofit turn.

### What this ADR does NOT include

The other 9 NF main.cpp files and 4 `bss/*` main.cpp files identified by the same-session grep
sweep -- CHF, UDR, AUSF, NRF, PCF, SMF, UDM, UPF, `hello-nf`, and all four BSS services still
hardcode DB URLs/connection parameters exactly as they did before this ADR. This is deliberate
staging, not scope-narrowing after the fact: CLAUDE.md's own "one NF/subsystem per turn" working
style applies here the same as everywhere else in this project -- retrofitting 13 more files in
the same turn as AMF's `ServiceRequest`/GUTI work (ADR-0076) would be an unreviewable, unrelated
wall of changes. Tracked as task #109, to be closed one service (or small batch) per turn. A YAML
config format was considered (matches `simulators/ransim/config/*.yaml`'s existing precedent) but
JSON was chosen instead specifically to avoid adding a new dependency (`yaml-cpp`) when
`nlohmann-json` is already a project-mandated dependency (CLAUDE.md's "Mandated tech stack") used
everywhere else in this codebase -- revisit only if a real need for YAML-specific features (e.g.
comments, anchors) surfaces later.

