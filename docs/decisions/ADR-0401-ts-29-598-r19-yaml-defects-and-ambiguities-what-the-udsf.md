## ADR-0401: TS 29.598 R19 YAML defects and ambiguities -- what the UDSF does with each

**Status:** Accepted, with **open questions for the owner** (none resolved by inventing a field).

1. **RecordMeta.schemaId is absent from the parsed YAML**: its lines sit inside RecordMeta's
   `example: >-` folded scalar (verified with PyYAML); TS table 6.1.6.2.3-1 lists it. The meta is
   stored verbatim (a sent schemaId is kept and returned) but not interpreted: SearchCondition.schemaId
   on records -> 400 (never silently ignored), SCHEMA_IN_USE is checked against timers only
   (Timer.schemaId is in the YAML), TagType `presence` / `UNIQUE_KEY` not enforced on records.
   **Question:** a documented codegen overlay from the TS for this field, or wait for a fixed YAML?
2. **GetMetaSchema 200 references the multipart `RecordBody` response**; TS table 6.1.3.9.3.1-3 says
   MetaSchema. The UDSF returns `application/json` MetaSchema (the TS). **Question:** confirm.
3. **tag-count-filter (AdvancedCounting)**: YAML `schema: CountExpression` (form-exploded single
   object) vs TS `map(CountExpression)`; Annex B.2 output contradicts the YAML's TagCount. Not built;
   feature 5 not advertised (consumers shall then not send it); the parameter is answered 400.
   **Question:** which encoding for a future implementation?
4. **DeleteNotificationSubscription 200** is an array in the YAML, one object in the TS table: the
   YAML shape is followed.
5. **client-id** (`schema: ClientId`, no `content:`) is parsed form-exploded (`?nfId=` / `?nfSetId=`)
   per OpenAPI defaults; a JSON-encoded `client-id=` is also accepted, leniently.
6. **ttl above the operator ceiling** (`max_record_ttl_seconds`, 0 = none): create -> 201 with the
   applied ttl; update with get-previous -> 403 TTL_VALUE_NOT_ALLOWED; update without -> 200 with the
   stored record.
7. **SearchComparison GTE with tag ""** selects everything (6.1.3.2.3.2), for search, bulk delete
   and timer filters alike.

