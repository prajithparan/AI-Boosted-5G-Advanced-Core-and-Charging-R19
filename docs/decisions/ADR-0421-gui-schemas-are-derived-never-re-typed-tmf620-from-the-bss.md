## ADR-0421: GUI schemas are derived, never re-typed -- TMF620 from the bss_sid DTOs, provisioning cross-checked against its source

**Date:** 2026-09-26. **Status:** accepted.

**TMF620 (ProductOffering, ProductOfferingPrice).** `gui/schema-gen/derive_tmf620_schema.py` parses
`libs/bss-sid/include/bss_sid/product.hpp` (the structs product-catalog deserializes every request
into; themselves transcribed from the real TMF620 v4.1.0 swagger) and emits
`gui/web/src/schemas/tmf620.derived.json`: `std::string` -> required string (from_json uses
`at()`), `optional<T>` -> optional, `vector<T>` -> array, nested structs inlined, `nlohmann::json`
-> untyped. A small overlay (`overlay-tmf620.json`) adds only SERVICE-side create rules, each with a
source citation (e.g. `name` required on ProductOffering create, `store.cpp`; `id`/`href`
server-assigned; which date-times `ts_in` validates) -- and the generator refuses any overlay rule
naming a field the DTO does not have, so the overlay can constrain but never invent.
Independent check: `test_tmf620_schema_roundtrip.cpp` builds a maximal instance from the emitted
schema (every property, one-element arrays, non-integral numbers) and round-trips it through the
REAL `bss_sid::from_json`/`to_json` -- equality proves every key and type; a second test removes
each property in turn and checks `from_json` throws exactly for the schema's `required`.
ProductOfferingPrice has **no** required top-level field (true of the DTO, the DB and the TMF620
swagger) -- stated, not "fixed".

**Provisioning customer order.** A project-owned OAM API with no DTO to derive from, so the schema is
hand-transcribed from `provisioning_store.hpp`'s documented request shape -- and
`check_provisioning_schema.py` re-derives from the running code: every key the service reads
(`jval`/`.value`, per nested object, including `initialBalance.usageType`, which the header comment
omits), every validation regex and which fields it guards unconditionally (= required: `supi`,
`sim.k`, `sim.opc`), and the `segment`/`chargingMode` enums from the charging DB CHECK
constraints. `usageType`'s documented values come from a column comment only, so they are shown as
a description, not enforced as an enum. `sim.k`/`sim.opc` are `writeOnly` (drives the masked input).
`test_schema_checks.py` proves both checks REJECT realistic drift (10 mutations).

**Rendering / validation.** The UI schema is generated from the schema (`uischema.ts`) because JSON
Forms' default generator drops untyped properties silently and the vanilla set has no nested-object
renderer; three small renderers fill the gaps (nested object, writeOnly secret -- a CSS-masked
text input, deliberately not `type=password` so no browser offers to SAVE a SIM key -- and raw JSON
for untyped fields). **AJV is not executed**: it compiles schemas with `new Function`, which the
console's CSP (`script-src 'self'`, no `unsafe-eval`) forbids, and weakening the CSP is the wrong
trade. `validate.ts` interprets the closed keyword set the derived schemas use; the BFF and the
service validate again. Cost, disclosed: errors are listed at submit, not inline per field.
`scripts/render_smoke.py` loads the built bundle in headless Chromium under the production CSP with
canned /api responses and fails on any page/console error or "No applicable renderer".

