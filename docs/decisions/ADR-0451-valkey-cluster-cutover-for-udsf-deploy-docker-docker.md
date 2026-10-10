## ADR-0451: Valkey Cluster cutover for UDSF (deploy/docker/docker-compose.yml only)

ADR-0445's debt list named two things about the 6-node Valkey Cluster `docker-compose.yml` already
built under ADR-0443: AOF disabled, and no NF actually pointed at it. Re-reading ADR-0443's own
comments in that file first (not assumed from memory) showed the real reason: UDSF's own store code
(`RedisRouter`, built on `sw::redis::RedisCluster`, which handles MOVED/ASK redirects) was already
converted and cluster-ready -- ADR-0443 simply never flipped UDSF's own compose entry over, using a
standalone harness for its own proof instead. This closes that specific, narrow gap -- not a
platform-wide migration. Every other NF (AMF, AUSF, CHF, ADRF, NWDAF, DCCF, LI-MDF, MFAF) keeps
using the single-node `valkey` service unchanged, because their store code talks to Valkey through
plain `sw::redis::Redis`, which has no redirect handling -- pointing that at one cluster node would
silently serve only ~1/N of keys. Converting any of those is separate, future, per-NF work.

**Changes, `deploy/docker/docker-compose.yml` only, no application code touched:**
- The 6 cluster nodes' shared command now uses `--appendonly yes` (was `no`) -- ADR-0445's "AOF
  disabled" debt item, closed. UDSF's data (PCF/SMF session context, exposure subscriptions) is real
  operational state a restart must not silently lose; RDB snapshotting alone loses everything since
  the last snapshot on a crash, AOF does not.
- UDSF's own service entry: `UDSF_REDIS_URL` changed from `tcp://valkey:6379` to
  `tcp://valkey-cluster-1:6379`, and a new `UDSF_REDIS_MODE: cluster` env var added (the exact env
  var `nfs/udsf/src/main.cpp` already reads -- `redis_mode`/`UDSF_REDIS_MODE` -- confirmed by
  reading the source, not guessed; `config/udsf.json`'s own default stays `"single"` so every
  existing UDSF integration test that relies on that default config is untouched by this change).
- `depends_on` changed from `valkey: condition: service_healthy` to
  `valkey-cluster-init: condition: service_completed_successfully` -- a cluster node reporting
  healthy only means that one process is up, not that the cluster has been FORMED (slots assigned
  across all 6 nodes); starting UDSF before `--cluster create` finishes would have every real key
  operation fail with `CLUSTERDOWN`.

**Verified: cluster formation is real, not assumed.** Brought the 6-node cluster up and ran the
existing `valkey-cluster-init` one-shot job (`docker compose up -d valkey-cluster-1..6
valkey-cluster-init`). Its own log is the evidence, not a guess: `[OK] All nodes agree about slots
configuration.` / `[OK] All 16384 slots covered.`, with the real topology printed -- 3 masters each
holding a distinct slot range (`0-5460`, `5461-10922`, `10923-16383`) and 3 replicas, one per master.
This is the real, structural proof the cutover's target is sound.

**Still not verified as of this update, disclosed plainly rather than assumed to work: the live
UDSF functional check.** The intended next step -- bring up `pki-init`/`nrf`/`udsf` against this
real cluster, PUT a real record through UDSF's own `Nudsf_DataRepository` API, and GET it back to
prove a write/read round-trip actually lands in the cluster -- was attempted **six times** across
this session and failed every time, never once on a code or config problem:

1-2. Blocked by a GitHub Actions self-hosted runner on this shared machine running its own heavy
   sanitizer/build cycles, pushing load average into the 30-55 range and available memory below 1GB
   for roughly two hours straight (swap climbed ~11GB to ~25GB). One stopped manually, one by the
   harness's own memory-pressure reaper.
3-4. Retried once the runner's load cleared (load dropped to ~1, confirmed via `uptime`). The build
   itself turned out to be the real cost here, not contention: every Dockerfile in this repo
   (`nrf.Dockerfile`, `udsf.Dockerfile`, ...) does its own fully independent `vcpkg install` from a
   single shared `vcpkg.json` manifest -- confirmed by reading `nrf.Dockerfile`'s own comment on
   this ("vcpkg.json is one shared manifest, `vcpkg install` pulls in every dependency for ANY
   target's configure step") -- which means building UDSF's image, by itself, still cold-compiles
   the *entire* dependency set including ONNX Runtime (a large, slow-to-compile ML library that
   nothing in UDSF actually calls -- it's pulled in only because the manifest is shared, not because
   UDSF needs it). Doing this for two images (`nrf` + `udsf`) in parallel, twice, both ran out of
   memory with the build ~90%+ complete each time (vcpkg package 93-100 of 102, mid-ONNX-Runtime).
5. Tried building only `udsf` (reusing the NF's own existing, already-built `docker-nrf:latest`
   image rather than rebuilding it) to halve the ONNX Runtime compile load -- this did NOT help,
   confirming point 3's finding: the shared-manifest cost is per-image, not per-pair-of-images, so
   `udsf` alone still needs the full cold bootstrap. Killed manually near the same completion point.
6. Retried once more, deliberately handing control back to the harness's own memory-pressure reaper
   (rather than a stricter self-imposed threshold) -- killed again by the harness.

**Decision, made with the user directly after the 6th failure: stop retrying for now.** This is a
genuine shared-machine capacity constraint (a ~15GB-RAM box cold-compiling ONNX Runtime from source,
competing with whatever else is running on it), not evidence against the change. What *is* real and
already verified, independent of this blocker: the cluster itself forms correctly (16384/16384 slots
covered, 3 masters + 3 replicas, `valkey-cluster-init`'s own log), the compose config is syntactically
valid (`docker compose config` passes), and `nrf` already starts and runs fine from its own existing
image with no changes needed. **Before relying on this cutover beyond a lab/demo context:** complete
the live UDSF write/read round-trip -- either on a machine with more headroom, or by pre-warming
vcpkg's binary cache outside of memory-constrained conditions first -- and record the real result
here.

**Completed, after ADR-0452's CMake fix made Docker builds reliable.** `docker compose up -d
--no-deps nrf` then `udsf` (`valkey-cluster-init` correctly refused to re-run against an already-
clustered set -- real, expected behaviour for a one-shot init container, not a bug -- so started
the two real NFs directly instead). Both crashed immediately on a real, separate, pre-existing bug
unrelated to this ADR (`nf_config: could not open config file`) -- see ADR-0453. Once that was
fixed:

- `udsf`'s own log: `udsf: connected to Valkey Cluster (seed tcp://valkey-cluster-1:6379?
  pool_size=16)` -- real connection, not the single-node fallback.
- Real `PUT /nudsf-dr/v1/Realm01/Storage01/records/adr0451-check` (a genuine `multipart/mixed`
  body matching this project's own `sbi_core::multipart::encode_subtype` wire format, sent over
  real mTLS using a client cert extracted from the deployment's own `certs_data` volume, not the
  host's unrelated `certs/` directory) -- **201**, with a real etag.
- Real `GET` of the same record -- **200**, same etag, same content, byte for byte.
- Real `DELETE` -- **204**, confirmed gone.
- Verified at the storage layer directly, not just through the API: `docker exec
  docker-valkey-cluster-3-1 valkey-cli keys '*'` showed `udsf:{Realm01/Storage01}:rec:
  adr0451-check` on **both** node 3 (a master) **and** node 4 (its real replica) while the record
  existed -- proof the write landed in the actual cluster and replicated, not an in-process cache.

ADR-0451 is now fully closed: config change, cluster formation, and the live functional round-trip
all real and verified. Scope unchanged from the original decision -- UDSF only; every other NF
still uses the single-node `valkey` service, since their store code isn't cluster-aware.

**NRF registration 400 -- found above, root-caused and fixed immediately after (two commits).**
Replayed UDSF's exact registration request directly against NRF with curl, using the same mTLS
client cert extracted from `certs_data`, and read NRF's real `ProblemDetails` body:
`"ipv4Addresses contains an invalid IPv4 address"`. Two separate fields in
`nfs/udsf/src/main.cpp`'s `run_nrf_lifecycle` NFProfile construction used `advertised_ipv4` -- the
Docker Compose service DNS name `"udsf"`, not an IP literal -- where NRF's validation requires a
real dotted quad: the top-level `ipv4Addresses` (first commit), then, after that fix alone still
left the same 400, `nfServices[].ipEndPoints[].ipv4Address` as well (second commit) -- NRF applies
the identical validation to both fields. Fixed both to the literal `"127.0.0.1"`, matching the
placeholder convention every other working NF's registration code already uses (e.g.
`nfs/chf/src/main.cpp:366`), confirmed safe for UDSF specifically because no NF discovers it yet
("No NF consumes the UDSF yet", noted elsewhere in this file). Live-verified after the fix: `udsf:
registered with NRF (HTTP 201)`.

**Correction, same day, caught before push:** the audit below originally claimed, universally,
that "real SBI-to-SBI calls in this project use config-provided base URLs, not the IP NRF returns
in a discovered NFProfile" and hardcoded `"127.0.0.1"` into six more NFs on that basis. That claim
is false in general -- two real counterexamples already existed in this codebase:
`nfs/nwdaf/src/mtlf.cpp:721-724` (the MTLF resolves a discovered AnLF's `nnwdaf-mlmodelmonitor`
endpoint directly from `ipEndPoints[].ipv4Address`) and `nfs/smf/src/main.cpp:413-414` (SMF reads
a discovered UPF's `ipv4Addresses[0]` for N4). Hardcoding nwdaf's fields to `"127.0.0.1"` would
have broken real AnLF/MTLF cross-container discovery -- caught by review before any push, reverted
for nwdaf specifically: it now resolves `advertised_ipv4` via `getaddrinfo` to a real routable IP
at startup (`resolve_ipv4_literal`) and uses that in both fields, keeping the config key.
dccf/adrf/mfaf/nsacf/amf's own fields have no real consumer anywhere in this tree (grepped, not
assumed) and are unaffected -- `"127.0.0.1"` stays correct for those five plus udsf/chf. Corrected
standing rule going forward: grep for a real consumer of a discovered field before choosing
between a hardcoded placeholder and a resolved real IP, rather than assuming the convention is
universal.

