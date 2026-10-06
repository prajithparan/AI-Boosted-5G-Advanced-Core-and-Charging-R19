## ADR-0364: Lawful Interception -- the architecture mapping, and increment 1 (X2/X3 PDU codec + TS 33.128 payload codec)

**Date:** 2026-09-14. **Status:** accepted. User-directed ("LI complete first before NWDAF",
"start LI"); resolves production blocker #0 of the 33-series review (ADR-0349) into a build
plan, and delivers its first increment.

**Sources in hand, versions pinned.** TS 33.127 V19.7.0 (`specs/3gpp/TS_33.127_j70.txt`,
architecture), TS 33.128 V19.7.0 (`TS_33.128_j70.txt`, protocols and payloads) with its Annex A
attachments (`specs/3gpp/33128-attachments/`: `TS33128Payloads.asn` module OID `... ts33128(19)
r19(19) version7(7)`, the X1/XLA/state-transfer XSDs, the dictionaries), ETSI TS 103 221-1
V1.23.1 (X1) and ETSI TS 103 221-2 V1.10.1 (2026-03) (X2/X3), and ETSI's BSD-3-Clause schema
repository (`specs/etsi/SOURCES.md` records every file and commit). The ETSI deliverables
themselves are **not committed**: their notice reads "No part may be reproduced or utilized in
any form or by any means ... except as authorized by written permission of ETSI", which the
3GPP material this repository does commit does not say. `specs/etsi/*.pdf` and `*.txt` are
gitignored; `tools/specs/fetch_etsi_specs.py` re-fetches the exact versions from etsi.org.

**The architecture, mapped onto this project (TS 33.127 clause 5).**

| TS 33.127 entity | Where it lives here | Interface | Realised by |
|---|---|---|---|
| ADMF = LICF + LIPF (5.3.5) | new `nfs/li-admf` -- no 3GPP stage-3 defines its internals; it is the X1 *client* and the HI1 endpoint | LI_X1 to every POI/TF/MDF (5.4.4) | ETSI TS 103 221-1 XML over HTTPS (TS 33.128 5.2) |
| MDF2 / MDF3 (5.3.4) | new `nfs/li-mdf` | LI_X2 / LI_X3 in (5.4.5/5.4.6), LI_HI2 / LI_HI3 out (5.4.8/5.4.9) | X2/X3: TS 103 221-2 PDUs over TLS (TS 33.128 5.3); HI2/HI3: TS 33.128 clause 5.5-5.6 (ETSI TS 102 232-1 PS-PDU), later |
| SIRF (5.3.6) | inside the NRF (TS 33.127 6.2.6, "LI support at NRF") | LI_SI (5.4.2) | the NRF's own registry, filtered for the LIPF |
| IRI-POI in AMF (6.2.2) | `nfs/amf` | LI_X2 | xIRI events of 6.2.2.4, payloads TS 33.128 6.2.2 |
| IRI-POI + CC-TF in SMF, CC-POI in UPF (6.2.3) | `nfs/smf`, `nfs/upf` | LI_X2, LI_T3, LI_X3 | 6.2.3.3 events; xCC per TS 33.128 6.2.3.3 |
| IRI-POI in UDM (6.2.4, 7.2.2), SMSF (6.2.5), NEF (7.9), NWDAF (7.18), CHF (7.22) | the respective NF | LI_X2 | per-clause events |
| LEA / LEMF (5.3.1, 5.3.7) | out of scope -- the peer, not the product | LI_HI1/2/3 | -- |

Each POI is embedded in its NF (TS 33.127 5.3.2) and *shall* be provisioned over X1 from the
LIPF; it never learns of a warrant any other way. The one implementation decision the spec leaves
open and this ADR takes: the POI is a library the NF links (`li_core`, below) plus a per-NF event
hook, not a sidecar process -- a sidecar would need an interface 3GPP does not define between the
NF and its own POI.

**Increment 1, delivered here: the protocol floor every later increment stands on.**

- `libs/li-generated`: `TS33128Payloads.asn` + the `IPIRIPacketReport` closure of ETSI TS 102
  232-3 `IPAccessPDU` (6 types, extracted verbatim by `tools/li-asn1/trim_ipaccess.py` at module
  version18, the version TS33128Payloads imports -- the ETSI repository head is already
  version20, so the file is pinned to ETSI commit `0d26bdf`) compiled by the project's asn1c
  (ADR-0030/0031) for BER. 1 101 C files, generated at configure time like NGAP.
- `libs/li-core` `x2x3_pdu.hpp`: ETSI TS 103 221-2 table 5.1-1 header (Version 6, PDU Type,
  Header/Payload Length, Payload Format, Payload Direction, XID, Correlation ID), table 5.3.1-1
  conditional-attribute TLVs with a builder per clause 5.3.6-5.3.23 and typed readers, the
  keepalive PDUs of clause 5.1/6.2.4, `validate()` for table 5.4.1-1's X2/X3 permission columns,
  and stream framing. Tests assert the byte layout against the tables, not against the encoder.
- `libs/li-core` `xiri.hpp`: `encode_xiri_payload()`/`decode_xiri_payload()` -- BER
  `XIRIPayload { xIRIPayloadOID {4 19 19 7 1}, event }` (TS 33.128 table 5.3.2-3), Payload
  Format 2 (table 5.3.2-1). C++ event structs in place of the generated C types; the first is
  `AmfRegistration` with the members its ASN.1 SEQUENCE marks mandatory. TS 33.128's M/C/O
  table for the event (6.2.2.2) is applied in the AMF POI increment, which is where the values
  come from.
- `tests/conformance/test_li_core.cpp` (10 tests) and `test_li_ngap_coexistence` (below).

**The symbol clash, and why `li_core` is this repository's first shared library.**
Two asn1c outputs in one executable collide twice over. The asn1c runtime skeleton (36 files:
`asn_DEF_INTEGER`, `ber_decode`, `OCTET_STRING_fromBuf`, ...) is copied into both generated
trees, so it exists twice -- identical source today, but two definitions of every runtime
symbol. And `TS33128Payloads` and TS 38.413 NGAP define 47 identically named spec types
(`AllowedNSSAI`, `AMFPointer`, `GUAMI`, `TAI`, `PDUSessionID`, ...) with different shapes --
NGAP's `AMFPointer` is a 6-bit BIT STRING, TS 33.128's an INTEGER (0..63) -- which is the
dangerous half: the wrong `asn_DEF_*` bound to the wrong struct layout. The AMF and SMF link `ngap_generated` flat, and they are exactly the NFs that host an
IRI-POI. Vanilla asn1c 0.9.29 has no `-fprefix`. So `li_generated` is compiled with hidden
visibility and folded into `libli_core.so`, whose dynamic symbol table exports 31 `li_core::`
functions and nothing from asn1c (`nm -D` verified); a consumer never sees a generated header
because the link is PRIVATE. `tests/conformance/test_li_ngap_coexistence` links `li_core` and
`ngap_generated` into one executable, encodes NGAP's `AMFPointer` through NGAP's descriptor and
TS 33.128's through `li_core`, and passes. `tests/fuzz/fuzz_li_x2x3` fuzzes both decoders
(X2/X3 framing, BER XIRIPayload) as the other codec directories do. Rejected: *sed-prefixing 1 101 generated files*
(fragile, unauditable against the spec module); *a separate POI process per NF* (invents an
NF-internal interface); *renaming types in the ASN.1* (modifying a spec module is the one thing
the source-of-truth rule forbids).

**What comes next, one increment per turn, POIs in the order the 33-series review ranked the
risk.** (2) X1 server in `li_core` (TS 103 221-1 XSD-driven: CreateDestination, ActivateTask,
ModifyTask, DeactivateTask, GetTaskDetails, Keepalive, ReportTaskIssue) and the X2/X3 TLS client
with clause 6.2.4 keepalives; (3) `nfs/li-mdf` MDF2 receiving X2 -- HI2 out is a later
increment; (4) the AMF IRI-POI, 6.2.2.4's Registration / Deregistration / Location update /
Start of interception with already registered UE / Unsuccessful procedure (the note in 6.2.2.4
scopes UE-state reporting to these), each with its TS 33.128 6.2.2.2 M/C/O table -- **the
procedure list will be shown for approval before that turn's code, per CLAUDE.md**; (5)
`nfs/li-admf` LIPF as X1 client with HI1 (ETSI TS 103 120) after; (6) SMF/UPF (IRI + CC),
(7) UDM, SMSF, NEF, CHF (7.22), NWDAF (7.18), NRF SIRF. Identity privacy (6.2.2.3, SUCI
de-concealment only at the UDM POI), LI_ST state transfer in SMF sets (6.2.3.8/9), LALS (7.3.3)
and the virtualised-deployment lifecycle procedures (5.6) are recorded as not started.

**Disclosed.** Increment 1 has no transport, no X1, no NF hook: nothing is intercepted yet.
`xiri.hpp` models one of 202 `XIRIEvent` alternatives. The keepalive timers, TLS mutual
authentication (clause 6.2.3, RFC 6125) and reconnect-with-X1-error-report of clause 6.2.4 are
specified in the header comment and not yet implemented. ETSI TS 102 232-1 (HI2/HI3 PS-PDU) has
not been fetched; the HI increments will need it.

**CI note (same day, found by the first self-hosted run).** The runner's fresh vcpkg clone sat
at vcpkg HEAD while `vcpkg.json` pins `builtin-baseline` f1d4bbc; vcpkg's binary-cache key
hashes the checkout's `scripts/` tree and tool version, so every one of the ~170 installed ports missed the
shared cache and rebuilt from source (onnxruntime alone ran 37 minutes before the run was
cancelled). The Bootstrap step now checks the clone out at the baseline commit, which is where
the lab machine's own vcpkg is, so the 470-archive cache hits.

**CI note 2 (2026-09-15, first TSan job on the lab runner).** GCC's libtsan aborts at startup
("FATAL: ThreadSanitizer: unexpected memory mapping") on the runner's Linux 7.0 kernel, whose
ASLR entropy (`vm.mmap_rnd_bits`) exceeds what the runtime's shadow layout assumes; GitHub-hosted
images lower that sysctl for exactly this reason. Verified on the box: a trivial
`-fsanitize=thread` binary segfaults; the same binary under `setarch -R` exits 0; clang-18's
runtime re-execs itself with ASLR off and also passes. The sanitize job's Build (gtest
discovery executes the test binaries) and Test steps now run under `setarch $(uname -m) -R`
when the matrix leg is `tsan`. Rejected: the sysctl (needs root, per machine, invisible in the
repo); switching the leg to clang-18 (a second toolchain's worth of warnings and ccache misses
to chase for a problem that is a process flag).

**CI note 3 (2026-09-15, the TSan leg past startup for the first time).** 641/650 passed; the
nine failures were every test that spawns the CHF with the Kafka bus enabled, plus the
event-bus test itself, which segfaulted 1 s in. Reproduced with a 15-line librdkafka producer
under both GCC 13 and clang-18 ThreadSanitizer: librdkafka creates its broker threads with
glibc's C11 `thrd_create` (`nm librdkafka.a` shows the undefined symbol), which calls
`pthread_create` inside glibc where TSan's interceptor does not see it; the new thread has no
TSan state and dies on its first `realloc` (glibc's `__check_pf` under `getaddrinfo`). Decision:
the TSan leg runs with `CHF_CDR_EVENT_BUS_BROKERS` empty (bus disabled, direct-insert path) and
the event-bus test skips itself when compiled under TSan, with the reason printed. The ASan/UBSan
leg and the plain build keep the full Kafka path. Disclosed: TSan therefore never observes the
CDR-to-Kafka path -- a small loss, since librdkafka itself is uninstrumented and the producer's
only shared state is the `std::unique_ptr` the destructor flushes. Rejected: an overlay port
building librdkafka with `WITH_C11THREADS=OFF` (changes the production library to suit a test
tool); the sysctl/compiler routes are irrelevant here (same crash on both compilers).

