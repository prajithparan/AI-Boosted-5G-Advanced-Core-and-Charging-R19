## ADR-0307: shared / family / group buckets -- the standard already had the field

**Date:** 2026-09-06. **Status:** accepted. **Implements:** ADR-0300's C1.

This was the one commercial product README listed as outright **Not supported**: balance buckets
were keyed by SUPI (`bucket.id = supi`), so several subscribers could not draw down one allowance.

The notable thing is how little was needed. **TMF654's `Bucket` already carries `isShared` and
`relatedParty`**, and `bss/balance-management/schema.sql` already had both columns
(`is_shared BOOLEAN`, `related_party JSONB`). A shared bucket is one with `is_shared = true` whose
`related_party` array names its members. No new resource, no schema migration, and no invented
concept -- "we used the mechanism the standard already has" is a materially different claim from
"we designed a group-bucket feature", and the difference matters for anyone integrating a
commercial BSS later.

The lookup is exposed through TM Forum's own dot-path collection filter,
`GET /bucket?relatedParty.id={supi}`, for the same reason.

### Three deliberate choices

- **The `status` check lives in the store**, not at the call sites. An expired or suspended family
  allowance silently absorbing a month of usage is a billing error, and pushing that check outward
  is how one of several call sites eventually forgets it.
- **Matched in SQL** (`related_party @> $1::jsonb`), not by scanning buckets in C++. This runs on
  every reservation; a full table scan per charging request is a real cost at subscriber scale.
- **Lookup failure falls back to the subscriber's own bucket.** A balance-management hiccup then
  charges the subscriber's own balance -- which may legitimately fail for funds -- rather than
  silently charging nothing. Critically it never charges a DIFFERENT family's bucket, which is the
  unrecoverable direction for this feature to fail in.

CHF resolves the bucket in **both** `reserve_subscriber_balance` and
`finalize_subscriber_balance`. Resolving in only one would reserve against the family and debit the
individual, or the reverse.

### Evidence

Four tests against **real PostgreSQL**, run for real rather than deferred: two members of one
family resolve to the same bucket (the fact that *is* the product), a non-member does not resolve
to someone else's family bucket, a personal bucket naming its own owner is not mistaken for a group
bucket, and an expired shared bucket is refused. 560/560 overall against a verified-current build.

### A latent hazard tripped while doing this

`bss/product-catalog`, `bss/balance-management` and several NFs each define a `store.hpp`. They
coexist only because no single target has two of their source directories on its include path.
Adding balance-management's to `integration_tests` silently redirected every `#include "store.hpp"`
in that binary -- and the failure surfaced in `test_product_catalog_postgres.cpp`, a file this work
never touched. The test now includes the header by explicit relative path, with a comment saying
why, so the next person to add a component's store here does not repeat it.

