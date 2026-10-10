## ADR-0349: three silent defects between "detected" and "stored"

**Date:** 2026-09-12. **Status:** Closed (work pushed; last citing commit 538b98b on origin/main, 2026-10-06). accepted.

ADR-0344 detected all 25 charging-information types. Bringing up a clean environment for the 2M/50k
regeneration found three defects between detecting a thing and it actually being in the data —
every one of which produced a plausible-looking run:

1. **`recorded_date` was NOT NULL with no default and the INSERT never set it.** Caught by reading
   the write path before launching, not by a failed run. Had it launched, every write would have
   failed for hours.
2. **The column list and the value tuple disagreed** — 18 columns against 19 values, because the
   column-list edit did not stick while the value edit did. Doris reported *"Column count doesn't
   match value count"* and **lost every batch**. The generator meanwhile reported 598 sessions
   released and 0 failures: it had no idea the CDRs never landed.
3. **`charging_information_type` was populated as a rating attribute but never written to the
   record.** The 25 types were detected, used for scoping, and then dropped at the last step — the
   exact bug ADR-0344 set out to fix, one layer further down.

`recorded_date` is derived from `invocation_time_stamp`, not wall-clock now: a late-delivered CDR
filed under today would be invisible to a query for the day it actually happened.

### The environment defects were as damaging as the code ones

Restarting Postgres to apply tuning **silently killed product-catalog and left balance-management
holding a dead connection**. Neither process exited; both kept listening. The symptom was 469
successful ratings with **zero** grants recorded, because a grant is only stored when the
reservation also succeeds — and the reservation was failing against a service that looked up.

That is worth recording as a pattern: a dependent service that survives its database going away,
and answers with empty results rather than errors, is indistinguishable from a correctly-working
service returning nothing. The only thing that caught it was querying the bucket by hand and
getting an empty array for a row that demonstrably existed.

### Verified before committing to a multi-hour run

| type | CDRs | granted | with cost |
|---|---:|---:|---:|
| PDUSession | 7,810 | 6,200 | 7,720 |
| SMS | 168 | 166 | 166 |
| MMS | 84 | 83 | 83 |
| (Release records) | 2,014 | — | 1,987 |

Every charging type now rates, reserves and is stored with the service it belongs to.

### Resources raised before the run

Postgres was entirely at defaults, which is where the pipeline's ceiling actually sat:
`shared_buffers` 128 MB -> **1 GB**, `work_mem` 4 -> 16 MB, `max_connections` 100 -> **300** (eight
CHF instances plus BSS services approach the old limit), `max_wal_size` 1 -> 4 GB. Doris was never
resource-limited -- it had the whole host and was idle at 16% while the pipeline was serialised
elsewhere, which is why adding resources there would not have helped and fixing an index did.

