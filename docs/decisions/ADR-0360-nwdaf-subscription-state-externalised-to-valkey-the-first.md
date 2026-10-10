## ADR-0360: NWDAF subscription state externalised to Valkey -- the first replicable NWDAF

**Date:** 2026-09-14. **Status:** accepted. Step 0 of ADR-0359's build order.

**What changed.** Phase A (ADR-0358) kept `Nnwdaf_EventsSubscription` subscriptions and transfers
in a process-local `unordered_map` behind a mutex. That is replaced by `nfs/nwdaf/src/
subscription_store.{hpp,cpp}`, a Valkey-backed store on redis-plus-plus -- the same client and
the same "hot-path state, not a system-of-record" pattern the AMF (`ue_security_context_store`)
and CHF (`stores.cpp`) already use. Keys: `nwdaf:sub:<id>` / `nwdaf:transfer:<id>` hold the
resource JSON in the YAML's own shape; `nwdaf:subs` / `nwdaf:transfers` are index sets the
notifier enumerates (never `KEYS`); `nwdaf:next_id` is an `INCR` counter, so IDs are unique across
replicas with no coordinator. PUT uses `SET ... XX` (replace-only-if-present) so a PUT racing a
DELETE on another replica cannot resurrect a deleted subscription, and a PUT on an unknown id is
a 404, never an upsert. The URL comes from `config/nwdaf.json` `redis_url` / `NWDAF_REDIS_URL`.

**Proof.** `tests/integration/test_nwdaf_phase_a.cpp` `SubscriptionsAreSharedAcrossReplicas`
starts two NWDAF processes (the second on port 7799 via the env overrides an operator would use)
against one Valkey: a subscription created on A is updated (200) and deleted (204) through B, after
which A reports it gone (404). Against the Phase A map that test fails at the first PUT.

**Found on the way, fixed.** ADR-0356 renamed the compose service `redis` -> `valkey`, but left
`AMF_REDIS_URL` and `CHF_REDIS_URL` pointing at `tcp://redis:6379` -- a hostname that no longer
resolved inside the compose network. Both now point at `valkey`, as does the NWDAF's. The lab had
not been brought up from scratch since the rename, which is how it went unnoticed; the
architecture-diagram sync check (ADR-0356) audits the diagram against the compose file, not the
env strings inside it -- a gap in that check, noted, not yet closed.

**Also fixed here, in sbi-core.** A handler that threw (which is what a Valkey client does
when its connection drops mid-request) escaped `io_context::run()` and terminated the NF -- every
NF, not just this one. The server now catches at the API boundary (the one place CLAUDE.md allows
exceptions) and answers that request with a 500 ProblemDetails, cause `SYSTEM_FAILURE` (TS 29.500
V19.7.0 Table 5.2.7.2-1: "generic error condition in the NF"). No expiry is set on the keys: a
subscription lives until its consumer deletes it, which is the resource's YAML semantics; the
notifier prunes index entries whose value is gone.

**Not state, so not moved.** The feature-store reader is a stateless Doris connection (its mutex
guards the socket, not data); the NRF registration is per-instance by design (each replica has
its own instance id and profile, which is what lets the NRF load-balance across them).

**Still process-local, disclosed.** The notifier thread runs in every replica, so with N replicas
each subscription is notified N times per interval. Partitioning the notifier (per-replica claim
on `nwdaf:sub:<id>` with a TTL lease) is the next piece of this store and is scheduled with the
`Nnwdaf_DataManagement` work, where the notification path is rebuilt anyway. Until then, run one
replica or accept duplicate notifications -- a consumer keyed on `notifCorrId` de-duplicates them
correctly, but that is a workaround, not the design.

**Rejected.** *Sticky sessions at the load balancer* (route every request for an id to the replica
that created it): hides the problem, breaks the moment that replica restarts, and the NRF-based
selection TS 23.288 describes has no such affinity. *PostgreSQL for subscriptions:* durable but
the wrong shape for a keyed, high-churn set; Valkey is what every other NF's hot-path
state already uses, and one store per kind of state is part of ADR-0359's point.

