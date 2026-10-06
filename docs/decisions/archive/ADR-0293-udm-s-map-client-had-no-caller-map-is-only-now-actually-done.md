## ADR-0293: UDM's MAP client had no caller -- MAP is only now actually done

**Date:** 2026-09-05. **Status:** Closed (work pushed; last citing commit f171dfb on origin/main, 2026-10-06). accepted.

The user's own item 4 was "complete map/cap is not done". Checking rather than assuming found
something worse than an incomplete feature: `libs/map-core` and `nfs/udm/src/map_client.cpp` were
real, tested code with **no caller anywhere**. `grep -rn map_client nfs/udm/src/main.cpp` returned
nothing. By this project's own standard -- recorded repeatedly since ADR-0184, "a server with no
consumer is not done" -- MAP was not done, and no document said so.

CAP was already wired for real (CHF's `cap_server`, ADR-0061), which is why the pair reads as
"MAP/CAP" but only half of it had a live path.

### What changed

`push_subscriber_data_to_vlr()` in `nfs/udm/src/main.cpp`, called from the real
`Nudm_UECM` AMF-registration handler (`PUT /{ueId}/registrations/amf-3gpp-access`). That is the
correct 3GPP trigger: TS 29.002's `insertSubscriberData` is what an HLR pushes to a VLR when a
subscriber becomes served by it, and the 5G analogue of "the subscriber is now served by this
serving node" is exactly the UECM AMF registration.

Peer address comes from `config/udm.json`'s `vlr_peer_address`/`vlr_peer_port`, overridable by
`UDM_VLR_PEER_ADDRESS`/`UDM_VLR_PEER_PORT` -- the standing no-hardcoded-config rule (task #109).
**Empty is the default and means disabled**, because a 5G-only deployment has no VLR; nothing
changes for anyone who does not opt in.

### Deliberate design points, stated rather than left to be discovered

- **Detached thread.** `send_insert_subscriber_data` opens an SCTP association and blocks on the
  M3UA handshake and the TCAP answer. Doing that inline would make a 5G registration's latency
  depend on a legacy VLR's. This is the same synchronous-client debt ADR-0009 tracks, worked
  around at the call site rather than pretended away.
- **A MAP failure is logged, not propagated.** The UECM registration still returns 201/200. A
  legacy interworking push failing must not fail a 5G registration; the log line says so
  explicitly.
- **Fire-and-forget on the response.** UDM does not retry, and there is no MAP-level state machine
  across registrations. Real HLR/VLR interworking has both. This is a real, disclosed narrowing.

### The test, and why it is shaped the way it is

`tests/integration/test_udm_map_insert_subscriber_data.cpp` starts a stand-in VLR on real SCTP,
answers the real M3UA ASPSM/ASPTM handshake (ASP Up/Ack, ASP Active/Ack), and asserts a real
`insertSubscriberData` Invoke arrives carrying the TBCD-encoded IMSI of the UE that just
registered.

It then **answers the dialogue for real** -- AARE with the same application context the AARQ named,
TC-END carrying a `ReturnResultLast`. That was not the first version: the first version accepted
the Invoke and closed, the test passed, and UDM logged `MAP insertSubscriberData ... failed`. A
green test next to a failing log is the exact shape of a test that proves less than it appears to.
Completing the dialogue makes the client's whole response path -- AARE handling, TC-END decode,
`ReturnResultLast` interpretation -- actually execute, which is where a decode bug would hide. UDM
now logs `accepted by VLR`, four protocol layers deep, in the test's own output.

The VLR uses this project's own `ss7_core`/`tcap_core`/`map_core` rather than a hand-rolled parser,
so a change that broke the encoding breaks the test instead of being mirrored by it.

### What is still not done

`Nudm_UECM` deregistration does not send `cancelLocation`, the natural MAP counterpart. The Gr/Gc
interface's other operations (`updateLocation`, `sendAuthenticationInfo`, `purgeMS`) remain
unimplemented -- `libs/map-core` encodes `insertSubscriberData` only. Named here so "MAP is done"
is not read wider than it is: one real operation now has a real caller and a real end-to-end test.

