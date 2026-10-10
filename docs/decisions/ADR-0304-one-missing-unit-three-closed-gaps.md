## ADR-0304: one missing unit, three closed gaps

**Date:** 2026-09-05. **Status:** accepted. **Implements:** ADR-0300's C2.

`build_rating_grant` mapped `unitOfMeasure` "GB" and "MB" to `totalVolume` and **everything else**
to `serviceSpecificUnits`. So a per-minute voice price silently became a service-unit grant, and
**`GrantedUnit.time` was never populated anywhere in this project**.

Three gaps disclosed separately, in three different ADRs, all traced to that one line:

1. **CAP's `maxCallPeriodDuration` was always 0** (ADR-0298). It derives from `grant->time`. With
   `releaseIfDurationExceeded = true`, a conformant gsmSSF would tear every call down at answer.
2. **CAP could not finalize proportionally** (ADR-0297). `ApplyChargingReport` reports elapsed
   TIME; the grant was in volume; no seconds-to-octets conversion exists to bridge them, and
   inventing one would have produced wrong money.
3. **Time-based bundles were Partial** in README's product table.

None of the three was a protocol problem. All three were this.

### The change

`SEC`/`SECOND(S)`/`S`, `MIN`/`MINUTE(S)`, `HOUR(S)`/`HR`/`H`, `DAY(S)` -- matched
case-insensitively against the operator's own TM Forum `unitOfMeasure.units` string -- now produce
a real `GrantedUnit.time`. Everything converts to SECONDS because that is what TS 32.291's
`GrantedUnit.time` is (Uint32 seconds); carrying minutes in a seconds field would be the kind of
unit error that is invisible until a bill is wrong.

`ChargingDataStore` gained `granted_time` alongside `granted_volume` and
`granted_service_units`, and `proportional_debit` gained time as a **third independently-weighted
dimension** -- never summed with the others, because octets, service units and seconds are not
commensurable. CAP now proportions its debit by the elapsed seconds the gsmSSF actually reported: a
15-second call on a 60-second grant is charged a quarter of the reservation.

### What is deliberately unchanged

When a session holds **no** time grant -- a volume-priced offering charged over CAP, which nothing
prevents an operator from configuring -- the dimension mismatch ADR-0297 described is real and
unchanged, and the full reservation is still finalized. The old behaviour is preserved for exactly
the case that used to be the only case, rather than pretending the mismatch is gone. The same is
true of Diameter Gy, which reports octets and has no time counterpart to report.

Backward compatibility is pinned by test: the two-dimension `proportional_debit` call behaves
exactly as it did, and an offering with no duration unit is unaffected.

### Product-table consequences

`README.md`: **Time-based bundle** moves Partial -> **Supported**. **Voice + data** keeps its
Partial for the reason that actually remains -- no combined allowance across rating groups (C4) --
rather than the duration reason, which is now fixed. A new **Slice-/UPF-/DNN-scoped bundle** row
records ADR-0303's capability, including that it is N40-only.

547/547 against a verified-current build.

