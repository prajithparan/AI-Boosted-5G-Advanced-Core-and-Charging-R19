## ADR-0298: CAP mid-call re-authorization, and the field that made it impossible

**Date:** 2026-09-05. **Status:** Closed (work pushed; last citing commit bc8f219 on origin/main, 2026-10-06). accepted.

`cap_server.hpp` disclosed it plainly: "no periodic re-authorization when `maxCallPeriodDuration`
expires mid-call -- this dialogue only ever runs one InitialDP -> one ApplyChargingReport ->
close". The consequence is worse than "one report": every `ApplyChargingReport` was treated as call
end, so a call longer than one charging period was **finalized while it was still running**, and
then continued unmonitored and unbilled.

### Why it could not have worked

`CAMEL-CallResult` (TS 29.078, read from jss7's own `ApplyChargingReportRequest` ASN.1) carries:

```
timeDurationChargingResult [0] SEQUENCE {
  partyToCharge   [0] ReceivingSideID,
  timeInformation [1] TimeInformation,
  legActive       [2] BOOLEAN DEFAULT TRUE,
  ... }
```

`legActive` is the entire distinction between a periodic report and a final one, and
`libs/cap-core` did not model it. No amount of server logic could have told the two apart.

It is now decoded, with its `DEFAULT TRUE` honoured **literally**: BER omits a field carrying its
default value, so **absent means TRUE**. A decoder that defaulted it to false would silently treat
every mid-call report as call end -- the exact bug, reintroduced through the codec instead of the
server. The encoder likewise emits the field only for `false`, which is what a conformant encoder
does. Three tests pin absent-means-true, the false round trip, and a peer that sends an explicit
non-zero TRUE rather than omitting it.

### The re-authorization

On a report with `legActive == true`, CHF rates a fresh period through **the same
`chf::charge_one_usage`** the InitialDP used -- so the renewal gets its own reservation, CDR row and
RatingDecision audit rather than being invisible -- and sends another `ApplyCharging` in a
TC-Continue, leaving the dialogue open. On `legActive == false` it finalizes and closes as before.

The rating group is remembered from the InitialDP's `serviceKey`, because no later message in the
dialogue carries it and re-deriving it would mean guessing.

**Out of money mid-call**: no further period is granted and the call is left to the
`releaseIfDurationExceeded` flag on the period already issued. A real gsmSCF would typically also
send `ReleaseCall`; `cap_operations` has the encoder and nothing calls it. Disclosed, not
implemented.

### A real defect found while doing this, and not fixed here

`max_call_period_duration` is derived from `rating.grant->time` -- and **this project's rating
engine never populates `GrantedUnit.time`** (it grants `totalVolume` or `serviceSpecificUnits`
only, `charging_engine.cpp`). So every `ApplyCharging` this server has ever sent carried
`maxCallPeriodDuration = 0`, and with `releaseIfDurationExceeded = true` a conformant gsmSSF would
tear the call down immediately.

The re-authorization loop above is therefore structurally correct and **will not fire in practice
until a time-denominated grant exists**. That is the same root cause ADR-0297 named for CAP's
non-proportional finalization: no time-priced offering, so no time grant. One missing capability
explains both, and it is product/catalog configuration rather than protocol work. Stated here
rather than left for someone to discover when a call drops at answer.

