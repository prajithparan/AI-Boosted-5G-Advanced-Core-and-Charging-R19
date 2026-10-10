## ADR-0285: a TPS ceiling for the Diameter front door -- and one value this project will not invent

**Date:** 2026-09-05
**Status:** Accepted, with an open question for the user

ADR-0280 gave every SBI server a TPS ceiling and disclosed that Diameter and SS7 had none. This
closes the Diameter half: `DiameterServer::set_tps_limit`, checked once a message is known to be a
supported request (an unsupported command is a peer error, not load) and before the AVPs are
decoded and the charging engine entered, because shedding is only protective if it is cheaper than
serving.

Separate bucket from SBI's, deliberately: P15 asks for **per-protocol** governance, and a shared
budget would let an SBI storm starve Gy.

### The value this ADR refuses to invent

The semantically correct answer to transient overload is RFC 6733's **DIAMETER_TOO_BUSY**, a 3xxx
transient-failure code. Its numeric value is **not verifiable from material in this repository**:

- `libs/diameter-core/include/diameter_core/header.hpp:17` already records that RFC 6733's own text
  is not in hand;
- the dictionary's Result-Code values are each cited to a specific `libfdproto.h` line, and the
  freeDiameter header installed on this machine contains **no TOO_BUSY definition at all**.

So the shed answer carries `kDiameterUnableToComply` (5012), which **is** verified in this
project's own dictionary.

That is honestly the wrong class of code: 5012 is a permanent failure where a transient one
belongs, and a peer may stop retrying instead of backing off. Two consequences follow, and both are
deliberate:

1. **The Diameter ceiling is OFF unless explicitly configured** (`diameter_max_tps`). No behaviour
   changes until an operator opts in knowing this.
2. **This is an open question for the user, not a closed decision.** Confirming
   DIAMETER_TOO_BUSY's value (or supplying RFC 6733) makes it a one-line change. Guessing 3004 from
   memory would have been indistinguishable from getting it right, which is exactly why this
   project's rules forbid it.

### Still not covered

SS7/M3UA has no ceiling. P15 says "per-protocol" and two of three protocols now have one.

---

