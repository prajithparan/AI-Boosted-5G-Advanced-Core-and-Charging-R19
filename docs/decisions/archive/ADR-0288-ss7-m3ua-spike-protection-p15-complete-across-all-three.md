## ADR-0288: SS7/M3UA spike protection -- P15 complete across all three protocols

**Date:** 2026-09-05
**Status:** Closed (work pushed; last citing commit d22e609 on origin/main, 2026-10-06). Accepted

The last protocol front door. `CapServer::set_tps_limit` gives CHF's CAP/gsmSCF listener its own
token bucket, checked on each received M3UA message **before** the SCCP unwrap and the TCAP/CAP
decode, which is where the real work is.

Its own bucket, not shared with SBI's or Diameter's: a CAP storm and an SBI storm are different
failures, and P15 asks for **per-protocol** governance.

### A shed here DROPS rather than answers, and that is deliberate

The SBI ceiling returns `503` and the Diameter ceiling returns an answer message. This one drops
the message, for two reasons that are about protocol shape rather than convenience:

1. A TCAP answer is not a status line. It is a TC-Abort inside a correctly addressed SCCP/M3UA
   envelope quoting the peer's own transaction id -- which means decoding the very message being
   shed and building a reply nearly as expensive as serving it. A shed that costs as much as
   service is not protection.
2. Dropping is what a real SS7 node does under congestion. TCAP dialogues are protected by the
   peer's own invoke timers; that mechanism exists for exactly this case.

### P15 status after this ADR

| Front door | Ceiling | Shed behaviour |
|---|---|---|
| SBI (all 22 servers) | ADR-0280 | `503` + `ProblemDetails` + `Retry-After` |
| Diameter (Gy/Rf/Sy) | ADR-0285 | answer with a verified Result-Code (see its own caveat) |
| SS7/M3UA (CAP) | this ADR | drop; peer's TCAP invoke timer recovers |

All three are **off unless configured**. What P15 still does not have, unchanged by this ADR: a
load campaign. Every ceiling is validated as a mechanism, and `docs/COMPLIANCE_P1_P15.md` says so.

---

