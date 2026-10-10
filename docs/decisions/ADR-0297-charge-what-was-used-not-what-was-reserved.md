## ADR-0297: charge what was used, not what was reserved

**Date:** 2026-09-05. **Status:** accepted. **Closes:** ADR-0057's carried-forward simplification.

`finalize_subscriber_balance` unreserved the session's total and debited **the same number**. A
subscriber who used 1 GB of a 5 GB reservation was charged for 5 GB. This was disclosed --
ADR-0057 said so, `charging_engine.hpp` said so, and `main.cpp`'s Release handler said so -- and
being disclosed is not the same as being acceptable. It is a live over-billing defect on every
`Nchf_ConvergedCharging` Release and every Diameter Gy CCR-Termination.

It survived this long because the disclosure made it *look* handled. That is worth naming: a
comment describing a wrong behaviour accurately is still a wrong behaviour.

### The fix

`finalize_subscriber_balance` now takes `amount_to_debit` **separately from** `total_reserved`.
That separation is the whole change. The reservation is still released in full -- it is what the
subscriber's balance is actually holding -- but only the consumed part becomes a permanent debit.

`proportional_debit()` computes the consumed fraction:

- Volume and service-specific units are proportioned **independently against their own grants**,
  never summed. They are not commensurable; adding octets to service units gives a ratio that means
  nothing. This project's rating engine grants exactly one dimension per rating group, so a real
  session exercises one branch -- the mixed case is handled rather than assumed away.
- Clamped to `[0, total_reserved]`. Usage beyond the grant cannot debit more than was reserved,
  because the balance never held more than that; debiting more would take money never authorised.
- **No grant recorded falls back to the FULL reservation, not to zero.** An unmeasurable session is
  not a free session, and silently zero-rating one would turn a data gap into revenue loss that
  nothing would ever surface.

### What had to be recorded to make it possible

Money alone cannot say what fraction of a grant was consumed, so `ChargingDataStore` now
accumulates the granted **units** beside the reserved money -- `granted_volume` and
`granted_service_units`, same Redis hash, same atomic `HINCRBYFLOAT`. They are written at the one
moment grant and cost are known together (inside `charge_one_usage`, right after the reservation
succeeds). Reconstructing the grant at Release instead would mean re-rating against a catalog that
may have changed since the grant was issued.

### Wired on two paths, deliberately not on the third

- **HTTP Release**: sums `usedUnitContainer` `totalVolume`/`serviceSpecificUnits` across every
  rating group in the request -- values this handler already decoded and wrote to the CDR, then
  ignored when charging.
- **Diameter Gy CCR-T**: sums `Used-Service-Unit` → `CC-Total-Octets` per MSCC, RFC 4006's own
  final-usage report and the direct equivalent of TS 32.291's `totalVolume`. Also already decoded
  and used only for the CDR.
- **CAP `ApplyChargingReport`: NOT proportioned, on purpose.** The rating engine grants
  `totalVolume` or `serviceSpecificUnits`, never `time`; `ApplyChargingReport` reports elapsed
  TIME. Proportioning one by the other needs a seconds-to-octets conversion that no price, config
  or specification in this project defines. Inventing one would silently produce wrong money, which
  is worse than the honest over-charge it would replace. CAP therefore still finalizes the full
  reservation, and the real fix is a time-denominated grant (`GrantedUnit.time` is never populated
  anywhere) driven by a time-priced offering -- product configuration, not codec work.

### Evidence

`proportional_debit` was split into its own translation unit specifically so it could be tested:
`charging_engine.cpp` drags in the HTTP/2 client, CDR writer, rating-decision store and ONNX quota
sizer, and a test that checks a division should not link all of that. Eight tests state the money
cases as money -- the fifth-of-a-grant case, full consumption, over-usage not overdrawing, zero
usage, the no-grant fallback, service units, the mixed-dimension weighting, and a non-positive
reservation.

`README.md`'s commercial-products table is updated in the same commit: prepaid billing now states
that finalization is proportional, and the voice and time-based rows now carry the CAP consequence
(a CAP-charged call is billed its whole grant regardless of duration) and name the missing
duration-denominated grant as the one piece that would close it.

