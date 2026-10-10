## ADR-0385: balance-management cut over to the consolidated charging DB -- lossless, NULL-safe, and reachable by the CHF

**Date:** 2026-09-25. **Status:** accepted (user-directed: "proceed with balance management
cut-over, don't wait"). Second service onto the `charging` DB after product-catalog (ADR-0384).

**Found before cutting over (each would have broken charging for onboarded customers):**
1. **The CHF could never find a provisioned customer's bucket.** The CHF resolves a subscriber's
   bucket as `GET /bucket?relatedParty.id=<SUPI>` (shared bucket) else `bucket.id == SUPI`
   (`charging_engine.cpp resolve_bucket_id`, every reserve); `bss/provisioning` (ADR-0382) created
   `bkt-<digits>` ids. Fixed by keying the subscriber's own bucket by SUPI in provisioning (the older,
   wider convention: CHF fallback, CAP test, lab data) -- the SID `bucket_logical_resource` link is
   still written.
2. **NULL amounts.** `40-balance.sql` left `remaining/reserved_value_amount` NULLable and provisioning
   omits the reserved amount; `NULL + x` is NULL, so the first reserve on every provisioned bucket
   would have "succeeded" while corrupting it. Now `NOT NULL DEFAULT 0` (backfilled).
3. **Precision**: `NUMERIC(20,4)` held less than the old store's 6 decimals -> unbounded `NUMERIC`.
4. **Account FK**: `bucket.party_account_id -> subscriber_mgmt.account` would reject top-up
   auto-creation for every account subscriber-management still creates in its OLD per-service DB.
   Dropped here; **re-add it in the subscriber-management cut-over** (recorded there too).
5. **CI gap (pre-existing)**: `TEST_BALANCE_POSTGRES_URL` was never set in CI, so
   `test_balance_shared_bucket.cpp` has been skipping there. Now set.

**Decision.**
- `bss/balance-management` persists TMF654 in schema `balance_mgmt` of `charging`; new idempotent
  `deploy/db/charging/41-balance-lossless.sql` adds what the DTOs need; schema-qualified SQL.
- **Access-path split, deliberately:** the **Bucket** (read and mutated on every charge) is
  normalized (scalar columns + ordered `bucket_logical_resource` / `bucket_product` /
  `bucket_related_party`). The **balance events** (topup/adjust/reserve; append-only, partitioned by
  `occurred_at`, one reserve per rating decision -- hundreds of millions a day at the Tier-1 target)
  keep their TMF654 reference lists as JSONB columns on the event row: one INSERT per event, no
  child-table fan-out on the hottest write path. Scalars are columns; `id` gets its own index (the PK
  is `(occurred_at, id)`, so a lookup by id would otherwise scan every partition).
- `bucket_related_party(party_id)` indexed -- the CHF's shared-bucket lookup precedes every reserve.
- **Semantics unchanged:** every movement is one conditional UPDATE (`WHERE remaining >= amount`,
  row lock = concurrency safety); insufficient balance is a business outcome (201, status
  `failed`), not an error; top-up of an unknown bucket creates it -- now as ONE atomic upsert
  (`INSERT ... ON CONFLICT DO UPDATE ... RETURNING (xmax = 0)`), removing a create race the old
  UPDATE-then-INSERT had. A malformed date-time is a 400 and rolls back the balance movement.
- Wiring: config, compose (depends on postgres-chf), both CI jobs' `BALANCE_MANAGEMENT_DATABASE_URL`
  and the new `TEST_BALANCE_POSTGRES_URL` -> `postgres-chf/charging`.

**Rejected.** *CHF resolves by `logicalResource.id`* (SID-proper) -- a second round trip on every
reserve; recorded as the follow-up once one subscriber needs several buckets (id = SUPI allows one
own bucket per subscriber, so separate data/voice/monetary buckets are not yet possible).
*Normalize event reference lists into child tables* -- write amplification on the hottest path.
*Keep the account FK* -- blocks every account subscriber-management owns until its own cut-over.
*COALESCE in the hot SQL* -- hides the NULL instead of making it impossible.

**Measured** (same machine, 60 iterations of the CHF's per-charge pair, curl over fresh TLS each):
shared-bucket lookup p50 5.5 ms (old) / 5.6 ms (new), p90 7.9 / 7.2; reserve p50 6.6 / 6.4, p90
8.4 / 9.2 -- equivalent; no regression.

**Disclosed.** The old per-service balance DB held 2 lab buckets (1 SUPI-keyed); not migrated,
nothing seeds them. `accumulated_balance` stays unused (computed on the fly, as before). Old reads
dropped `confirmationDate`/`partyAccount` on events; they now round-trip. Remaining on per-service
DBs: subscriber-management, roaming-interconnect, CHF `chf_rating`.

**Tests.** New `tests/integration/test_balance_lossless.cpp`: fully populated top-up/adjust/reserve
round-trip exactly via GET (top-up amount beyond 6 decimals exact; created bucket carries the whole
PartyAccountRef + products); unchanged semantics (credit, reserve, over-reserve fails untouched,
release, adjust floor, accumulated sum); a provisioning-shaped bucket reserves correctly (the NULL
case); 32 parallel reserves of 5 on 100 -> exactly 20 succeed, remaining 0, reserved 100; malformed
date rejected with the balance unmoved. `test_balance_shared_bucket` moved to the normalized
relatedParty table. Verified locally 2026-09-25 end to end with real processes (nrf, udr,
product-catalog, balance-management, provisioning, chf, Doris): a customer onboarded through
provisioning on a TMF620 offering got a CHF grant of 1,048,576 octets on rating group 20, and 0.01 USD
was reserved from THEIR bucket (remaining 5.00 -> 4.99). Locally BalanceLossless*, SharedBucket*,
ProductCatalog*, UdrOamProvisioning*, CapScopedCharging: 20/20.

