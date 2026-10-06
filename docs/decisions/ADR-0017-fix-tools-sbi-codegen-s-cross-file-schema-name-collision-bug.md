## ADR-0017: Fix tools/sbi-codegen's cross-file schema name collision bug

**Date:** 2026-08-05
**Status:** Accepted

**Context:** While building AMF's `Namf_Communication` surface, cross-referencing the already-agreed
`AMFStatusChangeSubscribe` procedure's request schema against the generated C++ output surfaced a
real generator defect, not a spec ambiguity: `TS29518_Namf_Communication.yaml` defines its own local
`SubscriptionData` schema (`{amfStatusUri (required), guamiList}`), but only ONE `SubscriptionData`
struct existed anywhere in generated output, and it had NRF's shape
(`{nfStatusNotificationUri (required), subscrCond, ...}` from `TS29510_Nnrf_NFManagement.yaml`) --
AMF's real schema had been silently dropped.

**Root cause, verified by reading `tools/sbi-codegen/sbi_codegen/loader.py`:**
`SchemaRegistry.schemas` was keyed by bare schema name only (`dict[str, tuple[schema, source_file]]`),
and `load_file()` only inserted a schema `if name not in self.schemas` -- first file loaded wins
silently, every later file's same-named schema is discarded with no warning. The loader's own
docstring asserted this was safe ("3GPP's OpenAPI files consistently reuse the same schema name for
the same concept across files"). Checked, not assumed: a script comparing every locally-defined
schema name shared by two or more of the 5 current pilot files
(`TS29510_Nnrf_NFManagement/NFDiscovery/AccessToken.yaml`, `TS29518_Namf_Communication.yaml`,
`TS29571_CommonData.yaml`) found 7 collisions -- `SubscriptionData`, `NFProfile`, `NFService`
(NFManagement vs. NFDiscovery), `Ipv4AddressRange`, `Ipv6PrefixRange`, `TransportProtocol`,
`MbsSession` (NFManagement vs. CommonData) -- and **every one of the 7 has genuinely different
content** (structural dict comparison, descriptions excluded), not an identical redefinition. The
docstring's premise was false for locally-declared-but-coincidentally-named schemas; it only holds
for the deliberate pattern of one file externally `$ref`-ing another's canonical type (e.g.
`NfInstanceId` from `TS29571_CommonData.yaml`), which never hits this collision path since those
files don't also redeclare it locally.

**Blast radius, checked:** `grep` across `nfs/`, `libs/sbi-core/`, `tests/` for every one of the 7
colliding type names found zero hand-written C++ references to any of them -- NRF works with raw
`nlohmann::json` for `SubscriptionData`, not the generated struct (`nfs/nrf/src/registry.hpp`).
Renaming the generated C++ types was therefore safe with no call-site fallout.

**Decision:**
- `SchemaRegistry.schemas` re-keyed to `dict[(source_file, name), schema]`. `load_file()` now
  registers unconditionally (idempotent per-file, not per-name) -- no more silent drops.
  `resolve_ref()` for an internal (`#/...`) ref now always resolves within the *calling* file's own
  namespace, never an arbitrary other file's same-named schema.
- `schema_to_ir.py`'s `Converter` now runs a two-pass build: types are collected keyed by
  `(source_file, yaml_name)` (`TypeRef` gained a `ref_key` field to carry this through), then a final
  `_disambiguate()` pass assigns every key a guaranteed-unique C++ name -- the plain
  `cpp_type_name(name)` when only one file defines that name (the overwhelming majority: 1092 of the
  previous 1104-type output), or `{name}_{file_tag}` (e.g. `SubscriptionData_Nnrf_NFManagement` /
  `SubscriptionData_Namf_Communication`) when multiple files collide on it -- then patches every
  `IRType.name` and every `TypeRef.cpp_name` to match before handing off to `render.py` (which itself
  assumes name-uniqueness via `name_to_type`/`name_to_file` dicts and would have silently re-collided
  the two variants back into one if the renaming weren't resolved before reaching it).
- `tests/conformance/validate_structural_conformance.py` updated for the new `(file, name)`-keyed
  registry API (it looked up `registry.schemas["Guami"]`/`["NFType"]` directly).

**Verification:** regenerated from scratch -- type count went from 1092 to 1104 (12 additional
distinct types now correctly emitted instead of silently merged/dropped, more than the 7 found in the
5-pilot-file check above since the fix applies to the full transitive closure, not just the pilots).
`SubscriptionData_Nnrf_NFManagement` and `SubscriptionData_Namf_Communication` both now exist as
distinct structs with their real, correct fields (confirmed by direct inspection of the generated
header). Full project rebuild green; all 6 existing tests (integration + conformance round-trip +
structural) pass unchanged -- NRF's behavior is provably unaffected by this fix.

**Rejected alternative:** hand-write AMF's `SubscriptionData` DTO instead of fixing the generator.
Rejected as a direct violation of CLAUDE.md's "never hand-write a DTO the YAML can generate" rule --
the correct fix belongs in the shared generator, especially since the same collision pattern will
keep recurring as more NF YAML files are added as codegen roots in later phases.

**Consequence:** the disambiguation-suffix naming scheme (`{Name}_{Nnrf_NFManagement|Namf_Communication|...}`)
is now a permanent, load-bearing part of the generated API surface for every currently-colliding type
and any future collision the same mechanism catches. Future NF work referencing a schema name known to
collide must use the qualified name; this is discoverable by grepping the generated header for the
plain name if a compile error suggests it's missing.

---

