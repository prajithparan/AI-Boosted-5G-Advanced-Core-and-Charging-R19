## ADR-0443: Valkey Cluster + Kafka partition/RF sizing for the Tier-1 scale mandate -- UDSF
converted to cluster-aware routing, the other 8 Valkey-consuming NFs' gap disclosed, not silently
papered over

**Date:** 2026-09-28. **Status:** accepted. See PROOF STATUS below for exactly what ran for real on
this machine today versus what is verified by source-reading and local syntax-checking only --
this machine showed the same severe CPU contention ADR-0394 recorded earlier today (a CI build job
alone held load average at 14-18 on 8 cores for the better part of an hour), and this ADR does not
claim a proof it could not actually run to completion.

**Survey (task step 1).** `grep -rn "redis\|valkey" -i` across `nfs/`, `bss/`, `libs/`, `config/`
finds 9 NFs with a real `redis_url` in `config/<nf>.json` and a real `sw::redis::Redis` connection
in their own source, all through the one shared helper, `nf_config::connect_redis_or_die`
(`libs/nf-config/include/nf_config/redis.hpp`):

| NF | store file(s) | what it keeps in Valkey | multi-key ops (transaction/watch/pipeline/scan/mget)? |
|---|---|---|---|
| AMF | `amf_ue_id_index_store.cpp`, `ue_security_context_store.cpp` | 5G-GUTI index, persisted NAS/AS security context | none |
| AUSF | `kausf_store.cpp` | KAUSF anchor key cache | none |
| CHF | `stores.cpp` | E3 ChargingDataStore session state | none |
| ADRF | `state_store.cpp` | collection/retention state | none |
| NWDAF | `subscription_store.cpp`, `ml_store.cpp`, `collection_store.cpp`, `accuracy_monitor.cpp` | event subscription state (ADR-0360), ML model registry, collected analytics, drift monitor | none |
| DCCF | `subscription_store.cpp` | data-collection subscription state | none |
| LI-MDF | `task_store.cpp` | LI delivery task state | none |
| MFAF | `config_store.cpp` | messaging-framework config | none |
| UDSF | `store.hpp`/`.cpp` | Nudsf_DataRepository + Nudsf_Timer records (the NF's entire job) | **yes** -- WATCH/MULTI/EXEC, SCAN, multi-key tag indexes, ADR-0400's own key design already `{realmId/storageId}` hash-tag-scoped per storage |

(`bss/balance-management/src/store.hpp` mentions `sw::redis::Redis` only in a comparative comment
explaining why IT uses plain PostgreSQL instead -- not a real Valkey consumer; excluded above.)

Kafka: two real topics, both through `libs/event-bus` (librdkafka), both with no explicit
partition/replication-factor provisioning anywhere -- `KAFKA_AUTO_CREATE_TOPICS_ENABLE: "true"`
on the single pre-existing broker meant every topic got whatever `num.partitions`/
`default.replication.factor` that broker's own defaults were (1/1), silently, on first produce.
`chf.cdr` (CHF's CDR event stream, `nfs/chf/src/cdr_event_producer.cpp`, ADR-0355) and
`mfaf.notifications` (MFAF's Nnwdaf-bound notification fan-out, `nfs/mfaf/src/main.cpp`,
ADR-0365). NWDAF/DCCF do not talk to Kafka directly today (checked, not assumed) -- their
"feature pipeline" language in CLAUDE.md is aspirational infrastructure, not yet a real
producer/consumer in this codebase; nothing here invents one.

Deployment topology before this ADR, `deploy/docker/docker-compose.yml`: one `valkey` container
(`valkey/valkey:8-alpine`, standalone RESP server, no `--cluster-enabled`), one `kafka` container
(`apache/kafka:3.9.0`, KRaft, broker+controller combined, the only possible node). `deploy/helm/`
has charts for 8 NFs (nrf, amf, smf, udr, ausf, keycloak, oam-gui-bff, pcf, udm) and **zero**
charts or values referencing Redis/Valkey/Kafka at all (`grep -rln "redis\|valkey\|kafka"
deploy/helm -i` -- no hits) -- Helm provisions no datastore for any NF today; an operator points
each NF's own `values.yaml` at wherever they run Valkey/Kafka themselves. This is real,
pre-existing debt, same shape as ADR-0400's own "Helm stays Phase 8 debt" note for MFAF/DCCF/ADRF
-- disclosed here, not silently extended by inventing a Helm convention that does not exist yet.

**Decision 1: the client library CAN speak real cluster protocol -- `sw::redis::RedisCluster`,
already vendored, was simply never used.** `vcpkg.json` already pulls `redis-plus-plus`
(1.3.15), and `sw/redis++/redis_cluster.h` -- included transitively by every NF's existing
`#include <sw/redis++/redis++.h>` -- ships `class RedisCluster`, a full second client class
alongside `Redis`, built specifically for this: `ShardsPool::_slot()` (read from the installed
package's own vendored source,
`~/vcpkg/buildtrees/redis-plus-plus/src/*/src/sw/redis++/shards_pool.cpp`) implements the
standard Redis Cluster hash-tag algorithm verbatim (find `{`, find matching `}`, CRC16 the
substring between if present, else the whole key) against whatever string is handed to it as a
"key" -- so `RedisCluster::redis(hash_tag)` / `::transaction(hash_tag, ...)` route correctly when
given a REAL key (or any string containing the same `{...}` substring a real key would) without
any extra logic on this project's side. `RedisCluster::redis()` returns a plain `sw::redis::Redis`
object bound to that one shard's connection pool, so most existing single-key call sites
(`smembers`, `hget`, `zadd`, `scan`, ...) are source-compatible with zero change once wired to a
bound connection. The one place this is NOT true, found by reading the source rather than
assumed: `Redis::transaction()`/`::pipeline()` unconditionally `throw Error("cannot create
transaction in single connection mode")` when the `Redis` object was constructed from
`RedisCluster::redis()`'s single pinned `GuardedConnection` rather than a real pool (`_pool` is
empty in that case, `redis.cpp:42-48`) -- `RedisCluster::transaction(hash_tag, ...)` is the
library's own, separate, correct entry point for a cluster-routed WATCH/MULTI/EXEC, and this is
the one real piece of routing logic this ADR's code actually adds (`RedisRouter::transaction()`
below), not invented, found.

**Decision 2: UDSF is the one NF converted this pass, not all 9.** UDSF is the only NF whose key
design was already built for this (ADR-0400's `{realmId/storageId}` hash tag, "so ... the
MULTI/EXEC transactions ... stay legal on a cluster" -- written before any cluster existed to test
that claim against). The other 8 are single-key-only (table above) and would need no key redesign,
only a mechanical client-type swap plus config plumbing -- real, deferred work, not built this
pass; enumerated so it is not lost. Converting UDSF and proving it for real against a genuine
multi-shard cluster is a more meaningful proof of the actual mechanism (hash-slot routing,
WATCH/MULTI on the correct shard connection) than converting 8 NFs whose single-key commands would
have worked almost by accident even against a broken router.

**Decision 3: `RedisRouter` (`nfs/udsf/src/store.hpp`), not a NF-wide abstraction, not yet.** A
small class holding either a `shared_ptr<sw::redis::Redis>` (single-node, unchanged) or a
`shared_ptr<sw::redis::RedisCluster>` (new), with two methods -- **this is the second, corrected
design**; PROOF STATUS below records the real bug an earlier `conn(hash_tag) -> sw::redis::Redis`
version had and why it was replaced before anything was committed:
- `with(hash_tag, f) -> decltype(f(Redis&))`: calls `f` once with a `Redis&` bound to that hash
  tag's shard (`cluster_->redis(hash_tag, /*new_connection=*/false)`) in cluster mode, or `*single_`
  unchanged in single-node mode. Every existing plain-command call site in `store.cpp`
  (`get_record`, `get_sub`, `list_subs`, `get_schema`, `timers_using_schema`, `get_timer`,
  `claim_due`, `schedule`, `enqueue`, `dequeue`, `purge_storage`, `IndexImpl`'s five methods) now
  does its whole unit of work inside one `with()` call instead of dereferencing a bare
  `shared_ptr<Redis>` directly -- mechanical, ~20 call sites, same command API either way. Each
  call site opens and closes its own `with()`; none call another Store method (which would open a
  second one) from inside one -- `list_subs()` inlines `get_sub()`'s own body for exactly this
  reason instead of calling it per id.
- `transaction(hash_tag, piped, new_connection) -> sw::redis::Transaction`: routes to
  `cluster_->transaction(...)` or `single_->transaction(...)` per Decision 1's real finding.
  `modify_record` and the shared `modify_doc<Extra>` template (used by `modify_sub`,
  `modify_schema`, `modify_timer`) go through this, never through `with(hash_tag,
  ...).transaction()`, which would have thrown in cluster mode (Decision 1).

`Store` gained a second constructor, `Store(shared_ptr<RedisCluster>, string)`, alongside the
unchanged `Store(shared_ptr<Redis>, string)`. `IndexImpl` now holds the `RedisRouter` itself plus
this storage's own prefix (not a bound connection -- an `IndexImpl` instance's lifetime spans
multiple calls from its caller, `dr_routes.cpp`/`timer_routes.cpp`, sometimes interleaved with
other `Store` calls; holding one bound connection for that whole span would risk the same
pool-exhaustion problem `with()`'s scoping exists to avoid). Each of its five methods opens its
own short-lived `with()`.

**Decision 4: config, not a hardcoded literal (ADR-0077).** `config/udsf.json` gained
`"redis_mode": "single"` (default, every other deployment's behaviour is byte-for-byte unchanged);
`"cluster"` switches `nfs/udsf/src/main.cpp` to call the new
`nf_config::connect_redis_cluster_or_die(redis_url, "udsf")` (`libs/nf-config/include/nf_config/
redis.hpp`) instead of `connect_redis_or_die`, reusing `redis_url`'s own existing `?pool_size=N`
query-parameter convention (parsed by `sw::redis::Uri::connection_pool_options()`) to size each
shard's own connection pool -- `ConnectionPoolOptions::size` defaults to 1 in the library, which
would have serialised a whole shard's traffic through one connection if left unset.
`connect_redis_cluster_or_die` calls `cluster->for_each([](Redis& r){ r.ping(); })` at startup --
strictly stronger than the single-node helper's own PING (it proves every discovered shard is
reachable, not just the seed), same fail-fast-or-die rule (`nf_config::fatal`). `UDSF_REDIS_MODE`
env override, same precedence as every other key. GUI schema regenerated
(`python3 gui/schema-gen/derive_nf_config_schemas.py . gui/web/src/schemas/nf-config`, `--check`
now passes); `redis_mode` is correctly listed under `x-review` ("present in the file but no source
line visibly reads it") because the generator's `REQUIRE_RE` only recognises
`nf_config::require<T>()`, not the `nf_config::optional<T>()` this key deliberately uses (a
default-preserving key cannot be `require<>`) -- a real, small, pre-existing generator gap this
ADR did not fix (out of scope), disclosed rather than worked around by hand-editing the generated
file.

**Decision 5: Valkey Cluster topology -- 3 masters + 3 replicas, additive, NOT a replacement
for the existing single-node `valkey`.** `deploy/docker/docker-compose.yml` gained
`valkey-cluster-1..6` (`valkey/valkey:8-alpine --cluster-enabled yes`) plus a
`valkey-cluster-init` one-shot running `valkey-cli --cluster create ... --cluster-replicas 1
--cluster-yes`. 3 masters is the minimum Redis/Valkey Cluster needs to hold slot/failover quorum
at all; 1 replica per shard is the minimum for the shard to have any failover. This is sized to
prove real multi-shard routing and per-shard replication, not against a measured UDSF ops/memory
budget -- no such figure is documented for UDSF the way `chf.cdr`'s CDR/day figure is documented
for CHF, and this ADR says so rather than inventing one.

The existing single-node `valkey` service is untouched and every one of the other 8 NFs keeps
pointing at it, unchanged, on purpose: their store code is plain `sw::redis::Redis`, which has no
MOVED/ASK-redirect handling (Decision 1), so repointing any of them at one node of the real
cluster would have made it silently serve only the ~1/6 of keys that happen to hash to that one
node's own slot range and error on the rest -- exactly the "cluster that only actually has one
usable shard from the client's point of view" trap this task named explicitly. UDSF is the only NF
whose CODE can speak to the new cluster (`RedisRouter`, Decisions 1-3); its own default compose
service entry is deliberately left on `redis_mode=single` against the unchanged single-node
`valkey` too, same as every other NF -- so, as shipped, nothing in the lab compose file actually
talks to the new cluster yet. This ADR delivers the capability and proves the mechanism against the
cluster directly (PROOF STATUS below), it does not flip a running default that the rest of the lab,
or other in-flight parallel-stream work, might depend on. Opting UDSF's own compose entry in is a
two-line change (`UDSF_REDIS_URL=valkey-cluster-1:6379`, `UDSF_REDIS_MODE=cluster`, plus a
`depends_on: valkey-cluster-init`) deliberately left for a follow-up, not done silently here.

**Decision 6: Kafka -- 3-broker KRaft cluster, real replication, explicit partition sizing.**
`kafka` (single broker) became `kafka-1`/`kafka-2`/`kafka-3` (same `apache/kafka:3.9.0` image, same
combined broker+controller KRaft role each already used), `KAFKA_CONTROLLER_QUORUM_VOTERS` listing
all three, `KAFKA_OFFSETS_TOPIC_REPLICATION_FACTOR: 3` (was 1 -- the internal
`__consumer_offsets` topic needs real replication too, not just the two topics this ADR sizes
explicitly). `KAFKA_AUTO_CREATE_TOPICS_ENABLE` flipped `"true"` -> `"false"`: with it left on, a
racing first producer creates the topic with whatever broker default `num.partitions` applies
before this file's own sizing ever runs, silently undersizing a topic nothing told it to size. A
new `kafka-topics-init` one-shot creates both topics explicitly instead, after all 3 brokers are
healthy.

Sizing math for `chf.cdr` (real Tier-1 target, not invented): 300M CDRs/day (`project_tier1_scale_
architecture`, ADR-0392) / 86,400 s = 3,472 CDR/s average. Busy-hour concentration is a
**disclosed assumption, not a measured or 3GPP-specified figure**: ~15% of a day's total traffic
falling in its single busiest hour, a commonly used telecom capacity-planning rule of thumb (a
day spread perfectly evenly across 24h would put 4.17%/hour; 15% is a real, if informal,
industry-standard concentration factor, not derived from any number in this repository) -> 45M
CDRs in that hour -> 12,500 CDR/s sustained peak. x2 disclosed headroom for burst-within-the-
busy-hour and near-term growth -> **25,000 CDR/s design target**. Divided by a **disclosed,
NOT independently measured** conservative per-partition throughput assumption of 3,000 msg/s
(KB-sized JSON, `acks=all`, idempotent producer, shared/modest hardware -- in the range commonly
cited in published Kafka sizing guidance, not this project's own benchmark): ceil(25,000/3,000) =
9, already a clean multiple of the 3-broker count (3 leader-partitions per broker) -- **9
partitions**. PROOF STATUS below ran a real `kafka-producer-perf-test.sh` against this exact
cluster and topic and reports what it found, explicitly NOT as this sizing's basis (that run
shared the machine with a live CI build under heavy contention, so its throughput number is not a
capacity ceiling, only a distribution proof). `mfaf.notifications` has no documented rate
anywhere in this repository -- this ADR does not
invent one; it is instead sized for consumer-group parallelism headroom (partition count is the
hard upper bound on how many MFAF replicas can consume the topic concurrently, ADR-0365's own
`--scale mfaf=N` replication model). Both topics: `replication.factor=3` (every broker),
`min.insync.replicas=2` -- without this, CHF/MFAF's own `acks=all` producer setting
(`cdr_event_producer.hpp`/ADR-0355) only ever actually waits for the partition LEADER, not a
second broker, so a leader crash right after ack could still lose an acknowledged record; RF=3
alone does not buy that guarantee, `min.insync.replicas` does.

`nfs/chf/routine_load.doris.sql`'s `kafka_broker_list` updated to the 3-broker internal list
(`kafka-1:29092,kafka-2:29092,kafka-3:29092`). Its `kafka_partitions` property is deliberately
left unset, and this ADR checked why that is still safe before growing `chf.cdr`'s partition
count -- fetched from the real, current Doris CREATE ROUTINE LOAD reference
(`https://doris.apache.org/docs/dev/sql-manual/sql-statements/data-modification/load-and-export/
CREATE-ROUTINE-LOAD/`) via WebFetch while writing this ADR, not recalled from memory: *"If not
specified, defaults to subscribing to all partitions under the topic from OFFSET_END"* -- so this
job needs no change when the partition count changes. This checks the public docs for the Doris
version family this project uses; it was NOT re-verified against the specific installed image's
own local behaviour the way ADR-0392's pg_partman work ran the real thing end-to-end -- narrower
verification than that precedent, disclosed as such rather than claimed equal to it.
`MFAF_EVENT_BUS_BROKERS` (compose) changed from `kafka:9092` (the single broker's HOST listener,
advertised as `127.0.0.1:9092`) to the new brokers' own INTERNAL listener,
`kafka-1:29092,kafka-2:29092,kafka-3:29092` -- matching the pattern Doris' own Routine Load
already used correctly. Whether the old `kafka:9092` value actually worked or actually failed for
MFAF was never observed (MFAF was never run against it to find out) -- this ADR infers it was
latently wrong from reading the broker's own `KAFKA_ADVERTISED_LISTENERS` config
(`HOST://127.0.0.1:9092`, which resolves to the container asking itself, not the Kafka broker,
from inside any OTHER container), not from a reproduced failure; stated as an inference, not an
observed bug. `kafka-ui`'s bootstrap-servers and `depends_on` updated to all three brokers.

**PROOF STATUS.** Run today against a real, temporary compose project (`-p 5gc-datalayer-proof`,
ports disjoint from the persistent lab stack and from CI's own testcontainers, brought up and torn
down under `flock /tmp/5gc-r19-itest.lock`, only after confirming via `gh run view
--job=<id>` that this was between the running CI build's own port-binding steps -- not simply
trusting `gh run list`'s own summary, which this project's own memory already flags as
sometimes stale).

*Valkey Cluster -- proven end-to-end, for real.* `valkey-cluster-1..6` came up healthy;
`valkey-cluster-init`'s real `valkey-cli --cluster create --cluster-replicas 1 --cluster-yes` log
shows 3 masters (5461/5462/5461 slots each, all 16384 covered) with one replica each, `[OK] All
nodes agree about slots configuration`, `[OK] All 16384 slots covered`. Rather than wait on a full
monorepo build of the `udsf` binary (this machine's contention made that impractical today, see
below), a standalone harness (`RedisRouter` copied verbatim from `store.hpp`, not re-derived, so
it is the exact same code) was compiled directly against the vcpkg-built `redis++`/`hiredis`
static libs alone (~1s, no monorepo dependency graph) and run against the live 6-node cluster
through a plain host-routable seed (`tcp://127.0.0.1:7001`; container IPs the cluster announces,
e.g. `172.21.0.3`, were confirmed directly pingable from the host over the compose bridge network
-- the exact thing ADR-0443's own draft flagged as needing verification, not assumption). Result,
across 50 distinct `udsf:{Realm01/StorageN}:` hash-tagged storages:
- 50/50 real WATCH/MULTI/EXEC transactions (`RedisRouter::transaction`, the same call
  `Store::modify_record` makes) committed with no `WatchError`.
- Keys landed on all 3 masters, not one: 17/16/17. Independently re-verified with a second,
  unrelated tool (`docker exec <container> valkey-cli dbsize` against all 6 containers directly,
  not through the harness or the router at all): masters 17/16/17, replicas 17/17/16 (replication
  keeping each replica in step with its own master). `cluster info` on node 1: `cluster_state:ok`,
  `cluster_slots_assigned:16384`, `cluster_slots_ok:16384`.
This is real proof of the actual thing this ADR's code change makes possible: hash-tag-based
routing to the correct shard for both plain commands and cluster-mode transactions, not a
single-shard cluster with the client pointed at one node by accident.

*Kafka -- proven end-to-end, for real, with the throughput number explicitly caveated.*
`kafka-1/2/3` came up healthy as a real 3-node KRaft quorum (not three isolated single-node
brokers -- `kafka-topics-init`'s own `--describe` output, run against the live cluster, shows
`chf.cdr` with `PartitionCount:9 ReplicationFactor:3 Configs:min.insync.replicas=2` and its 9
partitions' LEADERS spread across brokers 1/2/3, each with `Isr:` listing all 3 replicas in sync;
`mfaf.notifications` the same, `PartitionCount:6`). A real produce run,
`kafka-producer-perf-test.sh --num-records 5000 --record-size 700 --throughput -1
acks=all enable.idempotence=true` against `chf.cdr` on the live cluster, completed:
**2,782 records/sec, 920 ms avg latency**. `kafka-get-offsets.sh` immediately after shows every
one of the 9 partitions with a real, distinct, non-zero offset (368, 552, 552, 598, 598, 598, 414,
644, 676 -- summing to the full 5,000), proving genuine multi-partition distribution under real
produce load, the task's own stated proof criterion. **The 2,782 rec/s figure is explicitly NOT
used anywhere as a capacity/sizing number** (Decision 6 already states the real sizing basis is a
disclosed 3,000 msg/s/partition assumption, not a measurement): this produce run shared the
machine with GitHub Actions' own CI build, which alone held load average at 14-18 on 8 cores for
most of today's session (confirmed via `uptime` immediately before and after), so 2,782 rec/s from
a single producer thread under that contention says nothing reliable about this cluster's real
ceiling in an uncontended deployment -- reported here only as evidence the pipeline moved real
data through all 9 partitions, not as a throughput claim.

Both proof stacks were torn down immediately after (`docker compose ... -p 5gc-datalayer-proof
down -v`) -- nothing from this ADR's verification work is left running against the shared box.

*NOT run: the real `udsf` binary against the cluster end-to-end.* This machine showed the same
severe contention ADR-0394 recorded earlier today: a `ninja -j2 udsf` build managed 15 object
files in 13 minutes before it was killed (to stop competing with the in-progress CI build), and a
`-fsyntax-only` check of `nfs/udsf/src/store.cpp` alone -- one translation unit, against this
project's real generated headers (`sbi_gen`, `TS29598_*`), not the standalone harness's minimal
includes -- took roughly 7 minutes of wall time to return. It DID return clean: **zero errors**,
two pre-existing `-Wsign-conversion` warnings on the `purge_storage`/`scan()` cursor-type
conversion, unchanged by this ADR's edits (confirmed by diff: that line is untouched, only moved
inside a `with()` lambda) and already covered by CLAUDE.md's own "~40 outstanding clang-tidy style
warnings" disclosed debt -- not fixed here as out of scope. `libs/nf-config/include/nf_config/
redis.hpp` and `nfs/udsf/src/main.cpp` were not independently syntax-checked against the full
project headers the same way, for the same contention reason; both are small, were read closely
multiple times against the real `sw::redis::RedisCluster`/`Uri` API while writing them (Decision 4
already cites the exact library source read for each call made), and the standalone harness
exercises the identical `RedisRouter` logic `store.hpp` declares. The remaining, real, disclosed
gap: nothing here proves the full `udsf` HTTP-facing binary (routes, JWT, NRF registration, the
rest of `main.cpp`) actually links and starts in cluster mode -- only that its one new piece of
routing logic, byte-for-byte, does what it claims against a real cluster. A full build-and-run
proof is the natural next step once this box is not simultaneously running a CI build.

**One real bug this proof work caught, disclosed rather than left in:** the first version of
`RedisRouter::conn(hash_tag) -> sw::redis::Redis` returned `*single_` (a copy) in single-node
mode. `sw::redis::Redis(const Redis&) = delete` (`redis.h`) -- this would not have compiled in the
DEFAULT mode (`redis_mode: "single"`), the one every existing deployment and CI actually uses,
while `-fsyntax-only` was still queued behind the CI build and had not yet caught it. Found by
independently re-reading the vendored library source (`grep -n "Redis(const Redis"
sw/redis++/redis.h`) before committing, not by the compiler. The design changed from
`conn()`-then-hold to the current `with(hash_tag, f)` (Decision 3's real text above), which also
closes a second, related risk in the original design: a `Redis` built from
`RedisCluster::redis()` pins one connection out of the shard's pool for its whole lifetime
(`GuardedConnection`, fetched at construction, released at destruction); the original `IndexImpl`
held one for the object's entire lifetime and `list_subs()` called `get_sub()` (itself opening a
second bound connection on the same shard) inside a loop already holding one -- with
`ConnectionPoolOptions::wait_timeout` defaulting to 0 (wait forever), enough concurrent requests
on one shard would have hung rather than errored. `with()` scopes every bound connection to one
call, and `list_subs()` now inlines `get_sub()`'s body instead of nesting a second `with()` inside
the first's.

**Deferred, disclosed, not built this pass:**
1. The other 8 NFs (table above) stay on `sw::redis::Redis` against the single-node `valkey`.
   Converting each is mechanical per Decision 2 (no key redesign needed, all single-key) but is
   real, separate work: a `RedisRouter`-equivalent (or direct `RedisCluster` per-key calls, which
   need no `Redis` handle at all for single-key commands) plus `<nf>_redis_mode` config plumbing,
   one NF at a time, matching this project's own "one NF per turn" convention -- not done here.
2. Helm: zero charts reference Valkey/Kafka today (pre-existing gap, Decision-0/survey). This ADR
   does not invent a Helm datastore convention to fill that gap -- `deploy/helm/` has none to
   match, and ADR-0400 already recorded the same debt for MFAF/DCCF/ADRF's own compose-only
   deployment.
3. Valkey Cluster shard count (3) and Kafka partition counts are sized against the task's own
   stated Tier-1 volume target and a disclosed capacity-planning assumption, not a measured
   per-NF ops/memory budget for every consumer -- UDSF specifically has no documented ops/sec
   target anywhere in this repository the way CHF's CDR/day figure is documented, and this ADR
   says so rather than inventing one to make the shard count look more derived than it is.
4. TLS/SASL on the Kafka listeners and Valkey Cluster's own AUTH/TLS: both stacks stay PLAINTEXT
   in the lab compose file, same as every other lab datastore today -- CDRs are billing data and
   this remains real, named debt (same class as ADR-0009's already-tracked gaps), not newly
   introduced by this ADR and not newly closed by it either.

