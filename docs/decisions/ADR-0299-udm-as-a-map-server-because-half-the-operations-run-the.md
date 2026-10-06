## ADR-0299: UDM as a MAP server -- because half the operations run the other way

**Date:** 2026-09-05. **Status:** accepted. **Follows:** ADR-0293, ADR-0296.

The task was "the remaining MAP operations". Working out what that meant turned out to be most of
the work, because MAP operations have a **direction**, and the direction decides which side of this
project has to change:

| Operation | Direction | UDM's role |
|---|---|---|
| `insertSubscriberData`, `cancelLocation`, `deleteSubscriberData` | HLR -> VLR | **sends** |
| `updateLocation`, `sendAuthenticationInfo`, `purgeMS` | VLR -> HLR | **receives** |
| `checkIMEI`, `sendIdentification` | VLR->EIR, VLR->VLR | not the HLR's operations at all |

ADR-0293 and ADR-0296 built the sending side. Three of the five "remaining" operations run the
other way and **cannot be reached by a client at all** -- adding their argument encoders would have
produced exactly the unreachable code ADR-0293 was written about. They need a server, and this ADR
is that server.

That reframing is the finding. "Add the remaining codecs" would have looked like progress, compiled
cleanly, been tested at the codec level, and delivered nothing reachable.

### What was built

`nfs/udm/src/map_server.{hpp,cpp}` -- a real SCTP listener, dedicated accept thread, one thread per
association, real M3UA ASPSM/ASPTM handshake in the responder role. The shape follows
`nfs/chf/src/cap_server.cpp` deliberately rather than inventing a second arrangement, since that
server is this project's working precedent for a TCAP dialogue over this stack.

- **`updateLocation`** -- answered with this UDM's own configured `hlr-Number`, and the VLR recorded
  against the subscriber in **the same `AmfRegistrationStore` the `Nudm_UECM` HTTP handlers use**. A
  VLR registering a subscriber over MAP and an AMF registering one over SBI are the same fact about
  the same subscriber; two stores would let them disagree. It is also what gives a later
  `cancelLocation` (ADR-0296) something real to cancel.
- **`purgeMS`** -- the serving-node record is dropped. `freezeTMSI`/`freezeP-TMSI` are deliberately
  NOT set: they instruct a VLR to quarantine a TMSI, and nothing in this project allocates or tracks
  TMSIs, so setting them would assert state that does not exist.
- **`sendAuthenticationInfo`** -- NOT implemented, and answered with a real `ReturnError` rather
  than silence. A VLR that gets no answer waits out its invoke timer and cannot tell "unsupported"
  from "dead HLR". The gap is genuine and narrow: the response carries an `AuthenticationSetList`
  of quintuplets, an ASN.1 structure this codec does not model. UDM already has the real vectors
  (`Nudm_UEAU`/milenage) -- only the MAP encoding is missing.

Off unless `map_server_port` is configured; `hlr_number` comes from config, never a literal
(task #109).

### The codec hazard worth naming

`UpdateLocationArg`'s `imsi` and `vlr-Number` are **both untagged UNIVERSAL OCTET STRINGs**, sitting
either side of a tagged `msc-Number [1]`. Nothing but **position** tells them apart, so a decoder
that searched by tag would silently swap a subscriber identity with a network address. Decoded
positionally, with a test that would catch the swap. `PurgeMS-Arg` carries the same CONTEXT [3]
CONSTRUCTED wrapper `cancelLocation` has; `UpdateLocationRes`'s `hlr-Number` is **mandatory**,
unlike the empty-SEQUENCE results the other operations have, and a test rejects an empty one.

### A narrowing that is honestly wrong rather than invented

The `ReturnError` for an unimplemented operation carries **the opcode as its error code**, not a
real MAP error value. TS 29.002's numeric MAP-Errors codes were not located in this project's spec
material -- `map_dictionary.hpp` already records the same gap for `insertSubscriberData`'s errors.
The choice was between a structurally-real ReturnError with an honestly-wrong code and a
plausible-looking invented error number; the first is auditable and the second is not. If a real
peer's behaviour ever depends on the value, this needs the real codes rather than a better guess.

### Evidence

`tests/integration/test_udm_map_server.cpp` **is** a VLR: real association, real M3UA activation,
real TC-Begin/Invoke, and it checks the stored record over **SBI** rather than trusting the MAP
answer -- a server that replies correctly and stores nothing would pass a MAP-only assertion. It
also asserts the negative case, that an unimplemented opcode comes back as a ReturnError rather than
silence. Six codec tests cover the positional hazard, the `[3]` wrapper, and the mandatory
`hlr-Number`.

### Still not done

`deleteSubscriberData` (HLR -> VLR, sender side) has an opcode and no codec.
`sendAuthenticationInfo` needs `AuthenticationSetList`. `checkIMEI` belongs to 5G-EIR's analogue and
`sendIdentification` to a VLR, so neither is UDM's to implement.

