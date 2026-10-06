## ADR-0010: Custom Jinja2 generator for `tools/sbi-codegen`, not openapi-generator

**Date:** 2026-08-04
**Status:** Accepted

**Context:** Phase 1 requires evaluating openapi-generator's C++ targets against a custom Jinja
generator on three pilot files (`TS29510_Nnrf_NFManagement.yaml`, `TS29518_Namf_Communication.yaml`,
`TS29571_CommonData.yaml`) and recommending one with evidence. Correction to PROMPT.md's own text:
current openapi-generator has no `cpp-restsdk` *server* target -- `cpp-restsdk` is client-only.
Available C++ server targets: `cpp-httplib-server`, `cpp-oatpp-server`, `cpp-pistache-server`,
`cpp-qt-qhttpengine-server`, `cpp-restbed-server`(-deprecated). `cpp-pistache-server` was used as
openapi-generator's representative since PROMPT.md named Pistache explicitly.

**Framework HTTP/2 support (checked, not assumed):** none of Pistache, cpp-httplib, restbed, or
oatpp advertise HTTP/2 in their vcpkg port descriptions, and this matches their known designs
(Pistache, cpp-httplib, restbed are HTTP/1.1-only by construction). This reinforces ADR-0004: no
openapi-generator C++ server target can replace `libs/sbi-core`'s hand-rolled HTTP/2 server, so
codegen server/route scaffolding is out of scope regardless of which model-generation approach
wins -- only the DTO/serializer layer is a real candidate for adoption.

**openapi-generator model output -- concrete defects found by actually compiling it:**
1. **anyOf-open-enum (3GPP's pervasive `anyOf: [{enum:[...]}, {type: string}]` pattern, e.g.
   `NFType`, `NFStatus`) generates a completely empty class.** `NFType.h`/`.cpp` from
   `cpp-pistache-server` has no data member; `to_json` emits `{}`; and `operator==`'s body is a bare
   `return;` in a `bool`-returning function -- **confirmed with `g++ -std=c++20 -fsyntax-only`: this
   does not compile** (`error: return-statement with no value, in function returning 'bool'`). This
   pattern covers dozens of types across the R19 API surface, not an edge case.
2. **Pattern/format constraints are silently dropped.** `Fqdn`'s regex pattern
   (`TS29571_CommonData.yaml`) and `Ipv6Addr`'s `allOf`-combined patterns have zero enforcement in
   generated code -- `grep -rl "std::regex" model/` finds regex usage only in the generator's own
   hardcoded RFC 3339 date/date-time helper, nowhere else.
3. **Cross-file `$ref` duplication.** Generating `TS29510_Nnrf_NFManagement.yaml` and
   `TS29518_Namf_Communication.yaml` independently (as real per-NF codegen invocations would)
   produces 84 duplicate files (42 types x .h+.cpp) for shared `TS29571_CommonData.yaml`-derived
   types (`PlmnId`, `Guami`, `InvalidParam`, `AccessTokenReq`, ...) -- an ODR risk if ever linked
   together, and definite drift risk if maintained per-NF.
4. No `discriminator:` usage exists in the three pilot files (checked, not assumed absent) --
   openapi-generator's polymorphism handling was not evaluated since there is nothing to evaluate
   it against here.
5. `nullable` fields use an `...IsSet` boolean-flag pattern rather than `std::optional`, which is
   functional (not a defect), just a different convention than this project's `std::optional`-based
   `sbi_core::put_optional`/`get_optional` helpers already in use in `libs/sbi-core`.

**Decision:** Build `tools/sbi-codegen` as a custom Python + Jinja2 generator
(`tools/sbi-codegen/`), not a wrapper around openapi-generator. It:
- Resolves `$ref` (internal and cross-file) into a single schema registry so a shared type (e.g.
  `Guami`) is generated exactly once regardless of how many pilot/NF files reference it, eliminating
  defect #3 above.
- Represents the anyOf-open-enum pattern as a plain-`std::string`-backed struct with named
  `static inline const std::string` constants for known values -- any value (known or not)
  round-trips correctly, fixing defect #1. Proven in `tests/conformance/test_round_trip.cpp`
  (`NFTypeUnknownValueRoundTrips`) with a value that is not in the known-enum list.
  See `libs/sbi-core/include/sbi_core/sbi_headers.hpp`.
- Enforces `pattern` (direct or `allOf`-combined) via a generated `validate_<Type>(const
  std::string&)` using `std::regex`, fixing defect #2. Proven in `tests/conformance/test_round_trip.cpp`
  (`AmfIdPatternValidation`) with both an accepted and a rejected value.
- Uses `std::optional<T>` for `nullable`/non-required fields via the existing
  `sbi_core::put_optional`/`get_optional` helpers (`json_serde.hpp`), collapsing the
  absent-vs-explicit-null distinction into one state -- a disclosed simplification (most real-world
  OpenAPI-to-C++ generators do the same; a true tri-state would need
  `std::optional<std::optional<T>>` or an explicit sentinel, not worth the complexity for a Phase 1
  prototype).
- Falls back to `using Name = nlohmann::json;` (opaque passthrough) for schema shapes not
  confidently modeled (inline-composed properties, non-string enums, unrecognized `allOf` member
  shapes) rather than guessing. Across the full transitive closure of the three pilot files (1076
  types, 22 source YAML files pulled in transitively -- 3GPP's own schemas are far more
  interconnected than "three pilot files" suggests), **114 types (10.6%) are opaque fallbacks**;
  the rest (583 objects, 193 aliases, 186 open-enums) are fully typed. This ratio is reported
  honestly, not hidden.

**A genuine structural finding, not a codegen bug:** the three pilot files' transitive `$ref`
closure forms one large strongly-connected component spanning 22 of the 33 initially-encountered
source files (`TS29571_CommonData.yaml` <-> `TS29510_Nnrf_AccessToken.yaml` <->
`TS29520_Nnwdaf_EventsSubscription.yaml` <-> ... ). A first, naive one-header-per-source-file version
of this generator hit real "not declared in this scope" compile errors from `#pragma once` silently
no-op'ing a re-entrant `#include` before the needed type was defined -- a genuine circular
C++-header dependency caused by a genuine circular schema dependency in 3GPP's own YAML, not
something either generator invented. Fixed by computing strongly-connected components (Tarjan's
algorithm, `render.py`) over the file dependency graph and emitting **one merged header per SCC**,
with types topologically sorted *within* the merged group by field-level dependency. Result: 11
output file-groups instead of 33, one of which (`TS29122_CommonData_grp.hpp`) is a 546KB file
covering all 22 mutually-dependent source files with a citation block listing all of them.
**Trade-off, disclosed:** this means a change to any one of those 22 files' schemas forces
recompilation of everything in the merged group -- acceptable for a Phase 1 prototype proving
feasibility, but worth revisiting (e.g. finer-grained per-type headers with forward declarations
where the field is used via reference/pointer) if build-time incrementality becomes a real pain
point once more NFs are generated in Phase 2+.

**Four more real bugs found only by actually compiling the generated output** (not by inspection --
each was caught by `g++ -fsyntax-only` on the real generated files, then fixed and re-verified):
- Multi-line 3GPP YAML descriptions (common -- long prose paragraphs with embedded quotes and
  clause references like "3GPP TS 23.003") only had their first line `//`-commented; subsequent
  lines leaked as raw uncommented text into the file. Fixed with a `cppcomment` Jinja filter that
  prefixes every line.
- JSON field names that legitimately start with a digit (`5qi`, `5gMmCapability` -- real 5G QoS
  Identifier field names) are invalid C++ identifiers. Fixed with an `n`-prefix shim
  (`_cpp_field_name`) that only affects the emitted C++ member name, not the JSON wire name.
- The same issue for *type* names (`5GDdnmfInfo`, `2G3GLocationArea` are real schema names), plus
  `and`/`or`/`not`/etc. (C++'s alternative-operator keywords, easy to forget) colliding with real
  field names. Fixed with a shared `cpp_type_name`/reserved-word sanitizer applied consistently at
  both a type's declaration and every reference to it.
- A field whose name is identical to its own type's name (e.g. field `Snssai` of type `Snssai`,
  observed in `SmallDataRateStatusInfo`) compiles but triggers `-Wchanges-meaning` and genuinely
  breaks subsequent name lookup within the same struct once GCC's strict mode is considered. Fixed
  by appending `_` to a field name when it collides with its own bare type name.

**Rejected alternative:** Use openapi-generator for the model/DTO layer specifically (since server
scaffolding was already ruled out by the HTTP/2 finding above), patching its Mustache templates to
fix the anyOf-open-enum bug. Rejected: openapi-generator is a Java tool with its own template
release cadence; maintaining a fork of its templates long-term is a comparable or larger maintenance
burden than owning a ~600-line Python generator we already have full visibility and control over,
and the duplication problem (defect #3) is architectural to openapi-generator's per-invocation model
generation, not fixable via template patches alone.

**Consequence:** `tools/sbi-codegen/generate.py` is wired into the CMake build
(`libs/sbi-generated/CMakeLists.txt`) as a configure-time + rebuild-time step (re-runs when the
generator, its templates, or the pilot YAML change; adding/removing a pilot file needs a
re-configure for `file(GLOB)` to notice new output files -- a known, disclosed CMake limitation of
globbing dynamically generated sources). Requires `python3` with `jinja2`/`pyyaml` on `PATH` --
**not** vcpkg-managed, since there is no vcpkg C++ substitute for a Python code generator; CI
installs these explicitly. This generator's ~10.6% opaque-fallback rate and disclosed
nullable-collapsing simplification are expected to shrink incrementally as Phase 2+ NF work
surfaces schema shapes not yet handled -- it is deliberately not attempting to be a complete,
general-purpose OpenAPI-to-C++ compiler on day one.

---

