## ADR-0476: Duplicate JSON member names are rejected in the SBI server before any handler (finding F3, TS 33.117 4.3.6.3)

**Date:** 2026-10-09. **Status:** Accepted for the detector (unit-tested); server-level rejection coded, NOT yet exercised over a socket.

**Context.** Finding F3 of `docs/SECURITY_COMPLIANCE.md` (ADR-0354): TS 33.117 4.3.6.3 requires that the same name appearing twice in a JSON structure leads to an error and rejection of the message; the project's parser keeps the last value silently (`{"ratingGroup":1,"ratingGroup":99}` parsed as 99). The finding suggested fixing it in `parse_json_body`; many handlers call `json::parse(req.body)` directly instead (`nfs/nwdaf/src/mtlf.cpp` for one), so a fix there would leave most of the surface open.

**Decision.** The check runs once, in the SBI server, before the matched handler is posted (`libs/sbi-core/src/http2_server.cpp`): for a request with a body whose `content-type` is `application/json...` or contains `+json` (so merge-patch and problem bodies too; multipart is not touched), `sbi_core::http2::json_has_duplicate_keys` (a SAX pass with one name set per open object, `json_body.cpp`) decides, and a repeat answers 400 `application/problem+json` with `cause: INVALID_MSG_FORMAT` (TS 29.500 table 5.2.7.2-1). The same name in different objects, and arrays of objects, are fine; `"a"` and `"\u0061"` count as the same name; malformed JSON returns false and is left to the handler's own error. Every NF on `sbi_core` gets it with no per-NF change.

**Proof / not proof.** 6 unit tests on the detector (`sbi_json_duplicate_keys_tests`, no ports) pass. The server hook is compile-checked only: no test sends a duplicate-key body to a running server yet (it needs a listening port, so it waits for an idle runner). Cost: one extra parse of every JSON body (a SAX pass, no tree built); not measured.

**Not covered.** Duplicate names in a JSON body received by an NF acting as a CLIENT (responses), in `application/json` parts inside multipart bodies, and in files read from config. A request whose content-type is absent or other than JSON but whose body is JSON is not checked.

**Rejected alternatives.** Hooking only `parse_json_body` (most handlers bypass it). Switching the whole project's JSON parser to one that rejects duplicates (a large change for a check done once at the boundary). Rejecting in each NF (26+ copies).
