# Valkey keyspace design (charging + provisioning + NF state)

Valkey is key-structured, not tabular. This is the authoritative keyspace + sharding spec, designed
for Tier-1 scale (project_tier1_scale_architecture): **Valkey Cluster** (16384 hash slots across
nodes) with a **`{supi}` hash-tag** on per-subscriber keys so one subscriber's keys colocate in one
slot (enabling multi-key ops + even distribution), TTLs on caches, and replica-per-primary for HA.
No code may assume a single Valkey node.

## 1. Hot read caches — offload Postgres for digital-channel reads
The perf analysis showed per-customer Postgres reads are index-scanned sub-ms; these caches cut even
that round trip for the hottest reads and absorb read spikes.
| Key | Type | Contents | TTL | Invalidated by |
|---|---|---|---|---|
| `chg:sub:{supi}`            | hash | subscriber profile (account, charging_mode, status, offering) | 1h | provisioning / status change |
| `chg:bal:{supi}`            | hash | current balance per usage_type (remaining/reserved)          | 5m | balance txn (reserve/debit/topup) |
| `chg:cust:{accountId}`      | hash | customer-360 summary (subscribers, products, bill state)     | 15m| provisioning / bill run |
| `chg:offer:catalog`         | hash | sellable product-catalog snapshot (small, all instances)     | 1h | catalog change |

## 2. CHF operational state (existing)
| Key | Type | Contents |
|---|---|---|
| `chf:session:{ref}`         | hash | live charging session (granted/reserved units) |
| `chf:cdr:next_id`           | string(INCR) | ChargingDataRef sequence |
| `chf:idem:{key}`            | string | request idempotency |

## 3. Provisioning / orchestration (project_customer_onboarding_orchestration)
| Key | Type | Contents |
|---|---|---|
| `prov:order:{orderId}`      | hash | live order state cache (source of truth = orchestration DB) |
| `prov:idem:{idemKey}`       | string | provisioning-task idempotency (at-most-once across nodes) |
| `prov:resource:msisdn:pool` | set/list | free MSISDN pool for allocation |
| `prov:resource:imsi:pool`   | set/list | free IMSI/SUPI pool |

## 4. Externalized NF state (existing; P11)
`amf:*`, `smf:*`, `pcf:*`, `nwdaf:*`, `dccf:*`, `mfaf:*`, `adrf:*` — each NF's state, keyed so a
subscriber/session's keys carry the `{supi}`/`{sessionId}` hash-tag for slot colocation.

## Sharding rules
- **Hash-tag** the subscriber identifier in per-subscriber keys: `chg:sub:{imsi-99970...}` — the
  brace-delimited part is the only thing hashed, so all of one subscriber's keys land in one slot.
- Global/small keys (catalog snapshot, sequences) may live anywhere; keep them few.
- Cluster grows by adding primaries + resharding slots; replicas per primary for HA/failover.
- TTL every cache; the DB (Postgres/Doris) is the source of truth, Valkey is acceleration only.
