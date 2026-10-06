## ADR-0384: product-catalog cut over to the consolidated charging DB -- the normalized TMF620 model, made lossless

**Date:** 2026-09-25. **Status:** accepted (user-directed: "Please proceed product catalog, don't
wait"). First service cut over to the consolidated `charging` DB (DB-per-domain rule): until now
only `bss/provisioning` wrote there, so a customer onboarded today held a `product_subscription`
pointing at a catalog the running product-catalog / CHF never saw.

**Decision.**
1. `bss/product-catalog` persists TMF620 ProductOffering / ProductOfferingPrice /
   ProductSpecification in schema `product_catalog` of the `charging` DB (normalized tables from
   `30-product.sql`), replacing its own per-service DB of one-row-per-resource JSONB. Every
   statement is schema-qualified (the DB is shared by several NFs' schemas).
2. **The normalized model is made lossless, not the API narrowed.** Measured field by field against
   the `bss_sid` DTOs the API accepts, `30-product.sql` dropped data the JSONB store kept. New
   `deploy/db/charging/31-product-lossless.sql` (idempotent) adds:
   - `href`/`name`/`version`/`targetProductSchema` of every reference;
   - `offering_agreement` (AgreementRef is a list; the single `agreement_id` column is dropped);
   - `ordinal` on every child table so lists round-trip in posted order;
   - surrogate keys for ProductSpecificationCharacteristic / ...CharacteristicValueUse, with the
     TMF id in its own column -- TMF620 scopes those ids to their owner, and two prices both using
     a `ratingGroup` value-use would have collided on the PK;
   - separate TMF ids on relationship/bundle/tax rows (optional in TMF620, surrogate PKs here);
   - unbounded `NUMERIC` for `price_value` / tax amounts (`NUMERIC(18,4)` rounds per-octet prices);
   - server-id sequences and `product_catalog.audit_record` (ADR-0060's trail, same shape).
3. **Relational integrity surfaces as client errors:** a reference to a non-existent price/spec,
   a duplicate reference in one list, a missing `name` (required on TMF620 create) or a malformed
   date-time -> **400**; deleting an entity still referenced (a price an offering uses, an offering
   a customer's `product_subscription` holds, a spec an offering names) -> **409**. The 400 detail
   names the offending key, not internal tables.
4. **Reads are set-based.** The CHF fetches the whole offering list on every rating decision
   (`charging_engine.cpp`, no cache), so `list()` runs one query per table, never one per offering.
5. **Wiring.** `deploy/db/init-domain-dbs.sh` creates `charging` (applying `deploy/db/charging/*.sql`
   in file order) and `orchestration` on the postgres-chf instance -- as a compose init script and
   as a CI step in both jobs (nothing applied that DDL before; the local DB had been built by
   hand). product-catalog's config, compose env and CI `PRODUCT_CATALOG_DATABASE_URL` /
   `TEST_POSTGRES_URL` point at `postgres-chf/charging`.

**Rejected.**
- *Narrow the API to what `30-product.sql` stores* -- silent data loss on a TMF620 API (multiple
  agreements, reference names/hrefs, list order, price precision).
- *Store the lossy fields back as JSONB blobs on the parent* -- re-creates the model the SID rebuild
  replaced; references stay unqueryable.
- *One query per offering to reassemble* -- would put dozens of round trips on every CHF rating call.
- *Drop the FKs on reference columns* -- the normalized model's point is integrity; kept, with 400/409.

**Measured (same machine, 102 offerings each with a price + 2 characteristic value-uses, 30 GETs of
the full list):** old JSONB store p50 16.0 ms / p90 17.3 ms; new normalized store **p50 13.2 ms /
p90 15.5 ms**. No regression on the CHF hot path.

**Disclosed.**
- Two canonicalisations: date-times come back as UTC RFC 3339 with milliseconds (`...+05:30` in,
  `...Z` out); an object whose members are all absent (`"price": {}`) comes back absent.
- References are now enforced: an offering can no longer name a price/spec that does not exist in
  this catalog (TMF620 permits cross-catalog refs; this deployment does not, deliberately).
- The lab catalog in the OLD per-service DB (2 plans + 3 test offerings) was **not migrated**;
  it has no seed script and nothing references it. The old `postgres` compose service is now
  unused by product-catalog and can be retired with the other per-service DBs.
- balance-management, subscriber-management, roaming-interconnect and the CHF's `chf_rating`
  store still use their own DBs -- the next slices of the same cut-over.
- `30-product.sql` itself remains non-re-runnable (plain CREATE TABLE); only `31-` is idempotent.

**Tests.** New `tests/integration/test_product_catalog_lossless.cpp`: fully populated
spec/price/offering (every DTO field, every list >= 2, TMF-local ids reused across owners, three
agreements, sub-cent price) round-trip with exact JSON equality through `get()` AND `list()`;
date-time canonicalisation + rejection; 400 on a dangling reference / missing name, 409 on deleting
a referenced price, and a refused delete leaves the row. Existing `ProductCatalogPostgres*` pass
against the new DB. Verified locally 2026-09-25 through the real service: TMF620 POSTs -> 201,
dangling ref -> 400, delete in-use price -> 409; and an order through `bss/provisioning` for an
offering created via the TMF620 API completed, its `product_subscription` FK-joined to that offering
in `charging`. The full DDL chain applies to a fresh PostgreSQL. `CapScopedCharging` (needs Doris)
is left to CI.

