## ADR-0372: Lawful Interception increment 2 -- the X1 provisioning interface (libxml2 + runtime XSD), the NE server + keepalive machine, and the X2/X3 delivery client

**Date:** 2026-09-16. **Status:** accepted. User-directed ("Resume LI increment 2 now", chosen
over the NWDAF roaming/VFL backlog which stays in ADR-0359); continues ADR-0364's increment plan
(increment 1 = the X2/X3 PDU + xIRI payload codec). Two sub-decisions were put to the user and
taken here: the X1 XML library is **libxml2** (runtime XSD validation), and the **X2/X3 delivery
client is included now** rather than deferred to the MDF2 increment.

**Delivered here, all in `libs/li-core` (no NF wired yet -- this is the interface floor the MDF2
and the AMF POI stand on).**

- `x1.hpp`/`x1.cpp`: the ETSI TS 103 221-1 V1.23.1 LI_X1 codec. `parse_request()` reads an
  `X1Request` container into typed C++ (a `RequestBody` variant over ActivateTask / ModifyTask /
  DeactivateTask / DeactivateAllTasks / GetTaskDetails / CreateDestination / RemoveDestination /
  RemoveAllDestinations / Ping / Keepalive), discriminating the message by its `xsi:type`;
  `serialise_response()` builds the `X1Response`; `serialise_top_level_error()` builds the
  clause-6.1 TopLevelErrorResponse. Both directions validate against the committed XSD.
- `x1_server.hpp`/`x1_server.cpp`: `handle_request(xml, TaskStoreCallbacks)` -- the NE side of
  X1. The NF supplies `std::function` callbacks (activate / modify / deactivate / deactivate_all
  / create / remove / remove_all, each returning `optional<ErrorCode>`; plus `check_identity`,
  `ne_identifier`, `keepalive_supported`); the server maps their result onto the table 6.7-3
  error codes and an OK/Error `ResponseItem` per request. `KeepaliveMonitor` is the clause 6.6.2
  state machine (timers P2 no-X1 window, P1 wait-for-ack, P3 before deactivate-all; codes 9050
  "Keepalives not received", 10000 "Database cleared"; deactivate-all gated by a config default).
- `x2x3_client.hpp`/`x2x3_client.cpp`: the LI_X2/LI_X3 delivery client -- a blocking mTLS
  connection (OpenSSL directly, not `sbi_core`: X2/X3 is a raw TLS byte stream of TS 103 221-2
  PDUs, not HTTP/2 + JSON) that `validate()`s and `encode()`s a `Pdu` from increment 1's codec,
  sends keepalive PDUs, and reconnects on a broken link.

**Runtime XSD validation, and how the schema set is wired.** The normative `TS_103_221_01.xsd`
imports the TS 103 280 common namespace and the X1 HashedID namespace *without* a
`schemaLocation`, so libxml2's schema loader cannot resolve them alone. `specs/etsi/103280/
TS_103_280.xsd` was fetched from ETSI's BSD-3-Clause repository (same commit `e531df0`,
`specs/etsi/SOURCES.md`), and a **project-owned** wrapper `specs/etsi/103221-1/x1-validation.xsd`
(targetNamespace `urn:5gc-r19:li:x1-validation-wrapper`, recorded as not-ETSI in `SOURCES.md`)
supplies the import locations and pulls in the normative schema unchanged. The schema is parsed
once behind `std::call_once`; its path comes from the `LI_ETSI_SCHEMA_DIR` compile definition.

**Security decisions on an attacker-facing parser.** X1 is parsed from bytes a network peer (the
ADMF) sends, so the parser is hardened: `XML_PARSE_NONET` (no network fetches) and, deliberately,
**not** `XML_PARSE_NOENT` -- with NOENT libxml2 would substitute internal/external entities into
the tree, an XXE / entity-expansion vector (an entity resolving `file:///...` into a target
identifier the POI would store and log). X1 has no legitimate entity use; schema validation
rejects any unexpanded reference. `test_li_x1.cpp` `RejectsExternalEntityXxe` locks this in.
Because the X1 server will run on NF HTTP/2 worker threads (the AMF POI, increment 4),
`xmlInitParser()` is called inside the same `call_once`, and `xmlSchemaValidateDoc` -- which
mutates the shared schema's compiled-type cache -- is serialised by a mutex; the tests are
single-threaded, so TSan would not have caught the race.

**Symbol hygiene preserved.** `li_core` is this repository's one shared library (ADR-0364: it
hides the asn1c runtime and the 47 NGAP-colliding type names). Statically linking OpenSSL and
libxml2 into it leaked 34 `d2i_*`/`xmlSchema*` symbols into `libli_core.so`'s dynamic table, a
clash hazard against the AMF's own OpenSSL; `target_link_options(li_core PRIVATE
LINKER:--exclude-libs,ALL)` (GNU/Clang) contains them -- `nm -D` verified 0 OpenSSL, 0 libxml2,
and only the `li_core::` exports remain. libxml2 was added to `vcpkg.json`.

**Disclosed (stubs, simplifications, non-conformance).**
- **GetTaskDetails is answered with error 1080 (unsupported request), disclosed as conformant.**
  A full GetTaskDetailsResponse carries a rich TaskStatus (provisioningStatus + listOfFaults)
  that no consumer of ours needs yet; building it now would be speculative. The request parses;
  the NE declines it with the table 6.7-3 code for an unsupported request.
- **`LI_ETSI_SCHEMA_DIR` is a build-tree absolute path, and a missing schema fails closed.** If
  the schema is not found at runtime, `schema_valid()` returns false and *every* X1 request
  becomes a TopLevelError. The NF runtime containers copy only the binary today; when the AMF
  POI is containerised (increment 4) its image MUST also carry `libli_core.so` (the repo's only
  shared library) and the `specs/etsi` schema set, or a path override. Tracked for that increment.
- **No NF is wired to any of this yet.** The MDF2 (increment 3) is the first consumer of the
  X2/X3 client and the X1 server; the AMF IRI-POI (increment 4, which requires showing the
  TS 33.127 6.2.2.4 event list + TS 33.128 6.2.2.2 M/C/O table for approval first) is the first
  X1-provisioned POI. The X2/X3 client is exercised end to end by
  `tests/integration/test_li_x2x3_client.cpp`: a loopback TLS 1.3 server (the lab AMF cert,
  `certs/`) accepts the client's connection and the framed PDUs decode back to what was sent.
  That test surfaced a real defect -- OpenSSL's socket write raised SIGPIPE on a dropped link and
  killed the process; the client now ignores SIGPIPE once (process-wide, the correct disposition
  for a network server, and the repo's first raw-socket writer), so a broken link becomes the
  reconnect-on-next-send error it already returned.

**Rejected.** *pugixml* -- fast and header-only, but no XSD validation; X1 conformance to a
published schema is the point, and hand-checking every element against table 6.x is exactly the
fabrication risk libxml2's `xmlSchemaValidateDoc` removes. *Routing X2/X3 through `sbi_core`* --
that stack is HTTP/2 + JSON + OAuth for SBI; X2/X3 is a length-framed TLS byte stream, so it would
have to be bypassed anyway. *A URI under ETSI's namespace for the wrapper's targetNamespace* --
switched to a project URN so no ETSI-namespaced identifier names a non-ETSI file.

