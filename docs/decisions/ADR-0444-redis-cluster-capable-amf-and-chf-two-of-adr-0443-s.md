## ADR-0444: Redis-cluster-capable AMF and CHF -- two of ADR-0443's remaining 8 NFs converted this
pass, `RedisRouter` promoted to `libs/nf-config`

**Date:** 2026-09-28. **Status:** accepted. Follow-up to ADR-0443, which converted UDSF (the one
NF whose key design already needed real WATCH/MULTI/EXEC) to real Valkey Cluster routing and
named the other 8 Valkey-consuming NFs as "surveyed, none use multi-key ops, so converting each is
mechanical, not a redesign" -- deferred, enumerated follow-up work. This ADR is that follow-up, for
two of them: AMF (`ue_security_context_store.hpp/cpp`, `amf_ue_id_index_store.hpp/cpp`) and CHF
(`stores.hpp/cpp`, all six of its Redis-backed classes).

**PROOF STATUS.**

*AMF -- proven end-to-end, for real, against a real 6-node Valkey Cluster.* Brought up
`valkey-cluster-1..6` + `valkey-cluster-init` from `deploy/docker/docker-compose.yml`, unmodified,
under a disjoint temporary project (`-p 5gc-adr0444-proof`, ports 7001-7006, unused by both the
persistent lab stack and the CI runner's own service containers at the time -- checked via `docker
ps` before starting, not assumed) with `flock /tmp/5gc-r19-itest.lock`. `valkey-cluster-init`'s
real log: 3 masters (5461/5462/5461 slots, all 16384 covered), `[OK] All nodes agree about slots
configuration`, `[OK] All 16384 slots covered`, one replica each.

Rather than wait for the full monorepo build (see the disclosed gap below), a standalone harness
was compiled directly against the real, unmodified `nfs/amf/src/ue_security_context_store.cpp`
and `amf_ue_id_index_store.cpp` (compiled as those exact files, not copies -- `g++ -std=c++23`
against the vcpkg-built `redis++`/`hiredis`/`spdlog`/`OpenSSL` static libs alone, no monorepo
CMake graph needed since both files' own real dependency footprint is genuinely light: `aka_crypto`
contributes only a header-only `std::array<uint8_t,32>` type alias here, never a called function).
Compiled clean on the first attempt. Run against the live cluster through the host-published seed
`tcp://127.0.0.1:7001`, exercising 60 distinct synthetic tmsi/amf_ue_id pairs through
`UeSecurityContextStore::put/get/next_uplink_count` and `AmfUeIdIndexStore::put/get`, plus one call
to `allocate_tmsi()`:
- 60/60 `UeSecurityContext` round-trips and 60/60 `AmfUeIdIndex` round-trips succeeded (real GET
  after real PUT, real value equality checked in-process, not assumed).
- Real key format confirmed directly on-cluster (`valkey-cli keys "amf:*"` on one master):
  `amf:uesecctx:{10000002}`, `amf:ueidindex:{900003}`, etc. -- the literal hash-tag braces this
  ADR's `RedisRouter::tag()` adds in cluster mode, exactly as designed, not inferred.
- Keys landed on all 3 masters, not one, independently verified with `valkey-cli dbsize` against
  all 6 containers directly (not through the harness): masters 44/39/38 = 121 keys (60 uesecctx +
  60 ueidindex + 1 global `amf:next_tmsi` counter), replicas mirroring 44/39/38 in step with their
  own master.
This is real proof of the actual mechanism this ADR's AMF changes make possible: mode-gated
hash-tagged keys routing correctly to distinct shards for both stores, with zero connection-pinning
(`with(f)`, Decision 2 below).

*CHF -- NOT proven live this pass; a real, honestly-weaker gap than AMF/UDSF, disclosed rather
than papered over.* `stores.cpp`'s own real dependency footprint is genuinely heavier than AMF's
two stores or UDSF's own `store.cpp`: `stores.hpp` includes `TS26510_CommonData_grp.hpp` for
`sbi_gen::SpendingLimitContext`, a REAL generated type backed by `libs/sbi-generated`'s own static
library (247 generated `.cpp` files, `add_library(sbi_generated STATIC ...)`), not a light
header-only dependency the way `aka_crypto`'s `Kamf` type alias was for AMF's own harness. Building
that library (attempted: `ninja -C build-verify -j2 amf`, reusing the shared `~/.cache/5gc-ci/vcpkg`
binary cache, `build-tools/asn1c` symlinked in from the main tree rather than rebuilt) got to
2271/2410 build edges (past `sbi_generated`'s heaviest `TS26510_CommonData_grp_part*` files) when
this session's own harness independently reported the MACHINE was critically low on memory and
killed two idle monitor scripts to relieve it -- `free -h` at that moment showed 3.1Gi available
out of 15Gi, and `ps aux` showed FIVE concurrent `cc1plus` processes at ~2.1-2.2GB RSS each: two
from this session's own build, three from the CI runner's OWN in-progress `sanitize (tsan)` job
building the identical `sbi_generated` files with `-fsanitize=thread` in its own separate
`actions-runner` workspace at the same time. This session's own build was killed immediately
(`kill`/`pkill -9`) on seeing this, memory recovered to 9.8Gi available within seconds, and per
this session's own harness's explicit instruction ("do not start it again on your own... memory
may still be short") no further build of any kind was attempted afterward.

This means CHF's own routing logic is verified by careful reading only, not by a live run or even
a `-fsyntax-only` parse: every `RedisCluster` command CHF's stores call (`hset`/`hget`/`hmget`/
`hincrbyfloat`/`hsetnx`/`incr`/`sadd`/`srem`/`sismember`/`smembers`/`set`/`get`/`del`/`expire`) was
individually confirmed present with an identical signature in the real vendored
`sw/redis++/redis_cluster.h` (Decision 2), `sbi_gen::SpendingLimitContext`'s own real generated
definition was read directly (`TS26510_CommonData_grp.hpp:23260`, all-`optional` fields, so
default-constructible) rather than assumed, and every key-builder/`with(f)` call site in
`stores.cpp` was re-read after editing (this document's own commit range is exactly that diff).
This is real, disclosed, weaker verification than AMF's (which ran live) and UDSF's own ADR-0443
precedent (which ran a live harness too) -- named here rather than allowed to look equivalent.
Concretely deferred: compiling `nfs/chf/src/stores.cpp` (or a harness linking it) against the real
`sbi_generated` library and running it against a live cluster the same way `amf_harness.cpp` did,
the first time this machine is not simultaneously running a CI sanitizer build.

*NOT run within this pass's time budget: the real `amf`/`chf` HTTP-facing binaries end-to-end.*
Same disclosed gap class as ADR-0443's own UDSF gap, for the same real reason (contention), now
made concrete rather than hypothetical (previous paragraph). `clang-format-18 --dry-run --Werror`
is clean on every touched file (confirmed separately, does not depend on this build). The
remaining, real, disclosed gap, same shape as ADR-0443's own: AMF's proof is of the actual routing
logic, byte-for-byte the real store `.cpp` files, against a real cluster -- it does not prove the
full HTTP-facing `amf` binary (NRF registration, NGAP handlers, the rest of `main.cpp`) links and
starts in cluster mode, and CHF has neither level of proof this pass. `deploy/docker/
docker-compose.yml` was left completely untouched by this verification pass -- the temporary proof
stack was brought up under its own disjoint compose project and torn down (`docker compose -p
5gc-adr0444-proof down -v`) immediately after the AMF proof, nothing left running against the
shared box.

**Survey confirmed, not re-assumed.** Re-read every AMF/CHF store method before touching anything
(ADR-0443's own table already recorded "none" for multi-key ops on both, but this ADR verified it
against the real current source, not the table alone): AMF's `UeSecurityContextStore` is one Redis
HASH per tmsi (`amf:uesecctx:<hex>`, all reads/writes single-key HSET/HMGET/HINCRBY/DEL) plus one
genuinely global counter (`amf:next_tmsi`, INCR); `AmfUeIdIndexStore` is one STRING per
amf_ue_id (`amf:ueidindex:<id>`, single-key SET/GET/DEL). CHF's six classes (`IdempotencyStore`,
`ChargingDataStore`, `OfflineChargingDataStore`, `SpendingLimitSubscriptionStore`,
`PolicyCounterConfigStore`, `QuotaFeatureStore`) are the same shape throughout: one key per
entity, touched by one command at a time, plus a handful of genuinely global keys
(`chf:cdr:active`/`chf:cdr:next_id`, `chf:offline:active`/`chf:offline:next_id`,
`chf:sub:active`/`chf:sub:next_id`) that are shared SETs/counters across every session/subscriber,
not per-entity state. No call site does WATCH/MULTI/EXEC, a pipeline, or a multi-key command
(MGET/SCAN across entities) anywhere in either NF. This confirms ADR-0443's survey held.

**Decision 1: `RedisRouter` promoted from `nfs/udsf/src/store.hpp` into
`libs/nf-config/include/nf_config/redis_router.hpp`, verbatim plus one new method, not
copy-pasted a second and third time.** This project's own convention is that shared code lives in
`libs/` (`nf_config::connect_redis_or_die`/`connect_redis_cluster_or_die` already do), and a
second and third real consumer (AMF, CHF) is exactly the point at which "private to one NF" stops
being true. `nfs/udsf/src/store.hpp` now has a two-line `using RedisRouter = nf_config::RedisRouter;`
alias inside `namespace udsf` instead of the class body -- every existing unqualified `RedisRouter`
reference in `store.cpp`/`store.hpp` compiles unchanged, and UDSF's own `with(hash_tag, f)`/
`transaction(...)` entry points are untouched, byte-for-byte, in their new home.

**Rejected alternative: two more copies of the class, one in `nfs/amf/src`, one in
`nfs/chf/src`.** This is what ADR-0443 itself anticipated ("a `RedisRouter`-equivalent... one NF at
a time") and it would have worked, but three near-identical classes is exactly the kind of
duplication this project's own "shared code lives in libs/" convention exists to prevent, and any
future fourth NF would face the same choice again. Promoting once, now, closes that.

**Decision 2: AMF and CHF get a NEW entry point, `RedisRouter::with(f)` (no hash tag), not
UDSF's own `with(hash_tag, f)`.** Reading `sw/redis++/redis_cluster.h` (the real vendored header,
not assumed) confirms `RedisCluster` implements the FULL single-key command surface AMF/CHF's
stores use -- `hset`/`hget`/`hmget`/`hincrby`/`hincrbyfloat`/`hsetnx`/`hdel`/`get`/`set`/`del`/
`incr`/`expire`/`sadd`/`srem`/`sismember`/`smembers` -- as direct methods with the identical
signature `Redis` already has, each one computing its OWN hash slot from its OWN key argument
(`ShardsPool::_slot()`, the same real per-command routing UDSF's `with(hash_tag, f)` already
relies on) and fetching+releasing its own pool connection per call. Because every AMF/CHF
operation is genuinely single-key, there is no need for UDSF's `RedisCluster::redis(hash_tag,
new_connection=false)` shard-BINDING step at all -- `with(f)` just calls `f(*cluster_)` or
`f(*single_)` directly, generic over which type `f` receives (a lambda taking `auto&`).

This is a real, deliberate, and safer design difference from `with(hash_tag, f)`, not a shortcut:
`with(hash_tag, f)` PINS one connection out of a shard's pool for the whole call
(`sw::redis::GuardedConnection`, fetched at construction, released at destruction) -- exactly the
mechanism ADR-0443's own "one real bug this proof work caught" section warns never to nest.
`with(f)` pins nothing, ever, in either mode: every command inside `f` fetches and releases its
own connection independently, the same way `sw::redis::Redis`'s internal pool already worked in
single-node mode before any of this. This is checked, not assumed -- `ChargingDataStore::create()`
below batches an untagged global-set SADD and a tagged per-ref HSET inside the SAME `with(f)` call
(two different keys, potentially two different slots in cluster mode), which would be illegal
inside `with(hash_tag, f)`'s single bound connection but is exactly what `with(f)` is for.

**Rejected alternative: force AMF/CHF through UDSF's own `with(hash_tag, f)`, picking some hash
tag per call anyway.** Rejected on two grounds: (1) it would reintroduce the exact
nested-connection-pinning hazard ADR-0443's own bug-writeup names, for zero benefit, since nothing
in either NF needs a connection pinned across more than one command; (2) `RedisCluster::redis()`
returns a bound single-connection `Redis&`, and `Redis::transaction()` throws on it (ADR-0443
Decision 1) -- irrelevant here since neither NF transacts, but it would have been one more sharp
edge inherited for nothing.

**Decision 3: hash tags on AMF/CHF's own per-entity keys, mode-gated -- present ONLY in cluster
mode, so single-node key bytes are unchanged from before this ADR.** Unlike UDSF (whose
`{realmId/storageId}` tag has been baked into every key unconditionally since ADR-0400, before any
cluster existed to need it), AMF and CHF have real, currently-deployed single-node key formats with
no braces at all. `RedisRouter::tag(id)` is the single source of truth: it returns `id` unchanged
in single-node mode and `"{" + id + "}"` in cluster mode. Every per-entity key builder
(`context_key`/`index_key` in AMF, `charging_data_content_key`/`spending_limit_key`/
`quota_feature_key`/`idempotency_key_redis_key` in CHF) now takes the router and calls `tag()` on
its own identifier segment. Verified nothing outside these two NFs' own source depends on the
exact key bytes: `grep`ped every literal prefix (`amf:uesecctx`, `amf:ueidindex`, `amf:next_tmsi`,
`chf:cdr:`, `chf:sub:`, `chf:idem:`, `chf:quotafeat:`, `chf:offline`, `chf:policycounter`) across
`tests/`, `tools/`, `gui/`, `deploy/`, `config/` -- the only hits outside the NFs' own source are
`docs/DECISIONS.md`'s own ADR history and `deploy/db/valkey/keyspaces.md` (a forward-looking
Tier-1 design doc, discussed below), neither of which parses a live key at runtime.

Real per-entity tags chosen:
- AMF `UeSecurityContextStore`: tag = the tmsi hex itself (`amf:uesecctx:{0001abcd}` in cluster
  mode) -- this store's own whole key IS the tmsi, the UE's own real identity for this store.
- AMF `AmfUeIdIndexStore`: tag = the amf_ue_id itself (`amf:ueidindex:{12345}`), **not** tmsi.
  Real, disclosed correctness point, not just a missed optimisation: this index's entire purpose
  is a REVERSE lookup -- `PathSwitchRequest` arrives with only the amf_ue_id, before the tmsi it
  maps to is known, so `get(amf_ue_id)` can never supply tmsi as a hash tag; tagging by tmsi would
  have been impossible to even compute at the one call site that needs it. The two stores' own
  keys are NOT expected to colocate on one shard, and nothing needs them to -- both are exclusively
  single-key, so there is no shared transaction to protect.
- AMF `amf:next_tmsi`: deliberately untagged -- a single global counter, not a per-UE key.
- CHF `ChargingDataStore`'s per-ref content hash: tag = ref (`chf:cdr:content:{chg-42}`).
- CHF `SpendingLimitSubscriptionStore`: tag = the subscriptionId (`chf:sub:{sub-7}`).
- CHF `QuotaFeatureStore`: tag = supi only, NOT supi+ratingGroup (`chf:quotafeat:{imsi-...}:5`) --
  CHF's own natural sharding/subscriber identity is the SUPI; this colocates every rating group of
  one subscriber on one shard, a deliberate choice (not required by any current transaction) that
  matches `deploy/db/valkey/keyspaces.md`'s own stated Tier-1 design intent ("hash-tag the
  subscriber identifier ... all of one subscriber's keys land in one slot").
- CHF `IdempotencyStore`: tag = the caller-supplied idempotency key itself (`chf:idem:{...}`).
- CHF's global sets/counters (`chf:cdr:active`/`next_id`, `chf:offline:active`/`next_id`,
  `chf:sub:active`/`next_id`) and `PolicyCounterConfigStore` (operator config, not per-subscriber):
  deliberately, permanently untagged. Tagging a GLOBAL set per-entity would SPLIT it into many
  separate per-entity sets -- breaking `is_active`/`release`/`list_all`'s own "one shared set of
  everything currently active" contract. This is named explicitly as a real correctness trap the
  task itself warned about, caught by design rather than by testing it wrong first.

**Rejected alternative: hash-tag every key unconditionally, matching
`deploy/db/valkey/keyspaces.md`'s own generic (mode-non-specific) framing, the same way UDSF's
tag has applied since before any cluster existed.** That doc's own "Sharding rules" section reads
as a standing target for this project's Valkey usage generally, not literally already true of
every NF's current source -- its own `chf:session:{ref}` doesn't even match CHF's real key name
(`chf:cdr:content:<ref>`) today. Applying it unconditionally would change AMF/CHF's SINGLE-mode key
bytes for every existing deployment that has not opted into cluster mode -- directly against this
task's own explicit "do not change default behaviour for anyone who doesn't opt in" rule, and,
unlike UDSF, there is no pre-existing precedent inside these two NFs to preserve continuity with
(braces were never part of their key format before this ADR). Mode-gating is the interpretation
that needs no argument about what "default behaviour" means: it is trivially, syntactically
unchanged. The keyspaces.md doc's broader vision is not contradicted -- once/if AMF's or CHF's own
compose entry is ever opted into `redis_mode=cluster` (still not done for UDSF either, ADR-0443
Decision 5), the tags this ADR adds are exactly the ones that doc already called for.

**Decision 4: config -- `redis_mode`/`redis_pool_size` in `config/amf.json`/`config/chf.json`,
same shape as UDSF's, `redis_pool_size` applied ONLY in cluster mode.** `redis_mode` defaults to
`"single"`, `AMF_REDIS_MODE`/`CHF_REDIS_MODE` override, same precedence as every other key. Unlike
UDSF (which appends `?pool_size=N` to `redis_url` unconditionally, in both modes), AMF and CHF
append it ONLY when `redis_mode == "cluster"`. Real reason: neither NF's `redis_url` carries a
`pool_size` query parameter today, meaning both currently run with the library's own default
(`ConnectionPoolOptions::size = 1`, confirmed by reading `connection_pool.h`) in single mode --
that is these NFs' own existing, working, single-mode behaviour, and this ADR does not retroactively
decide a different pool size for it. Cluster mode is new, and the SAME risk ADR-0443 names (a
whole shard's traffic serialised through one connection if left at the library default) applies
here too, so cluster mode alone gets a `redis_pool_size` (default 8) appended, in main.cpp, before
`connect_redis_cluster_or_die` is called.

**Definition-of-done items this ADR does NOT claim:** this is an infrastructure/routing change to
two already-shipped NFs' own persistence layer, not a new NF or a new TS 23.502 procedure -- no new
`docs/TRACEABILITY.md` row (ADR-0443 set this same precedent: no TRACEABILITY entry for a routing
change with no new procedure/test-per-clause), no README architecture-diagram change (no new
component, no datastore this project didn't already draw).

**Deferred, disclosed, not built this pass:**
1. The remaining 6 of ADR-0443's original 8 (AUSF, ADRF, NWDAF, DCCF, LI-MDF, MFAF) stay on plain
   `sw::redis::Redis` against the single-node `valkey` service -- real, separate, mechanical
   follow-up work per NF, same as ADR-0443 itself deferred all 8.
2. Neither AMF's nor CHF's own `deploy/docker/docker-compose.yml` service entry is switched to
   `redis_mode=cluster` -- both stay pointed at the single-node `valkey` service, unchanged, same
   as UDSF's own entry (ADR-0443 Decision 5). This ADR delivers and proves the capability; it does
   not flip a running default.
3. Helm: unchanged, same pre-existing gap ADR-0443 already disclosed (zero charts reference
   Valkey/Kafka at all).
4. CHF's own live proof (PROOF STATUS above): not run this pass, real and disclosed, deferred to
   the first session that is not sharing this machine with an in-progress CI build.
5. Neither `amf` nor `chf`'s own full HTTP-facing binary was built or run this pass (PROOF STATUS
   above) -- AMF's two touched stores were proven live via a standalone harness instead; CHF's
   were not proven live at all this pass.

