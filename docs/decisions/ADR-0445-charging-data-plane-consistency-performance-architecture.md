## ADR-0445: Charging data-plane consistency & performance architecture -- response to an external
(ChatGPT/"GPT-6") architecture review, three storage designs compared, decision recorded

**Date:** 2026-10-02. **Status:** accepted (Option 1) for the consistency model; six implementation
items below remain open questions for the user, not yet decided.

**Origin and how it was treated.** The user had a different AI model (referred to in the source
file only as "GPT6"; not independently identifiable or verifiable as any particular Anthropic or
other vendor model) review this project and produced
`specs/GPT6-Model-Recommendation.pdf`, asking it to be studied and, where it improves the project,
implemented, with feedback and full ADR documentation. Per this project's own standing rule, that
document was treated as an external, untrusted-as-instructions input: every factual claim in it was
independently re-verified against this repository's actual HEAD (`1e4b792`) before any of it was
acted on, exactly as the document's own text in fact also asked for ("verify against HEAD... do not
rely on old status tables, treat these as findings to verify, not assumptions to blindly
implement"). Nothing in it was accepted on the other model's authority alone. It contains no 3GPP
field/path/TS-number claims to fabricate -- it is a data-plane/performance/HA engineering critique,
not a spec claim -- so CLAUDE.md's fabrication guardrail is not directly at stake here, but the same
discipline (verify before acting) was applied anyway.

**Verification result: every factual claim checked out true against real source, with one count
only approximately confirmed (UDR's connection total, below).**

| # | PDF claim | Verified against | Result |
|---|---|---|---|
| 1 | CHF rating fetches the ProductOffering collection, then fetches candidate prices individually over SBI | `nfs/chf/src/charging_engine.cpp:195,223` | **True** -- one `GET .../productOffering` then one `GET .../productOfferingPrice/<id>` per candidate |
| 2 | CHF resolves buckets and reserves/finalizes via separate SBI calls | `charging_engine.cpp:428` (bucket), `:470` (reserve), `:521-564` (finalize) | **True** |
| 3 | Release settlement is a separate unreserve + debit, leaving a failure window | `charging_engine.cpp:495-564`, comment at :495-502 | **True -- and already disclosed in-code** (ADR-0057's own comment explains the order-dependency bug this was fixed for; the *remaining* window between the two independent HTTP calls was not previously called out as its own gap) |
| 4 | Money passes through `double` in C++ despite PostgreSQL storing `NUMERIC` | `bss/balance-management/src/store.cpp:88,323,444,514`; schema `deploy/db/charging/40-balance.sql:24-25` (`NUMERIC(20,4)`, widened to unconstrained `NUMERIC` by `41-balance-lossless.sql`) | **True** |
| 5 | CDR producer's `publish()` doesn't wait for the broker's delivery report despite `acks=all` | `nfs/chf/src/cdr_event_producer.cpp:111,137-154` (`produce()` + non-blocking `poll(0)`) | **True** |
| 6 | UDR constructs dozens of stores with individual PostgreSQL connections, ~88/process | `nfs/udr/src/stores.hpp` (82 `pqxx::connection conn_;` members), `nfs/udr/src/main.cpp` (76 `...Store ...(conninfo)` instantiations) | **True, in shape; exact count not live-measured** -- `postgres-udr` was not running this pass, so `pg_stat_activity` could not confirm a live number; 82/76 static counts corroborate "approximately 88," not independently equal to it |
| 7 | The shared PostgreSQL pool waits indefinitely when exhausted; no deadline/backpressure/metrics | `libs/nf-config/include/nf_config/pg_pool.hpp:64-70` -- `cv_.wait(lk, [this]{ return !free_.empty(); })`, unbounded | **True.** (Note: this pool -- `PgPool`, already used by CHF's `rating_decision_store`, `balance-management`, `product-catalog` -- is a *different* component from finding #6's problem; UDR uses neither `PgPool` nor any pool at all, it is worse than #7's finding, not the same gap) |
| 8 | PostgreSQL is not configured as replicated/failover in Compose | `deploy/docker/docker-compose.yml` -- single `postgres-chf`, `postgres-udr`, `postgres-adrf`, no repmgr/Patroni/streaming-replica service | **True** |
| 9 | The new Valkey Cluster is additive, has AOF disabled, and no NF's default config uses it | `docker-compose.yml:884` (`--appendonly no` on all 6 `valkey-cluster-*` nodes); every `<NF>_REDIS_URL` (AMF, NWDAF x2, LI_MDF, MFAF, UDSF, DCCF, ADRF, CHF) points at the single-node `valkey:6379`; no `_REDIS_MODE=cluster` set anywhere in the default compose | **True.** ADR-0444 (immediately prior) made AMF and CHF *capable* of cluster mode in code; this compose file does not turn it on by default for anyone |
| 10 | Domain DDL applies only on first init of an empty volume; no migration/rollback framework | `docker-compose.yml` `docker-entrypoint-initdb.d` mounts (`101-132`, `441-484`, `578-625`); no Flyway/Liquibase/Sqitch/golang-migrate or custom runner found for the domain DBs (Keycloak's own internal DB is the one place Liquibase is mentioned, and it migrates Keycloak's own schema, not ours) | **True** |
| 11 | `docs/DATA_MODEL.md` is DRAFT and its Valkey "hot balance" role doesn't match the PostgreSQL-only balance implementation | `docs/DATA_MODEL.md:3` (`Status: DRAFT -- P4.1 deliverable, no code`), `:71,427` (`Redis/Valkey ... hot balance`) vs. `bss/balance-management` being the sole, PostgreSQL-only balance authority | **True** |

**What the PDF asked for that already exists -- not re-planned as new work:**

- **Step 5, "UPF meters locally, reports at thresholds, reauthorizes before quota exhaustion, no
  per-packet DB row."** Already real: `nfs/upf/src/datapath.{hpp,cpp}` (`register_urr`,
  `update_urr_thresholds`) implements real per-TEID Volume Threshold/Quota tracking in the XDP
  in-kernel counter path, per ADR-0050 Stage 2. No gap here.
- **Atomic, row-locked compare-and-update for a single bucket mutation.** The PDF's Data Model
  section asks for "one serialized owner or an equivalent atomic compare-and-update mechanism" per
  shared balance. `bss/balance-management/src/store.cpp`'s `reserve()` (:517-532) and `adjust()`
  (:449-453) are each already a single `UPDATE ... SET ... WHERE id = $1 AND <sufficient-funds
  check>` inside one `pqxx::work` transaction -- Postgres's own row lock is already the serializer.
  This requirement is met for *each individual* reserve/unreserve/adjust call; it is not met across
  the *sequence* of calls a Release/CCR-T finalize makes (see Decision B below).
- **Single code path for settlement across protocols.** `finalize_subscriber_balance()`
  (`charging_engine.cpp/.hpp:230-233`) is the one function all three settlement entry points call
  -- `main.cpp:1417` (Nchf `Release`), `diameter_server.cpp:1331` (Gy `CCR-Termination`),
  `cap_server.cpp:418` (CAP `ApplyChargingReport`/release). An atomic-settle fix only has to be
  made once, not three times.

**Correction to this project's own prior "zero benchmarking" framing.** ADR-0049 stated "zero
benchmarking of any kind has been performed against free5GC or anything else" -- true *at the time*
(2026-08-30), but ADR-0329/`docs/BENCHMARK_RESULTS.md`/`BENCHMARK_METHOD.md` (2026-09-10) have since
added one real, disclosed, narrow benchmark: NRF `SearchNFInstances` only, against free5GC v3.4.4,
with a committed-before-seeing-numbers method and both-directions-disclosed results (free5GC's p99
at concurrency 1 beats this project's). **That benchmark does not cover CHF, charging, or any
PostgreSQL/Valkey-backed path** -- "zero benchmark data exists for the charging data plane" remains
true today and is the actual baseline this ADR's Performance Evidence section (below) starts from.

---

### Required Architecture Decision: three storage/consistency designs compared

**Option 1 -- PostgreSQL authoritative balance and ledger; CHF holds only non-monetary in-memory
snapshots (catalog/policy) and rebuildable session projections (Valkey).** Read/write path for a
charge: snapshot lookup in-process (catalog+policy, no SBI/DB round trip) -> one PG transaction for
reserve (row-locked `UPDATE...WHERE`) -> Valkey session accumulator updated after PG commits.
Atomicity: per-bucket, via Postgres row lock; already implemented for single calls (see above).
Restart/failover: CHF restart loses nothing (PG is truth); Valkey loss loses only the per-session
running-reservation total needed to compute Release's finalize amount -- **today, this is not
rebuildable from PG alone** (see Decision B). PG failover: standard single-writer failover,
currently no HA configured (Compose gap, independent of which option is chosen). Footprint: no new
component. Complexity: lowest of the three -- extends what's there instead of replacing it.

**Option 2 -- Valkey as a replicated hot-state projection of balance, PostgreSQL remains the
ledger.** Same PG-authoritative write path as Option 1, but balance *reads* for rating also go
through a Valkey projection kept in sync by CDC/events rather than by a per-request PG read.
Atomicity: unchanged from Option 1 for writes; reads become eventually-consistent against the
projection, which is only safe if the write path (not the read-side cache) is what enforces
sufficient-funds, i.e., the projection must never be trusted to gate a reserve. Restart/failover:
adds a second thing that can be stale or lost and must be explicitly rebuilt from PG with a version
number -- more moving parts than Option 1 for a win (catalog reads are already free in both options;
this option's only extra benefit over Option 1 is caching the *balance* read, which CHF does not
currently do per-charge in the hot path once Option 1's snapshot exists for catalog/policy). Cross-
region: same as Option 1 for the ledger; the projection adds another thing to replicate or discard
on failover. Infra/ops cost: one more subsystem (a real CDC or event-replay pipeline) with no
measured workload yet proven to need it.

**Option 3 -- Valkey authoritative for reservation/balance, backed by a durable journal; PostgreSQL
becomes a derived/reporting store.** Lowest theoretical latency and highest footprint/complexity of
the three. Atomicity: requires Valkey's own transaction/scripting primitives (Lua scripts or
`MULTI`/`WATCH`) to replace Postgres row locks as the serializer, and a durable journal (Valkey AOF,
currently explicitly disabled on the only cluster this project runs -- finding #9 -- or an external
WAL) to survive a Valkey node loss without an unbounded RPO. Restart/failover: this is the design
that most directly needs the PDF's fencing-token requirement, because Valkey Cluster failover can
briefly leave two nodes believing they own a slot; Postgres's MVCC+row-lock model does not have this
failure mode. Infra/ops cost: highest -- a second source of monetary truth, its own backup/PITR
story, and a migration *away* from the schema-source-of-truth this project has built CHF/
balance-management/the CDR mediation pipeline around since ADR-0056/0057/ADR-0385's balance-event
model.

**Decision: Option 1.** Reasons, stated plainly:

1. **No measured need for Option 2 or 3 exists yet.** Finding #9 above (zero charging-data-plane
   benchmark) means there is no evidence PostgreSQL's reserve/adjust latency is a problem CHF
   actually has. ADR-0049's own rule -- "do not add a database product without measuring a concrete
   benefit" -- is a strong analogy for *re-architecting* an existing one's role: measure first
   (this ADR's Performance Evidence section), decide whether Option 2 is warranted second.
2. **Option 1 is also what the PDF itself defaults to**, explicitly: "Keep the PostgreSQL balance
   ledger authoritative unless the ADR proves another design has equivalent atomicity and
   recovery." No such proof exists for Option 2 or 3 today.
3. **The hardest part of Option 1 -- atomic compare-and-update under concurrent access to a shared
   bucket -- is already built and correct** (`reserve()`/`adjust()`'s row-locked conditional
   `UPDATE`). Options 2 and 3 would have to reproduce that guarantee in a different place (CDC
   consistency, or Valkey scripting) to be worth adopting, at real engineering and ops cost, for a
   benefit (faster balance reads) that Option 1 doesn't currently need -- CHF's slow path today is
   the *catalog* N+1 (finding #1), not the balance read, and Option 1 fixes that without touching
   where money lives.
4. **This keeps `balance-management` swappable for a commercial BSS stack** (CLAUDE.md's ODA
   boundary requirement) -- Option 1 changes CHF's internal catalog-snapshot behavior and
   balance-management's internal settlement transaction, not the TMF654-shaped boundary between
   them. Option 3 would make that boundary (and the backing store CHF's reservation-consistency
   story actually depends on) a provider-specific Valkey detail, which a commercial TMF654 product
   is very unlikely to expose the same way.

Options 2 and 3 are not discarded permanently -- they are the designs to revisit once the baseline
below exists and shows a real bottleneck Option 1 cannot address.

---

### Decision B: the real recovery gap Option 1 does not yet close, and the proposed fix

Independent of which option is chosen, one thing the PDF flagged turns out to be worse than its own
text stated: **`ChargingDataStore::add_reserved()`/`get_reserved_total()`
(`nfs/chf/src/stores.hpp:134-138`) hold the *only* record of how much a given charging session
(`ChargingDataRef`) has reserved so far, via Valkey `HINCRBYFLOAT`.** This is not a cache of
something Postgres also knows in a structured way: `balance_mgmt.reserve_balance` rows (the
PostgreSQL ledger of every reserve/unreserve call) carry no `chargingDataRef` or session
correlator column -- only a free-text `description` (confirmed: `charging_engine.cpp:465,526,554`
pass a human-readable string, not a structured key; confirmed: TMF654's real `ReserveBalance`
shape, `libs/bss-sid/include/bss_sid/balance.hpp:226-244`, sourced directly from TMF654
v4.0.0's swagger, has no such field either). **If Valkey is lost or this session's key expires
before Release/CCR-T/CAP-release runs, `get_reserved_total()` silently returns 0, finalize
unreserves nothing, and the reservation is stranded in `bucket.reserved_value_amount` forever** --
money the subscriber is holding but that is never returned to `remaining_value_amount` nor debited.
This is the real version of the PDF's "treat in-memory rating snapshots as rebuildable projections"
requirement failing today for the *reservation total*, specifically (the PDF's own language talks
about rating/catalog snapshots; this is the balance side of the same requirement, and it is not
currently met).

**Proposed fix (not yet implemented, scoped as part of increment 2 below):** add an **internal-only
PostgreSQL column**, not a TMF654 wire field, to `balance_mgmt.reserve_balance` (and
`adjust_balance`) tying each row to `charging_data_ref` + an invocation-sequence number -- exactly
the PDF's own Data Model ask ("a stable reservation/idempotency record tying... chargingDataRef,
invocation sequence..."), implemented the way this project's `audit_record.ai_advisory_ref` column
already demonstrates is safe: a column that exists in `balance_mgmt`'s own schema and is never
serialized into the TMF654 JSON response, so the public API contract is unchanged and
`balance-management` remains swappable. With that column, `get_reserved_total(ref)` becomes `SUM(
amount_value) FROM reserve_balance WHERE charging_data_ref = $1` -- a real, rebuildable, SQL
aggregate -- and Valkey's `HINCRBYFLOAT` becomes what the PDF asks for everywhere else: a
performance cache of a value PostgreSQL can always reconstruct, not the only record of it.

---

### Performance and Reliability Evidence -- plan, not yet executed

Per `docs/BENCHMARK_METHOD.md`'s own discipline (method fixed before numbers exist), before any
runtime code changes in increments 1-2 below, a baseline will be committed covering, separately:
`Nchf_ConvergedCharging` Create/Update/Release, Gy CCR-I/U/T, `reserveBalance`/`adjustBalance`/
`topupBalance`, at p50/p95/p99/p99.9, throughput, error rate, CPU/RSS, PgPool wait time and
connection counts -- small and production-shaped catalogs, single-SUPI and shared-bucket
contention, and the failure cases the PDF lists (kill CHF after reserve, kill a Valkey node, restart
`postgres-chf`, replay a duplicate request). Hardware, config, and any asymmetry will be disclosed
the same way `BENCHMARK_RESULTS.md` already does for the NRF comparison. No carrier-grade, lossless,
or superiority claim will be made without these numbers, consistent with ADR-0049's own mandate and
the Reality Check section of CLAUDE.md.

### Incremental execution order (matches the PDF's own "do not do this in one change")

1. **CHF in-memory, versioned catalog/policy snapshot** (ProductOffering + ProductOfferingPrice,
   refreshed on change via a version check, startup warm-up, full rebuild after a gap) -- removes
   finding #1's N+1 SBI calls without touching balance consistency at all. Fetched through TMF620
   bulk `GET`s (the existing SBI boundary), not by reading `product_catalog`'s schema directly from
   CHF, even though both share `postgres-chf` physically -- preserves the ODA boundary.
2. **Atomic settle + the `charging_data_ref` correlator column** (Decision B) -- closes the
   unreserve/debit failure window and the Valkey-is-the-only-record gap in the same pass, since
   both touch `finalize_subscriber_balance()` and `balance-management`'s settlement path together.
   Covers all three settlement entry points by construction (one shared function).
3. **PgPool hardening + UDR pool consolidation** -- acquisition deadline, backpressure signal, and
   metrics added to `libs/nf-config/pg_pool.hpp`; UDR converted from 76 individual connections to
   one bounded `PgPool`, with a live `pg_stat_activity` count taken before/after to replace this
   ADR's static estimate with a measured one.
4. **Valkey Cluster cutover decision, migration framework, HA compose profiles** -- only after 1-3
   are measured, per the PDF's own ordering and ADR-0049's "measure before claiming."

### Open decisions -- put to the user, answered 2026-10-02

Four items were genuine choices this ADR deliberately did not make unilaterally, consistent with
this project's "stop and ask" rule for anything that changes balance consistency semantics,
fail-open behavior, schema source-of-truth, or deployment scope (the PDF's own closing line, and
CLAUDE.md's guardrails independently). Asked via `AskUserQuestion`; answers below are binding on
every increment from here on.

1. **Money precision/scale: fixed-point minor units (int64 scaled integer), not a decimal/bignum
   type and not double-with-rounding.** Binds increment 1 (the catalog/policy snapshot must hold
   prices in this representation from the start) and increment 2/3 (balance-management's columns
   and `bss_sid::Quantity`/`Money` (de)serialization must round-trip through it without ever
   routing an exact value through a C++ `double`, including at the JSON parse boundary -- nlohmann
   parses JSON numbers to `double` by default, so the wire (de)serializer for money fields needs
   its own exact-integer path, not the library default). **Scale, decided 2026-10-02: 6 decimal
   places (micro-units -- 1 unit = 1/1,000,000 of the major currency unit), the same convention
   Google Ads API's "micros" and several real telco billing engines use for sub-cent metered
   rates.** No committed catalog seed data with real per-unit rates exists in this repository to
   confirm the scale empirically against (searched `bss/product-catalog`, `tools/cdr-traffic-gen`;
   neither holds one) -- asked rather than guessed, per this project's own "ask, don't invent" rule
   when a check comes up empty. int64 range at 6dp is approximately ±9.2 * 10^12 major currency
   units, far beyond any real balance/tariff this project handles, with headroom finer than the old
   NUMERIC(20,4) constraint for any future sub-$0.0001-per-unit tariff.
2. **Atomic settle: keep the two real TMF654 calls (`ReserveBalance` then `AdjustBalance`), no
   proprietary non-TMF settle endpoint.** balance-management's public API stays strictly
   spec-shaped; the ODA swap-for-a-commercial-stack property is fully preserved. The unreserve/
   debit failure window from finding #3 is **not removed** by this decision -- it is addressed
   instead by a reconciliation mechanism (detect a bucket whose `reserved_value_amount` is non-zero
   with no live `ChargingDataRef` holding it -- made possible by Decision B's `charging_data_ref`
   correlator column -- and repair it), scoped as part of increment 2 alongside that column.
3. **Fail-open/closed: no fixed global default yet -- expose the real TS 32.291 `FailureHandling`
   enum (`TERMINATE`/`CONTINUE`/`RETRY_AND_TERMINATE`) as a per-rating-group/operator config knob.**
   No value is hardcoded as "the" default in this ADR; CHF's config schema gains the field (sourced
   from the real R19 YAML enum, not invented), and which value ships as the *config default* (as
   opposed to a mandatory value) is deferred again to the increment that wires it in, once there is
   an operator policy requirement to point to.
4. **Push:** yes -- `main` (the ADR-0444 merge plus this ADR's own documentation commit) was pushed
   to `origin/main` once this decision was given; see the process notes below for the pre-push
   state.

### Process notes

- **ADR numbering.** `project_parallel_streams` memory reserved 0400-0419/0420-0439/0440-0459 for
  three now-apparently-finished parallel streams (Tier-2 NFs / GUI / LI+Redis-cluster); HEAD's
  highest used number is ADR-0444, inside the LI/Redis-cluster stream's own range, so no collision.
  This ADR takes 0445, the next free integer, worked by this session alone -- consistent with the
  PDF's own explicit instruction ("DO NOT USE/RUN MULTI-AGENT now") and independently with
  `ListAgents` confirming no other session is currently active on this machine.
- **Leftover worktree.** `.claude/worktrees/agent-a3bcde9171223a502` (branch
  `amf-chf-redis-cluster-adr-0444`, locked, at `6f5fdaa`) is the just-finished ADR-0444 stream;
  `6f5fdaa` is already in `main`'s history. Left untouched this pass -- not this session's to
  unlock/remove without being asked.
- **Unpushed commits.** `main` is 5 commits ahead of `origin/main` (the ADR-0444 merge and its
  constituents). HEAD shows no CI run in `gh run list` because **it has not been pushed, not
  because CI failed** -- confirmed via `git rev-list --count origin/main..HEAD`. Not pushed by this
  session without being asked.
- **The source PDF.** `specs/GPT6-Model-Recommendation.pdf` is currently untracked
  (`git status`). Whether it belongs committed to a public Apache-2.0 repository is the user's call,
  not assumed here.

