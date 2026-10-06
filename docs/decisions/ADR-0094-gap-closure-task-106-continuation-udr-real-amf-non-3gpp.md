## ADR-0094: gap-closure task #106 continuation -- UDR real `amf-non-3gpp-access` context-data resource

### Context

Continuing task #106's UDR resource-type-breadth gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md;
9 of free5GC's ~42+ real TS 29.504 resources closed as of ADR-0083). free5GC's real UDR treats AMF
3GPP-access and AMF non-3GPP-access registration as two distinct resource groups
(`AmfContext3gpp`/`AmfContextNon3gpp`); this project had only ever implemented the 3GPP one. Real,
confirmed-by-YAML-read: `TS29505_Subscription_Data.yaml`'s
`/subscription-data/{ueId}/context-data/amf-non-3gpp-access` path is a genuinely separate resource
from its 3GPP sibling -- distinct schema (`AmfNon3GppAccessRegistration`, not
`Amf3GppAccessRegistration`), same real "GET+PUT only, no PATCH/DELETE" operation shape its
sibling already has (checked directly against the YAML, not assumed uniform).

### Implementation

- `nfs/udr/schema.postgres.sql`: new `udr_amf_non3gpp_context` table (`ue_id` PK, `context`
  JSONB) -- deliberately a separate table from `udr_amf_context`, matching the real, distinct
  spec resource, not a shared document reused across both paths.
- `nfs/udr/src/stores.hpp`/`.cpp`: new `AmfNon3GppContextStore` class (`put`/`get`), same "one
  shared `pqxx::connection`, one mutex" discipline every other UDR store already uses -- a
  deliberately separate class from `AmfContextStore`, not a templated/shared base, matching this
  file's own established one-class-per-real-resource pattern.
- `nfs/udr/src/main.cpp`: new `GET`/`PUT` routes at
  `/subscription-data/{ueId}/context-data/amf-non-3gpp-access` (`QueryAmfContextNon3gpp`/
  `CreateAmfContextNon3gpp`), mirroring the existing 3GPP-access routes' structure (bearer-token
  check, 404-if-absent on GET, 201-vs-204 on PUT based on `is_new`, a new
  `udr_amf_non3gpp_context_write_total` OTel counter), reusing the sbi-codegen-generated
  `sbi_gen::AmfNon3GppAccessRegistration` DTO already vendored from the R19 YAML -- no new codegen
  work needed.

### Real fields discovered via live verification, not guessed upfront

An initial PUT with only `amfInstanceId` and `guami` real 400'd (`ProblemDetails`) against the
already-generated `AmfNon3GppAccessRegistration` DTO's own real required-field validation. Reading
the actual 400 response (not the YAML in isolation) confirmed the full real mandatory set:
`amfInstanceId`, `imsVoPs` (real enum `HOMOGENEOUS_SUPPORT`/`HOMOGENEOUS_NON_SUPPORT`/
`NON_HOMOGENEOUS_OR_UNKNOWN`), `deregCallbackUri`, `guami`. `ratType` is present in the real schema
as an opaque `nlohmann::json` fallback type in this project's codegen output (not a generated
C++ enum) -- a plain string value (`"NR"`) round-trips through it correctly, confirmed live, not
assumed.

### Live verification (real, live PostgreSQL, not self-consistency)

Real curl round-trip against a running `udr` process backed by a real, freshly-migrated
PostgreSQL database: `GET` for an unseeded `ueId` -> real `404`; `PUT` with the full real mandatory
field set -> real `201` with `Location` header and the echoed document; `GET` on the same `ueId`
immediately after -> real `200` with the identical document; a second `PUT` on the same `ueId` ->
real `204` (update path, `is_new=false`). Independently confirmed via a direct `psql` query against
`udr_amf_non3gpp_context` (not just trusting the HTTP response) -- the row is genuinely persisted
in PostgreSQL, not held only in the running process's memory.

### Testing and verification

`udr` built clean. Full `conformance_tests`: unchanged pass count (no new committed automated test
this pass -- same disclosed manual-live-verification precedent ADR-0090/ADR-0091/ADR-0092 already
established for gap-closure slices of this size), zero regressions.

### What this ADR does NOT include

AMF's own registration path does not call this new endpoint yet -- same disclosed "stand up the
surface first, wire consumers in a dedicated later turn" precedent already used for
`provisioned-data` (ADR-0069) and the 3GPP-access resource itself. This closes UDR resource #10 of
free5GC's ~42+; roughly 32 remain a real, open, disclosed gap (docs/CAPABILITY_GAP_ANALYSIS.md).
Task #106 remains open (not fully closed) at this project's own "eventually build every real
resource" bar (`feedback_full_yaml_coverage_mandatory` precedent) -- this is one more real slice,
not the finish.

