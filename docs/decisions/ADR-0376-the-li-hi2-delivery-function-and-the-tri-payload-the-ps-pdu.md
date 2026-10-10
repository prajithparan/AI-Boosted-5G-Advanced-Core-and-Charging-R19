## ADR-0376: The LI_HI2 Delivery Function, and the TRI payload the PS-PDU subset had to get back

**Date:** 2026-09-18. **Status:** accepted. Third part of ADR-0364's increment 3, after ADR-0374
(the mediation) and ADR-0375 (the receiving side). This is the link out of the MDF to the LEMF.

**The specification was fetched, not guessed.** ADR-0373 derived the PS-PDU subset from the ETSI
schema repository's `LI-PS-PDU.asn` without the deliverable that defines how those PDUs travel;
TS 33.128 clause 5.5.3's NOTE only says "ETSI TS 102 232-1 specifies in clause 6.4 a transport
layer based on TCP". Before writing a delivery function, **ETSI TS 102 232-1 V3.38.1 (2026-08)**
was fetched (`tools/specs/fetch_etsi_specs.py`, which now carries it alongside the two TS 103 221
parts; the PDF stays gitignored under ETSI's reproduction notice). Two things came out of reading
it that could not have been inferred:

1. **The vendored ASN.1 is confirmed against the deliverable itself.** The module table in
   V3.38.1 lists `genHeader(1) version43(43)` -- the exact module OID
   `specs/etsi/102232-1/LI-PS-PDU.asn` carries. ADR-0373's provenance claim now rests on the
   specification, not only on the schema repository.
2. **The subset was wrong, and in a way only the transport clause reveals.** ADR-0373's edit 2
   dropped `Payload.tRIPayload [2]` as "not a 5G IRI/CC delivery". But clause 6.3.4 is explicit:
   *"The 'keep-alive' message is sent as TRI of type keep-alive"* -- and 6.3.6's PDU
   acknowledgement is a TRI too. A Delivery Function that cannot encode a TRI cannot implement the
   session layer at all. `tRIPayload [2]` is restored, with `TRIPayload` trimmed to its
   self-contained alternatives (`testPDU`, `paddingPDU`, `keep-alive`, `keep-aliveResponse`,
   `firstSegmentFlag`, `lastSegmentFlag`, `cINReset`, the two `pDUAcknowledgement` NULLs);
   `integrityCheck [0]`, `operatorLeaMessage [8]`, `optionRequest [9]`, `optionResponse [10]` and
   `optionComplete [11]` are dropped, each pulling in a closure of its own. This is the value of
   reading the transport clause before implementing the transport.

**Decision: `li_core::Hi2Client`, a clause-6.3 Delivery Function.** One worker thread; `send()`
only enqueues, so an MDF2's X2 receive thread is never blocked by a slow or dead LEMF. What each
sub-clause demanded and what the class does:

- **6.3.2 (connections):** opens on `start()`, reopens immediately on any termination, retries a
  failed open at a configurable interval (`reconnect_interval`, the clause's "e.g. 30 s"), and
  reports every failure -- the clause says "to the Handover Manager", a component this project
  does not have, so failures surface through an `Hi2EventHandler` the MDF2 wires to its logs and
  counters rather than being swallowed.
- **6.3.3 (buffering):** a cyclic buffer bounded in octets. A sent PDU *stays* in it and is only
  evicted when the buffer needs the room, so a reconnect re-sends everything still held, which is
  what makes the clause's resynchronisation real rather than nominal (NOTE 2 of that clause is
  explicit that this can duplicate delivery and that the LEMF must cope). A full buffer is not an
  error: the oldest data is overwritten and a `BufferFull` event is reported, exactly as written.
- **6.3.4 (keep-alives):** a timer reset by every send; at TIME1 a keep-alive TRI with a fresh
  timestamp and a sequence number incrementing per keep-alive within this instance; if no
  `keep-aliveResponse` arrives within TIME3, the connection is torn down and reopened.
- **6.3.1 / 6.4:** TCP inside TLS. The clause requires at least TLS 1.2 and says new
  implementations should support 1.3; this one *requires* 1.3 with a client certificate, as every
  other link in this project does.

`hi2.hpp` grew the envelope side to match: `PsHeader` is now a type of its own (an IRI message and
a TRI message carry the same clause-5.2 header), `encode_tri_message` / `decode_tri_message` build
and read the session-layer PDUs, and `payload_kind()` lets a reader route a received PS-PDU before
decoding it. `IriMessage`'s flat header fields moved into `IriMessage::header`.

**Disclosed.**
- **Clause 6.4.3 option 3.** The clause offers three definitions of "successfully sent"; this DF
  counts a PDU sent once it is passed to an open socket. The clause says option 3 "is only
  acceptable subject to the agreement of the CSP and LEA" and that some data may be lost during
  network outages. The retained cyclic buffer narrows that window (a dropped connection re-sends
  what is still held) but does not close it. Options 1 and 2 need TCP-level send accounting that
  is not wired.
- **Option negotiation (6.3.5) and PDU acknowledgement (6.3.6) are not implemented.** The TRI
  alternatives for the acknowledgement pair are in the subset and encode; nothing drives them, and
  `optionRequest`/`optionResponse` are not in the subset at all. Both are "if required" in the
  clause.
- **The keep-alive response is read from a single `SSL_read`.** A response split across TCP reads
  would not be recognised and the connection would be torn down at TIME3 and reopened -- correct
  behaviour, but noisier than necessary. A PS-PDU reassembler on the receive path is the fix when
  a real LEMF needs it.
- **Nothing is delivered after `stop()`.** The clause's buffer covers short outages, not process
  exit; a restart loses whatever was still buffered. Persisting it is MDF2 state work.
- **`libli_core.so` now exports 98 symbols, 10 of which are weak libstdc++ `std::deque`
  instantiations** from the cyclic buffer. ADR-0364's invariant is about the asn1c, OpenSSL and
  libxml2 symbol sets, and that count is still exactly zero (`nm -D | grep -cE
  'SSL_|EVP_|xmlSchema|asn_DEF_|ber_decode'`). COMDAT template symbols are not a leak.

**Rejected.** *Delivering from the caller's thread* -- an MDF2 would then stall its X2 intake
whenever the LEMF was slow, which is how intercepted material gets dropped. *Dropping a PDU from
the buffer the moment it is written* -- makes clause 6.3.3's resynchronisation a no-op. *Waiting
for a real Handover Manager before reporting failures* -- the clause requires the reports; a
callback the MDF2 owns is the honest stand-in.

