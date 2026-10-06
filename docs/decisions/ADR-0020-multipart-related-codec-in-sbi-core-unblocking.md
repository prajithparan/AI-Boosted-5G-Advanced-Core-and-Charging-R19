## ADR-0020: multipart/related codec in sbi-core, unblocking CreateUEContext and SMF's CreateSMContext

**Date:** 2026-08-05
**Status:** Accepted

**Context:** Starting SMF's turn, `TS29502_Nsmf_PDUSession.yaml`'s `PostSmContexts` (CreateSMContext
-- the actual AMF-triggered PDU Session Establishment trigger, TS 23.502 clause 4.3.2.2.1, and
CLAUDE.md's stated Phase 2 end-state goal) turned out to be `multipart/related`-ONLY, same as
AMF's already-deferred `CreateUEContext`/`RelocateUEContext`/`CancelRelocateUEContext`
(ADR-0019). Unlike AMF's case, SMF's other `/sm-contexts` operations
(`UpdateSmContext`/`ReleaseSmContext`/`RetrieveSmContext`) all depend on an SmContext that only
`CreateSMContext` can create -- deferring it a second time would leave SMF's entire `/sm-contexts`
surface untestable beyond 404s, not just three peripheral operations. Given a second NF in a row
hitting the same wall on its most central operation, the user chose to build multipart/related
support now rather than keep accumulating deferred/unreachable code across NFs.

**Decision:** `sbi_core::multipart` (`libs/sbi-core/include/sbi_core/multipart.hpp` +
`src/multipart.cpp`): `parse(content_type_header, body) -> tl::expected<vector<Part>, string>` and
`encode(parts) -> Encoded{content_type_header, body}`. This is RFC 2046 ("multipart") + RFC 2387
("related") -- standard IETF MIME framing 3GPP SBI reuses verbatim, NOT a 3GPP-specific wire
format; the only 3GPP-specific knowledge (which named parts a given operation expects) stays in
each NF's own handler code, never in this codec. No changes to `http2::Server`/`Client` were
needed -- multipart bodies are just opaque bytes as far as HTTP/2 framing is concerned; this is
purely a body-content codec NF handler code opts into after checking `Content-Type`.

**One assumption disclosed as unverified, not asserted as fact:** whether 3GPP peers send/expect
`Content-Id` values wrapped in RFC 2045 msg-id angle brackets (`<foo>`) or as a bare token matching
`RefToBinaryData.contentId` verbatim. The OpenAPI YAML only declares
`Content-Id: {schema: {type: string}}`, which doesn't settle this, and there is no real external
SBI peer in this lab to interop-test against (`simulators/ransim` speaks NGAP/NAS to a gNB, not
Namf_Communication/Nsmf_PDUSession multipart bodies). The codec parses leniently (accepts either
form) and encodes without brackets (the bare-token convention this project recalls from other
open-source 5GC interop reports, not verified firsthand). Flagged for revisit the first time this
needs to interop with a real external SBI peer.

**Verification:** 8 unit tests (`tests/conformance/test_multipart.cpp`) -- encode-then-parse
round-trips (single part, multi-part with genuinely opaque binary bytes including embedded null
bytes), a hand-crafted body shaped exactly like `CreateUEContext`'s real wire format (proving the
codec works against literal bytes, not just its own `encode()` output), and 4 malformed-input
rejection cases (wrong content-type, missing boundary, no delimiter, unterminated body) -- all
wrapped in try/catch so malformed network input becomes a returned error, never an uncaught
exception. `sbi_core` builds warning-clean under the project's strict flags (`-Wall -Wextra
-Wpedantic -Wshadow -Wconversion -Wsign-conversion`, `5gc_project_options`).

**Applied immediately to retroactively unblock AMF:** `nfs/amf/src/main.cpp`'s `CreateUEContext`
(`PUT /ue-contexts/{ueContextId}`), `RelocateUEContext`, `CancelRelocateUEContext` are now
implemented for real (previously deferred per ADR-0019) via a shared `parse_multipart_json_body<T>`
helper mirroring the existing `parse_json_body<T>` pattern. Their N2/NGAP binary-content fields
(`targetToSourceData.ngapData.contentId`, etc.) are stub placeholders, disclosed in
`nfs/amf/src/main.cpp`'s file header -- this lab has one AMF and no NGAP stack, so there is no real
inter-AMF handover state to populate them with; the point of this pass was proving the multipart
plumbing end-to-end (encode -> real HTTP/2 wire bytes -> amf's parser -> store -> downstream
operations), not modeling real handover semantics.

**Proof, not assertion:** two new real subprocess-to-subprocess integration tests
(`tests/integration/test_amf_namf_communication.cpp`) construct an actual multipart/related
`CreateUEContext` request via `sbi_core::multipart::encode` (the same codec `amf` uses to parse),
send it over real TLS 1.3 + mTLS HTTP/2, and confirm: the response deserializes as real
`UeContextCreatedData`; `EBIAssignment`/`ReleaseUEContext`'s previously-unreachable "found"
branches now work end-to-end and correctly 404 on a second `ReleaseUEContext`;
`RelocateUEContext`/`CancelRelocateUEContext` work over real multipart bodies and correctly 404 for
a nonexistent context; a non-multipart body on a multipart-only operation correctly gets 400, not
silently accepted. All 19 project tests pass, stable across repeated runs.

**Rejected alternative:** keep deferring `CreateSMContext` a third time (this NF's turn) and build
SMF's `/pdu-sessions` collection instead (the I-SMF/inter-SMF scenario, which does have a JSON-only
alternative). Rejected per the user's explicit choice -- `/pdu-sessions` is the less common
inter-SMF/roaming scenario, not the standard AMF-triggered PDU session establishment CLAUDE.md
names as the Phase 2 end-state goal, and deferring a third time would mean SMF's entire
`/sm-contexts` surface (the actually-important one) stays untestable indefinitely.

**Consequence:** SMF's `CreateSMContext` (this NF's next turn) can now be built for real using this
same codec, rather than deferred a third time.

---

