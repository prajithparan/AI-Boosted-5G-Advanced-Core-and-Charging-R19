## ADR-0291: the Diameter overload answer is DIAMETER_TOO_BUSY -- and a search I gave up on too early

**Date:** 2026-09-05
**Status:** Accepted (user-directed)

ADR-0285 shed Diameter requests with `DIAMETER_UNABLE_TO_COMPLY` (5012) and disclosed that the
semantically correct `DIAMETER_TOO_BUSY` could not be used because its value was not verifiable
from material in this repository. **That disclosure was wrong**, and the error was mine.

### What I actually did, and what I should have done

I checked `/usr/include/freeDiameter/libfdproto.h`, found no `TOO_BUSY`, noted that
`header.hpp:17` records RFC 6733's text as not in hand, and stopped -- concluding the value was
unobtainable and writing that into an ADR.

The value is at **`simulators/reference/freeDiameter/include/libfdproto.h:1860`**
(`#define ER_DIAMETER_TOO_BUSY 3004`), registered at
`simulators/reference/freeDiameter/libfdcore/dict_base_proto.c:1201`. That vendored reference tree
is **the same source every other Result-Code in `libs/diameter-core`'s dictionary already cites**.
The citations in the file I was editing pointed straight at it.

Refusing to invent the value was right. Concluding it was unavailable after searching one system
include path was not -- the project's own citation convention was the map, and I did not follow it.

### The change

`ResultCode::kDiameterTooBusy = 3004` (cited like its neighbours), and the Diameter shed answer now
uses it. The difference is real, not cosmetic: 3xxx is a transient protocol error a peer backs off
and may fail over on, where 5xxx says the request will never succeed. Under overload a peer that
stops retrying is the opposite of what shedding is for.

### What this does not change

The ceiling stays **off unless configured** -- that is an opt-in policy choice for every one of the
three protocols, not a hedge about correctness. And ADR-0290's gap stands: neither the Diameter nor
the SS7 ceiling has an end-to-end test.

---

