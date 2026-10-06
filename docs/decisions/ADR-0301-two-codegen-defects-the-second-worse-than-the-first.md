## ADR-0301: two codegen defects, the second worse than the first

**Date:** 2026-09-05. **Status:** accepted. **Found:** wiring the first of NEF's 58 AF-facing
YAML files (ADR-0294's largest finding). Fourth generator defect found this way, after ADR-0022,
ADR-0024 and ADR-0190.

Adding `TS29522_TrafficInfluence.yaml` to the codegen set produced
`using TrafficInfluSub_TrafficInfluence = nlohmann::json` -- an opaque alias with no fields and no
validation. Building NEF's AF-facing routes on that would have given an API that accepts anything
and validates nothing, which is the exact opposite of what "every DTO is generated from the R19
YAML" is supposed to guarantee. So the generator got fixed instead.

### Defect 1: a constraint-only `allOf` member opaqued the whole type

The recorded reason was `allOf member not $ref/object/pattern-only: ['oneOf']`. The `allOf` in
question carries **no data at all**:

```yaml
allOf:
  - oneOf: [required:[afAppId], required:[trafficFilters], required:[ethTrafficFilters], ...]
  - oneOf: [required:[ipv4Addr], required:[ipv6Addr], required:[macAddr], required:[gpsi], ...]
```

Every branch is a bare `required` list. 3GPP uses this shape constantly to express mutual
exclusivity, and all of the type's real fields sit in the sibling `properties:` block the generator
already handled correctly. A fully-modellable type was being discarded.

`_is_constraint_only_member()` now recognises `oneOf`/`anyOf`/`not`/`required` combinations that
carry no data, and lets them contribute zero fields. Deliberately conservative: anything with
`properties`, `$ref`, `type`, `items` or `enum` anywhere in it still takes the opaque path rather
than having data silently dropped.

### Defect 2 (worse): sibling `properties` were never merged under `allOf`

Fixing defect 1 made the type a struct -- **an empty one**. `struct TrafficInfluSub { };`.

That is worse than the opaque fallback it replaced. An opaque `nlohmann::json` preserves data it
cannot validate; an empty struct compiles, serves requests, and **silently discards every field**.
It would have looked correct in every status table.

The `allOf` branch returned only fields merged from its members and never looked at the schema's own
sibling `properties`. JSON Schema composes both. This was latent because every previously non-opaque
`allOf` type drew its fields from `$ref`/`object` members, so dropping siblings was invisible --
`TrafficInfluSub` is the first case where the `allOf` is *purely* constraints and *every* real field
is a sibling. The schema's own block is now processed as an implicit final member, through the same
duplicate-detection path ADR-0190 and ADR-0209 already hardened.

**How it was caught matters more than the fix.** After defect 1, the fallback count dropped and the
type became a struct -- and "it is a struct now" is exactly the kind of green signal that ends an
investigation early. Printing the generated fields, rather than trusting the count, is what found
it.

### What is genuinely NOT fixed, stated so the type does not imply a guarantee it lacks

The generated struct **cannot enforce the `oneOf` constraints**. Every field is `std::optional`, so
a request with neither `afAppId` nor `trafficFilters`, or with both `ipv4Addr` and `anyUeInd`,
deserialises happily. That is a narrower gap than an opaque blob (which enforces nothing AND
validates no field types) and is the same class of runtime-validation gap this generator already has
for ordinary `required` handling -- but it is real. NEF's own handlers must validate those two
mutual-exclusivity groups explicitly against the spec's own lists; the DTO will not do it for them.

### A rename consequence, and a stale test result

Adding the YAML collided with the `TrafficInfluSub` already generated from
`TS29519_Application_Data`, so the generator suffixed **both**, and the previously-bare
`sbi_gen::TrafficInfluSub` that UDR's `influenceData` routes referenced stopped existing. Repointed
to `TrafficInfluSub_Application_Data`, which is what those routes always meant -- UDR stores
provisioned application data, not an AF-facing subscription. Same consequence class ADR-0208
recorded for a common-data group rename.

This will recur for the remaining 57 AF-facing files: `TS29522_*` and `TS29591_Nnef_*` describe
overlapping concepts from the AF and NF sides, so each newly wired file can rename types other NFs
reference. Compiler-caught, not silent -- but it means **every** such addition needs a full rebuild,
not a NEF-only one.

Worth recording because it nearly slipped through: the first `ctest` run after this change reported
`100% tests passed out of 533` **while UDR had failed to compile**, so it ran against a stale
binary. A suite result is only evidence when the build preceding it is verified clean. Re-run
against a confirmed-current build: 533/533, no regression across ~2,900 generated types.

