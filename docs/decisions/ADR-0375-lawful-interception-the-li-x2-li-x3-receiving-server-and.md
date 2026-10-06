## ADR-0375: Lawful Interception -- the LI_X2/LI_X3 receiving server, and the first end-to-end interception path

**Date:** 2026-09-18. **Status:** accepted. Continues ADR-0364's increment 3 alongside ADR-0374
(the HI2 mediation): an MDF2 cannot mediate what it cannot receive, and until now `li_core` had
only the *sending* half of X2/X3 (ADR-0372's `X2X3Client`).

**Decision.** `libs/li-core/src/x2x3_server.cpp` -- `li_core::X2X3Server`, the mirror of the
client: OpenSSL directly (not `sbi_core`; X2/X3 is a length-framed TLS byte stream, and `li_core`
must stay independent of the SBI stack), blocking sockets, one thread per connection, TLS 1.3
minimum. It binds, accepts, reframes the stream by the header's own Header/Payload Length
(TS 103 221-2 clause 5.2.3), answers a Keepalive with a Keepalive Acknowledgement carrying the
same Sequence Number (clause 6.2.4) without passing it to the application, `validate()`s every
X2/X3 PDU against table 5.4.1-1 before delivering it, and hands the rest to a handler.

**Security decisions on the interface through which intercepted material enters the MDF.**
`SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT`: a POI *must* present a certificate chaining
to the MDF's CA, because an unauthenticated peer that could open this socket could inject IRI
records into a warrant's delivery. Payload Length is a 32-bit field from that peer, so it is never
used as an allocation size: a frame beyond `max_pdu_bytes` (8 MiB default) or below the
clause-5.2.3 40-octet minimum drops the connection rather than trying to resynchronise a stream
whose framing is already untrustworthy. SIGPIPE is ignored once, process-wide, for the same reason
the client does it (ADR-0372).

**Lifecycle, because a long-running MDF is not a test fixture.** Each connection thread flips a
flag on the way out and the accept loop joins the finished ones, so a POI that reconnects in a
loop does not accumulate thread objects for the life of the process. `stop()` shuts down every
live connection socket *before* joining: a thread blocked in `SSL_read` would otherwise keep the
MDF alive until the POI chose to close.

**The first end-to-end interception path in this repository.**
`tests/integration/test_li_x2x3_server.cpp` drives the real `X2X3Client` into the real
`X2X3Server` over real mTLS on an ephemeral loopback port with the lab PKI: two PDUs written back
to back arrive as two decoded PDUs equal to what was sent (the reframing), a keepalive is
acknowledged and not delivered, a peer with no client certificate is refused before it can send
anything, and -- the one that matters -- a received xIRI is mediated through ADR-0374 into an
LI_HI2 PS-PDU whose LIID, networkFunctionIdentifier, extendedInterceptionPointID, timestamp
qualifier and IMSI are all asserted. An xIRI now leaves a sender, crosses a TLS socket, and
becomes the record a LEMF would receive. A fifth test covers the one path the client cannot
exercise -- `X2X3Client` writes one whole PDU per `SSL_write`, so on loopback a frame never
arrives split: a raw TLS client writes the first 20 octets, the server is asserted to deliver
nothing, and the remainder completes the frame into exactly one delivered PDU.

The drain loop advances by what `decode()` reports it consumed, not by the frame length
`frame_length()` computed, and treats a disagreement as a framing error. Today they are always
equal (both are Header Length + Payload Length from the same two header fields); the point is that
if that ever stops being true the connection drops instead of silently desynchronising, which
would turn every later PDU on that link into garbage.

**Disclosed.** This is a library, not the MDF2 process: there is still no `nfs/li-mdf` with an
X1-provisioned task store deciding *which* warrant a received PDU belongs to, no Valkey state, no
HI2 delivery transport to an actual LEMF (TS 102 232-1 clause 6.4 profiles it over TCP), and no
Docker/Compose/Helm entry. The handler is called on the connection's own thread, so an MDF2 that
does slow work per PDU will need a queue between the two -- not this increment's problem, but the
next one's. A non-conformant PDU is counted only by its absence from the handler; there is no
fault counter or `ReportTaskIssue` back to the ADMF yet.

**Rejected.** *An event loop (Boost.Asio, as the SBI stack uses)* -- an MDF has a handful of
persistent POI connections, not a C10K fan-in, and thread-per-connection keeps this file readable
against the clause it implements. *Reusing the NF HTTP/2 server* -- X2/X3 is not HTTP. *Detaching
connection threads* -- `stop()` could then return while a handler was still running, which is the
kind of shutdown race that shows up once, in production, in a lawful-intercept path.

