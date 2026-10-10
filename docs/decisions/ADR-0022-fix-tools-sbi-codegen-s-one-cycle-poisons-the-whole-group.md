## ADR-0022: Fix tools/sbi-codegen's "one cycle poisons the whole group" topo-sort bug

**Date:** 2026-08-06
**Status:** Accepted

**Context:** Adding `TS29503_Nudm_UECM.yaml`/`TS29503_Nudm_SDM.yaml` as codegen pilot files for
UDM produced a `TS29122_CommonData_grp.cpp` that failed to compile with dozens of "has no member
named X" / "was not declared in this scope" errors across many, mostly unrelated types
(`AllowedNssai`, `Point`, `LocationArea`, `RegistrationDatasetNames`, ...). Root-caused (not
assumed) via a minimal standalone reproduction (`g++ -fsyntax-only` on a tiny test file including
just the generated header) plus a direct Tarjan-SCC probe of the IR graph: a **real, genuine 3GPP
schema cycle** -- `SharedData.sharedAmData` -> `AccessAndMobilitySubscriptionData` ->
`AccessAndMobilitySubscriptionData.sharedDataList` -> `SharedData` (a real "shared subscription
data aggregates per-type data, per-type data can itself be marked shared" pattern, not a generator
artifact) -- combined with a real generator bug in `render.py`'s `_topo_sort_types`: the instant
its DFS found ANY cycle anywhere in a merged group, it discarded the entire computed ordering and
fell back to raw input order for **every type in the group**, not just the two cyclic ones. A
2-type cycle was silently corrupting the emission order for (in this group) over a thousand
otherwise-acyclic, unrelated types.

**A second, related gap found while fixing the first:** `_referenced_names` (the function that
walks a type's fields to build the dependency graph) only ever looked at `ObjectType` fields.
`AliasType` (`using X = std::vector<Y>;`) dependencies on other named types in the same group were
never tracked at all -- both for topo-sort ordering AND for cross-group `#include` computation.
Combined with `header.hpp.j2`'s template structure (which always emits the opaque, then alias,
then open-enum, then object blocks in that fixed category order, regardless of what the topo-sort
computes), any alias depending on a struct/enum type -- e.g. `using RegistrationDatasetNames =
std::vector<RegistrationDataSetName>;` where `RegistrationDataSetName` is an open-enum struct --
was structurally guaranteed to reference it before its definition, independent of the first bug.
This is why fixing only the cycle-poisoning bug still left `RegistrationDatasetNames`-shaped
failures; both needed fixing together.

**Decision:**
- `_topo_sort_types` rewritten to use proper SCC condensation: compute strongly-connected
  components via the existing Tarjan implementation, build the condensation graph (guaranteed
  acyclic by construction -- SCCs cannot cycle with each other), topologically sort *that*, and
  return `(ordered_names, cyclic_names)` where `cyclic_names` is only the (typically tiny) set of
  types actually participating in a real cycle. Every acyclic type elsewhere in the group keeps
  its correct dependency order regardless of an unrelated cycle existing somewhere else in the
  same group. Only `ObjectType` nodes can have outgoing edges (enums/aliases-as-targets never
  originate one via the old field-walk), so a multi-member SCC can only ever consist of
  `ObjectType`s -- meaning `struct X;` forward declaration is always sufficient and correct for
  breaking it, never attempted on an alias (which can't be forward-declared in C++).
- `_referenced_names` extended to also extract dependencies from `AliasType.cpp_underlying` via
  identifier tokenization (regex) rather than adding a structured `TypeRef` field to `AliasType` --
  the only thing that matters for dependency tracking is which names appear, and every call site
  already filters the result against a known-names set, so stray non-type tokens (`std`, `vector`)
  are harmless noise, not false edges.
- `render()` now computes cross-group `#include` deps uniformly for every kind (previously
  `ObjectType`-only), and separately computes `alias_forward_decls`: any struct/enum-shaped type an
  `AliasType` in the group depends on, forward-declared unconditionally (not just when part of a
  detected cycle), since the alias block's fixed position before the enum/object blocks makes that
  dependency direction structurally impossible to satisfy any other way.
- `header.hpp.j2` emits a `// Forward declarations ...` block (the union of both sources above)
  immediately after `namespace sbi_gen {`, before the opaque/alias/enum/object blocks.

**Verification:** minimal standalone compile (`g++ -fsyntax-only` against just the regenerated
header) went from ~40 distinct errors to a clean compile. Full project rebuild from a completely
clean `build/` directory (no stale-cache possibility) succeeds. All 21 pre-existing tests
(NRF/AMF/SMF integration, multipart unit tests, round-trip/structural conformance) still pass
unchanged, proving the fix is purely additive (extra forward declarations, more accurate ordering)
with no behavioral change for code that already compiled correctly.

**Rejected alternative:** patch around this one specific cycle (e.g. demote
`SharedData.sharedAmData` or `AccessAndMobilitySubscriptionData.sharedDataList` to an opaque
`nlohmann::json` fallback to break the cycle without touching the topo-sort algorithm itself).
Rejected -- would have fabricated a simplification not called for by the schema (both fields are
perfectly representable, real 3GPP types; the cycle is a normal, valid C++ mutual-reference
pattern once forward-declared) and would not have fixed the underlying flaw, which was already
guaranteed to resurface -- with a different, unpredictable set of collateral damage -- the next
time any future NF's pilot file introduced a different cycle.

**Consequence:** this is the second real generator bug found only by actually compiling generated
output against a growing pilot-file set (after ADR-0017's cross-file name collision) -- both
precisely the kind of bug `docs/DECISIONS.md` ADR-0010 already anticipated ("this generator is
deliberately not attempting to be a complete, general-purpose OpenAPI-to-C++ compiler on day
one... expected to shrink incrementally as Phase 2+ NF work surfaces schema shapes not yet
handled"). No known residual gaps in this area, but per that same expectation, not asserted as the
last one either.

---

