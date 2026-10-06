## ADR-0377: MDF2 -- the `li-mdf` Mediation and Delivery Function for IRI, the first LI process

**Date:** 2026-09-18. **Status:** Closed (work pushed; last citing commit 055a53a on origin/main, 2026-10-06). accepted. The close of ADR-0364's increment 3: ADR-0373/0374
built the PS-PDU envelope and mediation, ADR-0375 the LI_X2/LI_X3 receiving server, ADR-0376 the
LI_HI2 Delivery Function. This is the process those three `li_core` libraries were built for -- the
Mediation and Delivery Function for IRI of **TS 33.127 clause 5.3.4**.

**Decision: `nfs/li-mdf`, a standalone process that terminates three LI interfaces and owns no
others.**

- **LI_X1** (ETSI TS 103 221-1, XML over HTTP/2 + mTLS): the ADMF provisions warrants here.
  `li_core::x1::handle_request` already parsed the requests; the NF wires
  Activate/Modify/Deactivate(All)Task and Create/Remove(All)Destination to a `TaskStore`, and runs
  the clause-6.6.2 keep-alive timers on their own thread.
- **LI_X2** (ETSI TS 103 221-2 PDUs over mTLS, TS 33.128 clause 5.3): IRI-POIs stream xIRIs in.
  Each PDU carries the XID the POI was provisioned with (clause 5.2.7); that XID is how a received
  record finds its warrant. An X3 PDU is refused, not silently dropped -- content of communication
  is the MDF3's job, a later increment.
- **LI_HI2** (ETSI TS 102 232-1 PS-PDUs over TLS, TS 33.128 clause 5.5): mediated IRI records go
  out to the LEMF through one ADR-0376 Delivery Function per provisioned Destination, created on
  first use and kept for the life of the process (the clause-6.3 DF owns a persistent connection
  and a cyclic buffer; one per record would defeat both).

**Deliberately not an SBI NF.** It does not register with the NRF and exposes no `N`-service. TS
33.127 keeps the LI architecture off the service-based plane; the NRF's LI role is the SIRF (clause
5.3.6), which tells the LIPF what exists -- it does not make an MDF discoverable. So the CLAUDE.md
"every NF registers with the NRF" Definition-of-Done step is **not applicable** here, and that is a
spec fact, not a skipped step.

**State lives in Valkey, never in the process (ADR-0359).** `TaskStore` (`task_store.hpp`) keeps
five kinds of key: `limdf:task:<xid>` (the acted-on subset of a TaskDetails), `limdf:tasks` (live
XIDs, so DeactivateAllTasks and a restart can enumerate), `limdf:dest:<did>`, `limdf:dests`, and
`limdf:seq:<liid>` -- an `INCR` counter behind `PSHeader.sequenceNumber`, kept **per LIID** rather
than per connection because TS 102 232-1 clause 5.2.5 numbers the records of one interception, not
the socket that happens to carry them. A second `li-mdf` replica sees the same warrants.

**Three `li_core` changes this NF forced, each spec-driven:**

1. **The LIID reaches the MDF through `listOfMediationDetails`, not `productID`.** TS 103 221-1
   Annex C.2.2 MediationDetails is the part of a TaskDetails meaningful only to an MDF, and clause
   5.1.2 is explicit: "the ADMF shall provide the XID to LIID(s) mapping to the MDF", one XID to
   one or several LIIDs, each delivered separately. `productID` is **not** the LIID -- the schema
   types it as a UUIDv4 and marks it optional. `li_core::x1` grew a `MediationDetails` struct and
   `read_task_details` now parses `listOfMediationDetails`; the NF turns each into a
   `MediationRoute` (LIID + its HI2/HI3 choice + its own DID list when it deviates from the task's).
2. **TS 103 280 IPAddressPort element names, matched on local name.** The destination-address
   parse used `iPv4Address`/`iPv6Address` and read `port` as a bare number. The TS 103 280 schema
   spells them `IPv4Address`/`IPv6Address` (the "IP" capitalised) and wraps the port in a `TCPPort`
   or `UDPPort`; it is `elementFormDefault="qualified"`, so `child()` matching on local name is
   what makes the parse work whatever namespace prefix the ADMF chose. Fixed in `x1.cpp`.
3. **First-connection delivery is not a resynchronisation.** `Hi2Client` reported
   `Resynchronised` whenever the buffer held PDUs at connect time -- but on the *first* connection
   the buffer legitimately holds PDUs the caller queued before the link came up, and reporting that
   as resynchronisation would tell an operator a link had dropped when it never had. It now reports
   `Resynchronised` only on a genuine re-connection (`reconnect_count > 1`).

**Mediation path (X2 -> HI2).** For each `MediationRoute` of the matched task (skipping HI3-only
routes), the NF builds a `hi2::MediationContext` -- the route's LIID, the operator/NE identifiers
and country codes from config, a per-LIID sequence number, and the target identifiers marked
`lEAProvided` (TS 33.128 clause 5.5.5) -- and calls `hi2::mediate_x2_pdu`, which also adds the
`matchedOn` identifiers from the PDU's own Matched Target Identifier attribute. The record then
goes to each of the route's Destinations (skipping X3-only ones).

**Proven end to end.** `tests/integration/test_li_mdf.cpp` spawns a real `li-mdf`, provisions a
task over real X1 (with a `listOfMediationDetails` LIID), sends a real xIRI over X2 from an
`X2X3Client`, and asserts the LEMF receives a mediated LI_HI2 record whose header carries the
**provisioned LIID** (not the XID, not an invented value), the mapped NF/interception-point
fields, and identifiers with both provenances an MDF2 can source today -- `lEAProvided(1)` from the
X1 task and `matchedOn(3)` from the X2 attribute. It then deactivates the task and confirms the
same xIRI is dropped.

**Disclosed.**
- **Every record is `iRI-Report(4)`.** IRI-Type is per-event (TS 33.128 table 5.5.2-1). Until the
  POI increments carry the event-to-IRI-Type mapping, the MDF2 has no basis to emit
  begin/continue/end and stamps the type for "an event that is not a session boundary". Placeholder,
  not a claim of conformance.
- **`ReportNEIssue` is logged, not sent.** The clause-6.6.2 keep-alive monitor detects a TIME_P2
  silence and a TIME_P3 expiry (at which it does deactivate all tasks), but reporting the fault
  *back* to the ADMF needs an X1 **client**, which lands with the ADMF increment. Until then the
  fault is logged at error, never silently dropped.
- **HI2 delivery inherits every ADR-0376 limitation** (clause 6.4.3 option-3 "sent" semantics,
  no option negotiation or PDU acknowledgement, single-`SSL_read` keep-alive responses, nothing
  delivered after `stop()`).
- **MDF3 (LI_X3 content of communication) is out of scope here.** An X3 PDU is refused with a log
  line; CC mediation and HI3 are a later increment.

**Rejected.** *Registering with the NRF* -- wrong per TS 33.127; the SIRF, not NF discovery, is how
LI is found. *In-process task/destination state* -- breaks the ADR-0359 replica rule and loses
warrants on restart. *Treating `productID` as the LIID* -- the schema says UUIDv4; the LIID is in
`listOfMediationDetails`, and conflating them would stamp the wrong identifier into intercepted
material.

