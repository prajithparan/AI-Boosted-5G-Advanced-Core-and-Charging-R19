## ADR-0386: subscriber-management cut over to the consolidated charging DB -- TMF632 party lossless, SUPI/MSISDN as SID Resources, account FK restored

**Date:** 2026-09-26. **Status:** accepted (continuing the user-directed charging-DB cut-over,
ADR-0384/0385). Third service onto `charging`.

**Decision.**
- `bss/subscriber-management` persists TMF632 Individual / Organization in schema `party`
  (normalized, `10-party.sql`) and the project's Account / Subscriber in `subscriber_mgmt`
  (`20-subscriber.sql`), replacing its per-service JSONB rows. New idempotent
  `21-party-subscriber-lossless.sql`: `birthDate`/`deathDate` as TIMESTAMPTZ (was DATE; TMF632 carries
  a date-time), `href` on relatedParty and on parent/child organization refs, surrogate keys with the
  optional TMF ids of TaxExemptionCertificate / TaxDefinition in their own columns, `ordinal` on every
  TMF632 list, id sequences, and `subscriber_lifecycle_event` (the transition history the service
  records, which `20-` had no home for).
- **Subscriber SUPI / MSISDN are SID Resources** (`subscriber_mgmt.resource` rows, unique while
  active), not columns; `get_by_supi` goes through `idx_resource_lookup`. A SUPI that is already an
  active resource is a **409**.
- **Stricter by design** (the relational model's NOT NULL/FK/CHECK): a subscriber needs an existing
  `accountId` and a `chargingMode` (**400** otherwise); unknown individual / organization / parent
  account / parent organization -> **400**; `accountKind` outside CONSUMER|ENTERPRISE -> **400**.
  API-created subscribers start `active`, as before (the schema default `pendingActive` belongs to the
  provisioning workflow).
- **ADR-0385's deferred FK is restored:** `42-balance-account-fk.sql` re-adds
  `balance_mgmt.bucket.party_account_id -> subscriber_mgmt.account` (NOT VALID: new rows enforced,
  pre-existing lab rows not retro-checked); balance-management maps an unknown account on top-up to
  **400**.
- Wiring: config, compose (depends on postgres-chf), both CI jobs' `SUBSCRIBER_MANAGEMENT_DATABASE_URL`
  / `TEST_SUBSCRIBER_MANAGEMENT_POSTGRES_URL` -> `postgres-chf/charging`.

**Found and fixed along the way.** `SUBSCRIBER_MANAGEMENT_TEST_URL` was never set in CI, so
`test_subscriber_lifecycle.cpp` has **always skipped** (the second such silent skip after ADR-0385's
balance one); now set, and the test asserts the recorded history. `test_subscriber_management_postgres`
used a fixed SUPI and only ever passed on a fresh CI database; now unique per run.

**Rejected.** *Keep SUPI/MSISDN as subscriber columns* -- duplicates the SID Resource the
provisioning workflow and the rest of the domain already use. *Keep the bucket FK dropped* -- the
reason (accounts in another DB) is gone. *VALIDATE the restored FK now* -- lab rows from earlier
tests reference accounts that never existed; enforced for new rows, validation left as a clean-up step.

**Disclosed.** Stores keep the one-connection-plus-mutex model (no pool yet; not on a hot path --
nothing calls subscriber-management per charge). The old per-service DB's lab rows were not migrated.
Canonicalisations as in ADR-0384 (UTC date-times with milliseconds; all-absent objects come back
absent). Remaining on per-service DBs: roaming-interconnect and the CHF's `chf_rating`.

**Tests.** New `tests/integration/test_party_lossless.cpp`: fully populated Individual and
Organization (every field, every list >= 2, parent/child refs, tax certificates with definitions)
round-trip exactly via get() and list(); integrity violations (unknown parent org, bad accountKind,
subscriber without / with an unknown account, malformed date) are client errors. Balance: top-up for
an unknown account -> 400. Verified locally 2026-09-26: PartyLossless, SubscriberManagementPostgres,
SubscriberLifecycle (with its env var), BalanceLossless, SharedBucket -- 19/19; service-level POST
account/subscriber 201, duplicate SUPI 409, no account 400; a provisioning order still completes with
the restored FK.

