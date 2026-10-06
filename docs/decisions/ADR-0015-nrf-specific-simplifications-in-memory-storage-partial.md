## ADR-0015: NRF-specific simplifications (in-memory storage, partial discovery/subscription semantics)

**Date:** 2026-08-05
**Status:** Accepted (disclosed, tracked)

Consolidates the Phase 2 NRF-specific gaps that don't warrant their own ADR individually but need
to be findable in one place rather than scattered only as code comments:

- **In-memory storage only** (`nfs/nrf/src/registry.hpp`'s `NfRegistry`/`SubscriptionRegistry`),
  no persistence across restarts. A restart loses every registered NF and subscription; every NF
  would need to re-register (which real NFs do periodically via heartbeat anyway, so this is a
  softer gap than it sounds for NFs already running, but a hard one for anything mid-flight during
  a restart).
- **`SearchNFInstances` filters on `target-nf-type` only.** `TS29510_Nnrf_NFDiscovery.yaml`'s real
  query parameter set is much larger (service-names, snssais, dnn, requester-nf-instance-fqdn,
  ...); only the one mandatory discriminating parameter needed for a single-NRF lab to be useful
  is implemented. Real per CLAUDE.md's source-of-truth rule -- not a fabricated subset, a
  disclosed incomplete one.
- **Subscription notification fan-out ignores `subscrCond`.** `SubscriptionData.subscrCond` is a
  real, conditionally-typed filter (which NF types/events a subscriber cares about); this
  implementation delivers every `NF_REGISTERED`/`NF_PROFILE_CHANGED`/`NF_DEREGISTERED` event to
  every active subscriber regardless of what they actually asked for.
- **Notification delivery is synchronous best-effort** (log a warning on failure, no retry) --
  consistent with ADR-0006's synchronous HTTP/2 client; a slow or unreachable subscriber blocks
  the NRF request that triggered the notification for the duration of that one delivery attempt.
- **`GetNFInstances` (list-all) has no pagination** -- `TS29510_Nnrf_NFManagement.yaml` doesn't
  mandate it for this operation, but a very large registry would return an unbounded response
  body; not a problem at lab scale, flagged for whenever it might become one.
- **`/shared-data*` (multi-NRF federation) and `/scp-domain-routing-info*` (SCP-specific) are not
  implemented at all** -- explicitly out of scope for this pass per the agreed procedure list, not
  silently dropped from consideration. No second NRF instance exists in this lab and SCP isn't
  built yet, so neither is exercisable regardless.

---

