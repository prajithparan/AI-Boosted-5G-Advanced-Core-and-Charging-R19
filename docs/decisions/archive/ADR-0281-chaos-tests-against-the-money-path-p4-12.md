## ADR-0281: chaos tests against the money path (P4.12)

**Date:** 2026-09-05
**Status:** Closed (work pushed; last citing commit c6d460a on origin/main, 2026-10-06). Accepted

P4.12 asks for "chaos tests (kill a node mid-session, partition the balance store) asserting no
double-charge and no lost usage". Both scenarios now exist as real tests against real processes.

### What the words can actually mean here, stated before the assertions

- **no lost usage** = a charging session that existed before a hard kill is still chargeable after.
  CHF's `ChargingDataStore` is Redis-backed, so a killed CHF must come back able to Update the same
  `ChargingDataRef`.
- **no double charge** = the operations that move money are not repeatable. Release succeeds once
  and 404s on repeat; an unknown ref 404s rather than being silently adopted as a new session.
- **partitioned balance store** = CHF must not record a reservation it could not make.

### Chaos 1: SIGKILL mid-session

`SIGKILL`, not `SIGTERM` -- a crashed NF gets no chance to flush, which is the case worth testing.
A charging session is created, CHF is killed, a replacement CHF starts against the same Redis, and
the test asserts: the session is still updatable (no lost usage), Release succeeds once, a second
Release 404s (no double charge), and an unknown ref 404s.

### Chaos 2: partitioned balance store

CHF is pointed at a port with nothing on it -- what a partition looks like from CHF's side -- and
must (a) keep answering rather than hanging or dying, and (b) not produce a phantom reservation.
The code path this pins already behaved correctly: `reserve_subscriber_balance` returning false
means `add_reserved` is never called, so no quota is recorded against money that was never held.
The test exists so that stays true.

### Disclosed

CHF's `reserve_rejected` counter and its "reserveBalance call failed" log are the direct evidence
for chaos 2, and neither is assertable over the API -- the test asserts the externally visible
behaviour and this ADR names the rest rather than implying a stronger check. Chaos coverage is
CHF-centric: killing SMF or UPF mid-session is not covered.

---

