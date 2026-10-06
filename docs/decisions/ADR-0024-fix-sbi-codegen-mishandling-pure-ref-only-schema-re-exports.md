## ADR-0024: Fix sbi-codegen mishandling pure `$ref`-only schema re-exports

**Date:** 2026-08-06
**Status:** Accepted

**Context:** Adding `TS29505_Subscription_Data.yaml` (UDR's real schema file) as a codegen pilot
surfaced a third real generator bug. Checked directly against the YAML: `TS29505_Subscription_
Data.yaml` locally "defines" roughly 25 schemas -- `SmfRegistration`, `Amf3GppAccessRegistration`,
`AccessAndMobilitySubscriptionData`, `SdmSubscription`, and others -- that are each nothing but a
pure indirection, e.g.:
```yaml
SmfRegistration:
  $ref: 'TS29503_Nudm_UECM.yaml#/components/schemas/SmfRegistration'
```
i.e. "this name, here, just means UDM's own already-defined schema of the same name" -- UDR's spec
authors re-exporting UDM's types by reference for convenience within their own file's local
namespace, not defining a second, different concept. `schema_to_ir.py`'s `_convert_one` had no
branch for a top-level schema whose entire body is `{"$ref": ...}` -- it fell through to the
`OpaqueType` fallback (`unhandled schema shape, keys=['$ref']`). Combined with ADR-0017's
collision disambiguation (correct in isolation: two different `(source_file, name)` keys sharing a
plain name), this made things actively worse than a plain opaque fallback: each of these ~25 names
got disambiguated into a spurious, disconnected `TypeName_Subscription_Data` opaque type,
completely unrelated to the real `TypeName_Nudm_UECM`/`TypeName_Nudm_SDM` struct UDM already uses
for the identical concept -- exactly the kind of duplicate-type problem ADR-0010 picked a custom
generator specifically to avoid.

**Decision:** `SchemaRegistry.resolve_ref` (`loader.py`) now transparently follows pure-`$ref`
indirection schemas to their real target before returning, via a new `pure_ref_target(schema)`
helper (returns the inner `$ref` string iff the schema dict is exactly `{"$ref": X}`, else `None`)
and a loop with cycle detection (`ValueError` on a repeated `(file, name)` key). Any caller asking
to resolve `TS29505_Subscription_Data.yaml#/components/schemas/SmfRegistration` now transparently
gets back `("SmfRegistration", <the real object schema>, "TS29503_Nudm_UECM.yaml")` -- the actual
shape, from its actual owning file -- never the pass-through wrapper. `schema_to_ir.py`'s
`Converter.convert_files` seeding loop (which enqueues every locally-defined schema of a pilot
file directly, without going through `resolve_ref`) now skips any entry where `pure_ref_target`
returns non-`None` -- these names never need their own IR type; anything that references them by
name resolves straight through to the real target instead.

**Verification:** regenerated from scratch -- type count dropped 1549 -> 1512 (the ~25 spurious
disconnected types no longer generated) and opaque-fallback count dropped 154 -> 100 (exactly
matching -- these were misclassified as opaque before). Direct inspection: exactly one
`struct SmfRegistration`/`Amf3GppAccessRegistration`/`AccessAndMobilitySubscriptionData` each now
exist, matching UDM's already-generated definitions (confirmed via `grep -c`). Minimal standalone
`g++ -fsyntax-only` compile using all three types together succeeds. Full project rebuild from a
clean configure succeeds; all 25 pre-existing tests pass unchanged -- purely a correctness fix
(fewer, more correct types), no behavior change for anything that already compiled correctly.

**Rejected alternative:** special-case UDR's route handlers to use the (wrongly) disambiguated
`*_Subscription_Data` opaque types directly, treating them as `nlohmann::json` passthrough.
Rejected as a direct violation of CLAUDE.md's "never hand-write a DTO the YAML can generate" rule
in spirit -- these fields have full, real, already-generated typed shapes one file over; using an
opaque passthrough instead would be pure workaround, not a fix, and would have left UDR's DTOs
structurally inconsistent with UDM's for what are explicitly, per the spec text itself, the exact
same schema.

**Consequence:** this is the third real generator bug found only by actually compiling generated
output against a growing pilot-file set (after ADR-0017's cross-file name collision and ADR-0022's
topo-sort/alias-ordering bug). The pure-$ref-reexport pattern is likely to recur as more NF YAML
files reference each other's schemas this way (a natural, common pattern once enough of the API
surface is covered) -- this fix generalizes to any future occurrence, not just this file pair.

---

