## ADR-0400: The UDSF -- Nudsf_DataRepository and Nudsf_Timer on Valkey, stateless replicas

**Status:** Accepted (2026-09-26). Tier 2 NF (parallel-agent range ADR-0400..0419).

**Context.** CLAUDE.md scopes the UDSF (Tier 2) and names Valkey as its store. TS 29.598 defines two
services, each with its own R19 YAML: `TS29598_Nudsf_DataRepository.yaml` (API 1.3.0, root
`/nudsf-dr/v1`, 21 operations) and `TS29598_Nudsf_Timer.yaml` (root `/nudsf-timer/v1`, 6
operations). The TS text (V19.5.0, fetched with `tools/specs/fetch_3gpp_specs.py` into
`specs/3gpp/`) defines the behaviour; the YAML the shapes (ADR-0250). TS 23.502 has no UDSF call
flow; the procedures are TS 29.598 clauses 5.2.2 / 5.3.2.

**Decision.**
1. **Both services, one NF, every operation**: all 27 operations and the four UDSF-originated
   notifications (`recordExpired`, `onDataChange`, `subscriptionExpiryNotification`,
   `timerExpiry`). Both YAMLs joined the codegen list; every DTO used is generated. Two generated
   types cannot carry their content and are handled around, not re-declared: `SearchExpression`
   (object + oneOf -> empty struct; the raw JSON is walked and each leaf decoded with the generated
   `SearchComparison` / `RecordIdList` / `SearchCondition`) and `Block` (opaque json alias -- block
   bytes never pass through JSON).
2. **Valkey only, per-storage hash tag.** Keys are `udsf:{<realm>/<storage>}:...` (layout in
   `nfs/udsf/src/store.hpp`), so a storage is the Cluster shard unit and every transaction stays in
   one slot (Tier-1 scale mandate). Writes are optimistic WATCH/MULTI/EXEC with retry. Search: a SET
   per (tag, value) for EQ, a ZSET per tag (`value\0id`, ZRANGEBYLEX) for GT/GTE/LT/LTE -- no search
   scans the record set. Expiry schedules are ZSETs claimed with ZREM (one replica wins each entry);
   outbound notifications are a Valkey list. No in-process state: `--scale udsf=N` is safe (the test
   runs two replicas and sees one expiry notification).
3. **Realms and storages are configuration** (`storages` in `config/udsf.json`): TS 29.598 has no
   API to create them yet defines REALM_NOT_FOUND / STORAGE_NOT_FOUND. Advertised as
   `UdsfInfo.storageIdRanges` (exact-match patterns).
4. **Conditional requests throughout** (6.1.2.2): strong ETags per record, block, subscription,
   schema; Last-Modified; If-Match / If-None-Match (incl. `*`) / If-Modified-Since -> 304 / 412 with
   the current ETag (CR 0086); Cache-Control max-age from config; get-previous wherever the TS has it.
5. **OAuth2 per TS 29.500 6.7.3**: no token accepted per local config (`oauth2_required`, default
   false as elsewhere); invalid token -> 401 `WWW-Authenticate: Bearer ... error="invalid_token"`;
   valid token without the service-name scope (`nudsf-dr` / `nudsf-timer`) -> 403
   `insufficient_scope`. The finer per-resource scopes are optional in both YAMLs and not required.
6. **sbi-core multipart made subtype-generic, additively**: `parse_any` / `encode_subtype` (no RFC
   2387 `type=`), `Part::content_transfer_encoding`; a zero-length part body used to swallow the next
   part -- fixed. `parse`/`encode` keep their multipart/related contract (`Multipart.*` pass).
7. **Ports** chosen unused by every `config/*.json` after the merge with main (see config/udsf.json).

**Rejected.** One JSON document per record with base64 blocks (every block write rewrites the
record); scan-and-filter search (O(records) per query, Tier-1 mandate); Lua scripts for conditional
writes (WATCH/MULTI gives the same atomicity with tested C++ logic; revisit under contention); an
in-process timer wheel (lost on restart, fires once per replica); treating any realm/storage as
existing (makes the TS's 404 causes unreachable); a Helm chart now (the Tier-2 NFs MFAF/DCCF/ADRF
have none; Docker image + Compose service follow that pattern -- Helm stays Phase 8 debt).

**Tests.** `tests/integration/test_udsf.cpp`, own binary `udsf_integration_tests` (depends on nrf +
udsf only): 5 logic tests + 12 wire tests against a real NRF, two UDSF replicas over TLS 1.3 + mTLS
and real Valkey, each run in a realm unique to the run, cleaned by exact key pattern (never FLUSH).
`api_root_conformance` covers both roots.

