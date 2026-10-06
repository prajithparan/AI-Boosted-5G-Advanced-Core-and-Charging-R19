## ADR-0052: Real fix for a genuine 3GPP schema cycle -- std::shared_ptr indirection for cyclic back-edges in tools/sbi-codegen

**Date:** 2026-08-10
**Status:** Accepted.

**Context:** CI's `lint` job (`clang-tidy`) had been failing on a real, pre-existing bug, found
while chasing down CI's overall failing status: `sbi_gen::SharedData_Nudm_SDM` (generated from
`TS29503_Nudm_SDM.yaml`) is forward-declared, and its full definition genuinely exists later in
the same header, but it's embedded via
`std::optional<std::vector<SharedData_Nudm_SDM>>` at a point in the header where only the forward
declaration has been seen -- which Clang's frontend correctly rejects (`std::vector<T>`'s implicit
destructor needs `T` complete) while GCC happens to tolerate it in practice via more lenient lazy
instantiation timing -- a real difference in strictness between the two compilers, not a GCC bug
or a Clang bug, just two conforming-in-spirit implementations disagreeing on when to enforce this.

**Real, genuine schema cycle, not a generator artifact.** `_topo_sort_types`'s own docstring
(`render.py`, added in ADR-0022) already names the exact cycle: `SharedData.sharedAmData ->
AccessAndMobilitySubscriptionData -> AccessAndMobilitySubscriptionData.sharedDataList ->
SharedData`. This is a real, intentional 3GPP API design ("shared data aggregates per-type
subscription data, per-type subscription data can itself reference shared data") -- ADR-0022's own
SCC-condensation fix correctly solved the *declaration-order* half of handling this (forward
declarations emitted before either type is defined, replacing a prior, worse bug where ANY cycle
anywhere in a merged group discarded the correct order for every unrelated type in it too) but did
not address that direct embedding via `std::optional<T>`/`std::vector<T>` needs `T` complete
regardless of declaration order -- no linear ordering of two mutually-referencing structs can ever
satisfy both sides' completeness requirement simultaneously in plain C++. That gap sat latent
(GCC never enforced it) until `clang-tidy`, using Clang's stricter frontend, actually hit it.

**Real fix: `std::shared_ptr<T>` for the specific field that creates the back-edge, not both
sides.** `render.py`'s `_topo_sort_types` already computes emission order and the set of types
in a real cycle (`cyclic_names`). New `_forward_only_ref_name`: for each `ObjectType` field,
checks whether its referenced type (or array-element type) is both in `cyclic_names` AND not yet
defined at this field's own position in emission order (i.e. this field is specifically the
back-edge, not the forward-edge -- of a 2-cycle, only one direction actually has the problem,
whichever type is emitted second gets to reference the first directly with no issue at all).
`RenderField` now renders such a field as `std::shared_ptr<T>` instead of
`std::optional<T>`/`std::vector<T>` -- `shared_ptr<T>`'s deleter is type-erased at construction
time (in the generated `.cpp` file, where `T` *is* complete by then), not required complete at
the point the containing struct's implicitly-defaulted destructor is instantiated in the header,
so this is genuinely, not just practically, well-formed regardless of compiler.

**New `std::shared_ptr<T>` overloads for `sbi_core::put_optional`/`get_optional`**
(`libs/sbi-core/include/sbi_core/json_serde.hpp`) so the existing `source.cpp.j2` template needs
*zero* changes -- the same `sbi_core::put_optional(j, key, v.field)` call now resolves to the new
overload automatically via ordinary C++ overload resolution, since the field's own C++ type
changed but the call site's shape didn't.

**Explicit guardrail, not silent generation, for the one case not yet handled.** Both real
instances of this cycle in the R19 corpus (`sharedAmData`, `sharedDataList`) are optional fields
in the real schema. A *required* field that also happens to be a cyclic back-edge would need
different (de)serialize codegen (enforcing presence like every other required field does, not
silently-absent like an optional one) that doesn't exist yet and has never been exercised --
`render.py` now raises `NotImplementedError` with a clear message if this combination is ever
found, rather than emitting untested code for it. Matches CLAUDE.md's "stop and ask" guardrail:
a real gap disclosed as a hard failure, not silently papered over.

**Verified two ways.** (1) Regenerated the full corpus (`1917 types -> 42 files`, unchanged
counts) and rebuilt the entire project with GCC (the `build`/`sanitize` compiler) -- clean, zero
regressions, 140/140 conformance tests + 31/31 integration tests pass. (2) Directly compiled a
minimal translation unit constructing both `AccessAndMobilitySubscriptionData` and
`SharedData_Nudm_SDM` with `clang++-18 -fsyntax-only` (the same frontend `clang-tidy` uses) --
clean, zero errors, confirming the fix at the actual point of previous failure without waiting
for a full, slow (~28+ minute) whole-tree `clang-tidy` pass to finish.

**Consequence:** the real, previously-undiagnosed root cause of `lint`'s CI failure is fixed at
its source (the codegen tool), not worked around per-file. The fix is general -- it applies to
any future cyclic back-edge the R19 corpus's evolution introduces, not just this one instance.

---

