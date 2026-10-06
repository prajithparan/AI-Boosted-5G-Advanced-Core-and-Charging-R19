## ADR-0268: asn1c's open-type presence index is wrong on unsuccessful-outcome NGAP PDUs

**Date:** 2026-09-04
**Status:** Accepted

### The defect

asn1c 0.9.29's generated open-type selector -- `select_UnsuccessfulOutcome_value_type` and its two
siblings in `build/generated/ngap_gen/{InitiatingMessage,SuccessfulOutcome,UnsuccessfulOutcome}.c`
-- ends with:

```c
result.presence_index = row + 1;
```

`row` is the row in the **all-procedures** Information Object Set table
(`asn_IOS_NGAP_ELEMENTARY_PROCEDURES_1`), not the index within the `value` CHOICE. The two differ
whenever a CHOICE omits procedures the table lists, because it only holds the ones that actually
have that outcome: 76 initiating, 29 successful, 15 unsuccessful, out of ~120 rows. So a decoded
`HandoverPreparationFailure`, whose correct presence is 5, came back as **8** --
`UnsuccessfulOutcome__value_PR_InitialContextSetupFailure`.

**How much of the set is affected, measured rather than assumed.** Comparing each `value` CHOICE's
member order against the IOS row its selector would return, for every procedure in the generated
set:

| outcome | members | mis-indexed |
|---|---|---|
| `InitiatingMessage` | 76 | **0** |
| `SuccessfulOutcome` | 29 | **0** |
| `UnsuccessfulOutcome` | 15 | **13** |

Every procedure has an initiating message, so that CHOICE tracks the table row for row and the two
indices coincide; successful outcomes happen to stay aligned too. Failures are sparse in the table,
so they do not: only `AMFConfigurationUpdateFailure` (1) and `BroadcastSessionModificationFailure`
(2) survive, and every member from `BroadcastSessionSetupFailure` on is shifted -- by 1 at first,
growing to 11 by `UEContextSuspendFailure` (26 for a correct 15). Three independent runtime decodes
confirm the analysis (`InitialUEMessage` 46 = 46, `NGSetupResponse` 15 = 15,
`HandoverPreparationFailure` 8 != 5).

So the practical blast radius is narrower than "every decoded PDU": it is every decoded
**unsuccessful outcome**, and of those 15, only four have a hand-edited
`ConcreteProtocolIE-Container` and can therefore carry real IEs at all --
`HandoverPreparationFailure`, `HandoverFailure`, `NGSetupFailure`, `PathSwitchRequestFailure`. All
four are in the mis-indexed set, and all four are messages AMF or the test gNB really decodes. The
other eleven still get a wrong index; they simply have nothing in them to leak. The repair is
written for the general case regardless: nothing in the generator promises the other two CHOICEs
stay aligned as R19 procedures are added.

The decoded data lands in the right place (the selector's `type_descriptor` is correct; only the
index is wrong), which is why nothing visibly misbehaved and every existing caller read the right
bytes. What breaks is the **free**: `ASN_STRUCT_FREE` walks the union as whatever type the bogus
index names.

That is only harmless if both shapes are the same, and in this project they are deliberately not.
Per ADR-0031 the ASN.1 is mixed: 23 messages were hand-edited to `ConcreteProtocolIE-Container`
(IE `value` is an inline `OCTET_STRING`), while the rest keep the real parameterized
`ProtocolIE-Container {{...}}` (IE `value` is a CHOICE). Freeing one as the other walks different
offsets -- the `OCTET_STRING`'s `buf` is never freed (the leak), and its pointer bytes are read as
a CHOICE presence tag. It does not crash only because that bogus tag exceeds `elements_count` and
`CHOICE_free` bails out. Benign by luck, not by design.

Not test-only: `nfs/amf/src/ngap_task.cpp` and `nfs/amf/src/ngap_handover.cpp` call `decode_pdu`
on every inbound NGAP PDU.

### Evidence, measured both ways

A standalone harness -- encode a `HandoverPreparationFailure` via `ngap::encode_pdu`, decode it,
`ASN_STRUCT_FREE` -- built `-fsanitize=address -DASN_DISABLE_OER_SUPPORT` against
`libngap_core.a` + `libngap_generated.a`, run against the pre-fix and post-fix codec:

```
before: encoded 25 bytes; decoded value.present = 8 (expected 5)
        AddressSanitizer: 9 byte(s) leaked in 3 allocation(s)   <- one per IE, in OCTET_STRING_decode_uper
after:  encoded 25 bytes; decoded value.present = 5 (expected 5)
        no leaks
```

### Decision

Repair the index inside `libs/ngap-core/src/ngap_codec.cpp`, on the success path of every decode
the file exposes. `repair_open_type_presence` re-runs the member's `type_selector` (whose
`type_descriptor` **is** correct), finds that descriptor among the open type's own members, and
writes that 1-based index, recursing through SEQUENCE / SET OF / SEQUENCE OF / CHOICE so nested
open types are covered too.

Three details worth recording, because each was a wrong turn avoided:

- **The index is written directly, not via `CHOICE_variant_set_presence`** -- that helper frees the
  currently-selected member first, which is precisely the wrong-type free being fixed. The write
  mirrors asn1c's own `_set_present_idx`, which is `static` in `constr_CHOICE.c` and so cannot be
  called.
- **The open-type member's descriptor is not a CHOICE.** `asn_DEF_value_4` has op
  `asn_OP_OPEN_TYPE`, not `asn_OP_CHOICE`. It is CHOICE-*shaped* -- `OPEN_TYPE.h` defines
  `OPEN_TYPE_free` as `CHOICE_free` and its `specifics` really is an `asn_CHOICE_specifics_t` --
  so both ops are read and written the same way, and nothing else is. A first draft guarded on
  `asn_OP_CHOICE` alone and silently disabled the entire repair; it was caught by the harness
  above, not by reasoning.
- **Members are matched by `asn_TYPE_descriptor_t*` identity**, which is only unambiguous if no
  CHOICE has two members of the same type. Checked, not assumed: a walk of the whole type graph
  reachable from `asn_DEF_NGAP_PDU` (142 descriptors, 7 CHOICE/OPEN_TYPE among them) reports **no**
  CHOICE with duplicate member types.

### Rejected alternatives

- **Make the ASN.1 uniform** so every member of a `value` CHOICE has the same layout. This masks
  the bug rather than fixing it -- the index would still be wrong, it would just stop mattering for
  the free -- and it would mean either hand-editing ~100 more messages to
  `ConcreteProtocolIE-Container` or reverting the 23. Rejected.
- **Patch asn1c's generated output during the build.** Fragile (a sed over generated C that must
  survive every regeneration), and it puts a local fork of the generator in the build. Rejected.
- **Upgrade or replace asn1c.** 0.9.29 is the last released version; the fix would have to come
  from a fork. Out of scope for this increment, and the repair above is independent of it -- if a
  future asn1c sets the index correctly, `repair_open_type_presence` writes the same value it
  already holds.

### Disclosed limitations

- **The failure path is deliberately not repaired.** On `rv.code != RC_OK` the structure is freed
  without the repair, because the selector picks a type from a sibling field (the IE `id` /
  `procedureCode`) that on a partial decode may hold garbage. Writing an in-range-but-wrong index
  there would convert today's benign bail-out into a genuine type-confused free. The leak this ADR
  fixes is on the success path only; a decode that fails may still leak, and that is not fixed
  here.
- **The parameterized messages' IE-level open type is empty**, a pre-existing ADR-0031 consequence
  rather than anything new: asn1c emits one shared `ProtocolIE-Field` whose `value` CHOICE has zero
  members, so `correct == 0` and the repair leaves it alone. That is why the 23 concrete messages
  exist at all.
- **This is a workaround for a generator defect, not a fix to the generator.** It runs on every
  decoded PDU -- a bounded walk (depth-capped at 16) over a structure that was just fully decoded,
  so it is cheap relative to the decode itself, but it is not free and has not been benchmarked.

### Test

`tests/integration/test_ngap_codec_presence.cpp`, three cases -- one per outcome
(`InitialUEMessage`, `NGSetupResponse`, `HandoverPreparationFailure`) -- each asserting the decoded
`value.present`, re-reading the IEs to show the repair does not disturb the payload, and freeing
the PDU. Per the table above, only the unsuccessful-outcome case fails without the repair; the
other two are guards that the repair leaves already-correct indices alone, and are labelled as such
in the test rather than left to imply a reproduction they do not perform. The free leaks only under
ASan, which is exactly how this stayed invisible.

### Why it went unseen until now

Only the test binary is ASan-instrumented -- NFs are spawned as separate processes -- and nothing
in that binary decoded an NGAP PDU until ADR-0264's `NgapTestGnb`. The `sanitize (asan-ubsan)` leg
also passed through ADR-0257 and was then cancelled on every run until run 33787616880 -- the run
that finally executed it failed on
`AmfNgapTestGnb.HandoverRequiredForUnknownUeIsRejectedWithPreparationFailure` with
`12 byte(s) leaked in 3 allocation(s)`, whose stack is this defect exactly
(`OCTET_STRING_decode_uper` <- `SET_OF_decode_uper` <- `OPEN_TYPE_uper_get` <- `ngap::decode_pdu`
<- `NgapTestGnb::summarize`). Byte count differs from the standalone harness's 9 only because the
IE payloads differ. Same lesson
as the crypto work: a self-consistent encode/decode round-trip proves nothing an independent
checker would not have caught immediately.

---

