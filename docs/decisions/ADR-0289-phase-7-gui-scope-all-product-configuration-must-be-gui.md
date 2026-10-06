## ADR-0289: Phase 7 GUI scope -- all product configuration must be GUI-editable

**Date:** 2026-09-05
**Status:** Accepted (user decision)

**User decision, 2026-09-05.** Two assignments recorded so they are not lost between phases:

1. **The N28/Sy `policyCounterId` GUI** (the remaining half of the 2026-08-16 directive, ADR-0286)
   is implemented during **Phase 7**, not before.
2. **Every product configuration surface must be editable from the GUI.** No product, tariff,
   quota, throttle or partner change may require hand-editing a file or a database.

### The concrete surfaces that commits to

| Surface | Where it lives today | API it is already behind |
|---|---|---|
| Product offerings and prices, including `prodSpecCharValueUse` (`ratingGroup`, `validityTime`, `quotaHoldingTime`, volume/time/unit quota thresholds, `unitOfMeasure`) | `bss/product-catalog` + PostgreSQL | TMF620 |
| Balance buckets and top-ups | `bss/balance-management` + PostgreSQL | TMF654 |
| N28 spending-limit mapping (`policy_counter_actions`) | `config/pcf.json` | read at PCF startup (ADR-0286) |
| Slice admission quotas (`max_ues`, `max_pdus`) | `config/nsacf.json`, and `LocalNumberUpdate` at runtime | TS 29.536 `Nnsacf_NSAC` |
| Interconnect/roaming agreements | `bss/roaming-interconnect` + PostgreSQL | TMF651 |

### Why this is a rendering problem, not a re-architecture

Every surface above is **already** JSON-shaped data behind a real API, because of principle P7
("product/tariff/policy is data, never code") having been applied as each was built -- most
recently to the N28 mapping, which existed as a gap precisely because TS 29.594 leaves
`currentStatus` unspecified and no rule could honestly be written in C++.

Two consequences follow, and both are load-bearing for Phase 7's estimate:

- The GUI needs **no new business logic**; it needs schema-driven forms over existing APIs, which
  is exactly what the brief's "JSON-schema-driven GUI" already calls for.
- Two of the five surfaces are **config files rather than APIs today** (`policy_counter_actions`,
  NSACF quotas). Those need a real read/write API before a GUI can edit them at runtime -- named
  here rather than discovered in Phase 7. NSACF's already has a spec-defined runtime path
  (`LocalNumberUpdate`); PCF's does not.

---

