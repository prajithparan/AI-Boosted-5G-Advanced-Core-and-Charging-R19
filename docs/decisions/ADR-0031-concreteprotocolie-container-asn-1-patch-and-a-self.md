## ADR-0031: ConcreteProtocolIE-Container ASN.1 patch, and a self-authored Aligned PER (X.691) patch for asn1c 0.9.29

**Date:** 2026-08-07
**Status:** Accepted

**Context:** ADR-0030 got `libs/ngap-generated` compiling, but Stage 1's actual goal --
AMF successfully completing NG Setup against the real `nr-gnb` binary -- required solving two
further, real problems discovered only by attempting the real build and the real interop test, not
foreseen at ADR-0030's time.

**Problem 1: asn1c 0.9.29 cannot resolve NGAP's parameterized `ProtocolIE-Container {{IEsSetParam}}`
Information Object Class syntax into a usable type.** Compiling `specs/NGAP/ngap-17.9.asn` unpatched
produced a `NGSetupRequest_t.protocolIEs` field typed as an empty `ATF_OPEN_TYPE` CHOICE with no
concrete member bound to it -- verified by inspecting the generated `.c` (`asn_MBR_ProtocolIE_Field_*`
showing `ATF_OPEN_TYPE | ATF_NOFLAGS`, `asn_OP_OPEN_TYPE`, no concrete type binding), not assumed
from a compiler warning. Confirmed via UERANSIM's own GitHub Discussions that other users hit the
same asn1c limitation on this exact file. **Fix:** `specs/NGAP/ngap-17.9.asn` is patched (see its own
header comment) to add a concrete, non-parameterized `ConcreteProtocolIE-Field`/
`ConcreteProtocolIE-Container` pair, and the `protocolIEs` field of the 6 messages this build
currently handles (`NGSetupRequest`, `NGSetupResponse`, `NGSetupFailure`, `InitialUEMessage`,
`DownlinkNASTransport`, `UplinkNASTransport`) is repointed to it instead of the real spec's
parameterized `ProtocolIE-Container {{XxxIEs}}`. This is justified, not just expedient: X.691 clause
10.9 defines an ASN.1 open type as PER-encoded identically to an octet-string-wrapped blob --
`ConcreteProtocolIE-Field`'s `value OCTET STRING` field is exactly that wrapper, manually filled in
by `nfs/amf/src/ngap_codec.cpp`'s `make_ie`/`decode_ie_value` (which PER-encode/decode the IE's real
typed value against its own type descriptor, e.g. `&asn_DEF_AMFName`, and store/load the result as
the wrapper's raw bytes) -- functionally equivalent to what the real parameterized IE-set machinery
would produce, without asn1c needing to resolve the full Information Object Class table. Each
repointed field cites this ADR and the real IE-list name inline. Other NGAP messages beyond these 6
are unaffected (still use the real, unresolvable parameterized form, but are not yet compiled by
`-pdu=all`'s auto-discovery mattering here since they're simply not referenced/used); extending
this pattern to further messages in later stages is expected to be mechanical.

**Problem 2: neither vanilla asn1c 0.9.29 nor an available fork provides usable Aligned PER.**
TS 38.413 mandates X.691 *Aligned* PER; vanilla asn1c 0.9.29 only ever implemented *Unaligned*
PER (`uper_encode`/`uper_decode` family) -- confirmed via `grep` finding zero `aper_*`/
`ATS_ALIGNED_CANONICAL_PER` symbols anywhere in its built output, and empirically: a real
`nr-gnb`-sent `NGSetupRequest` connected over real SCTP but AMF logged "failed to decode NGAP PDU"
using the Unaligned decoder. Two escape routes were evaluated and rejected before landing on a
third:
- *Copy UERANSIM's own vendored, already-Aligned-PER-capable asn1c runtime* (they ship one at
  `simulators/ransim/vendor/UERANSIM/src/asn/asn1c/`). Rejected: UERANSIM is AGPL-3.0-licensed;
  copying its code into this project's own compiled artifacts (this build's own `amf` binary) would
  contaminate Apache-2.0-licensed code with AGPL obligations. ADR-0016 already established an
  arms-length relationship with UERANSIM for exactly this reason (external test peer only, never a
  source dependency of this project's own binaries) -- reusing their code here would break that
  boundary. UERANSIM's binaries and source were, however, legitimately used throughout this stage
  as a **test oracle** (running the real `nr-gnb` binary as an unmodified external process, and
  separately compiling small standalone decode-test harnesses directly against their vendored
  sources to get detailed X.691-conformant debug traces) -- reading/running vendored code to check
  this project's own independently-authored code against a reference implementation is materially
  different from copying that code into a shipped binary, and was essential to finding every rule
  documented below.
- *`osmocom/asn1c`'s `aper-prefix` branch* (a real, published fork with genuine Aligned PER support).
  Attempted, then abandoned after two real, independent failures: (1) its **compiler** cannot parse
  the actual NGAP-17.9 ASN.1 module -- fails with a hard grammar error on the very first
  `NGAP-PROTOCOL-IES ::= { ... }` Information Object Set definition it encounters (line ~1411 of
  the vendored module), a construct that appears hundreds of times throughout the file and that
  vanilla 0.9.29's newer parser handles without issue; this fork's grammar predates whatever parser
  fix let 0.9.29 handle it. (2) Even setting compilation aside, its **runtime skeleton** turned out
  to be ABI-incompatible with vanilla 0.9.29's own generated code: 0.9.29 uses a newer, shared
  `asn_bit_data_t`/`asn_bit_outp_t` architecture (`asn_bit_data.h`) with macros like
  `ASN__DECODE_FAILED`, while the osmocom fork's lineage predates that refactor and uses older
  `asn_per_data_t`-direct types and `_ASN_DECODE_FAILED`-style macros -- confirmed by a real link/
  compile failure (`error: conflicting types for 'ENUMERATED_decode_uper'`, `'_ASN_DECODE_FAILED'
  undeclared`) when vanilla-generated per-type code was compiled against this fork's skeleton files.
  Mixing "vanilla compiler output + fork skeleton" was the first thing tried (since the fork's
  compiler couldn't even parse the file) and failed for exactly this reason.
- **Chosen approach: patch vanilla asn1c 0.9.29's own skeleton sources in place**, adding Aligned
  PER support self-authored from the X.691 standard's rules (not copied from either rejected
  source above), keeping the same struct layouts and macros vanilla 0.9.29 already uses everywhere
  else in this build. This means asn1c's *compiler* binary needed zero changes -- the per-type C
  descriptor tables it already generates correctly (including for Problem 1's concrete-container
  patch) work unmodified; only the shared, ASN.1-module-independent runtime primitives needed new
  code.

**What the patch adds, and the X.691 rules behind each piece** (full unified diff at
`scripts/patches/asn1c-aligned-per.patch`, applied by `scripts/setup-asn1c.sh` after extracting
the official `asn1c-0.9.29` release tarball -- `build-tools/` itself stays gitignored, matching
every other local build tool in this project, but the *patch* is committed since it is this
project's own original work product, not a rebuildable-on-demand artifact):

1. `asn_bit_data.{h,c}`: adds an `int aligned` field to both `asn_bit_data_t` (decode) and
   `asn_bit_outp_t` (encode), plus `asn_get_align`/`asn_put_align` primitives that consume/emit
   padding bits up to the next octet boundary (a no-op when already aligned).
2. `per_encoder.{h,c}` / `per_decoder.{h,c}`: adds `aper_encode`/`aper_encode_to_buffer`/
   `aper_encode_to_new_buffer` and `aper_decode`/`aper_decode_complete` as new top-level entry
   points, each setting the new `aligned` field before dispatching through the *same*
   `td->op->uper_encoder`/`uper_decoder` function pointer `uper_encode`/`uper_decode` already use.
   This is the crux of why a skeleton patch (not a compiler patch) suffices: asn1c generates only
   one PER codec function per type regardless of alignment -- alignment is a runtime property of
   *how the bits are packed*, decided by the low-level primitives below via this flag, not by which
   generated function is called. (The function-pointer struct field is still named
   `uper_decoder`/`uper_encoder`, a naming artifact predating this project's own aligned-PER
   addition -- misleading but left as-is throughout to minimize the diff against upstream.)
3. `per_support.h` adds four `static inline` helpers, and `per_support.c`/`OCTET_STRING.c`/
   `BIT_STRING.c`/`constr_SEQUENCE_OF.c`/`constr_SET_OF.c` call them at each point X.691 requires
   alignment. **Three distinct, real X.691 rules were needed here, each found the hard way** --
   this project's first attempt applied one rule everywhere and broke every message it touched;
   each correction below was pinned down by decoding real bytes (a real `nr-gnb`-sent
   `NGSetupRequest`, and this build's own `NGSetupResponse`) against **both** this project's own
   decoder **and** a standalone harness compiled directly against UERANSIM's own vendored asn1c
   sources as an independent reference oracle (see the licensing note above for why that's a
   legitimate, arms-length use):
   - **VALUE fields** (`aper_align_value_get_nbits`/`put_nbits`, used only by
     `uper_get_constrained_whole_number`/`uper_put_constrained_whole_number_u`, i.e. plain
     INTEGER-typed fields like NGAP's `ProcedureCode`): ALIGNED PER octet-aligns **even when the
     range fits in 8 bits or fewer** -- there is no small-range exception. Found because
     `InitiatingMessage.procedureCode` (`INTEGER(0..255)`, exactly 8 bits) decoded as `0` instead
     of the real `21` (id-NGSetup) until alignment was added here; the real value landed exactly on
     a clean octet boundary in the captured bytes.
   - **LENGTH/COUNT determinants** (`aper_align_length_get_nbits`/`put_nbits`, used by
     `uper_get_length`'s small-range fast path and by `OCTET_STRING.c`/`BIT_STRING.c`/
     `constr_SEQUENCE_OF.c`/`constr_SET_OF.c` for a SIZE-constrained string's character count or a
     SEQUENCE-OF/SET-OF's element count): the *opposite* rule -- **no** alignment when the range
     fits in 8 bits or fewer; alignment (and widening to exactly 16 bits) only kicks in for 9-16
     bits. Found because `AMFName`'s implicit length (`PrintableString (SIZE(1..150,...))`,
     `effective_bits=8`) decoded as length `1` instead of the real `11` when the VALUE-field rule
     was (wrongly) applied here too -- cross-checked byte-for-byte against UERANSIM's own reference
     decoder's debug trace, which reads the same 8-bit length with zero padding immediately after
     the preceding 1-bit size-extension flag.
   - **CHOICE presence index and ENUMERATED root-value index** (`constr_CHOICE.c`,
     `NativeEnumerated.c`): neither rule applies -- these two have their own dedicated X.691
     procedures and are **never** octet-aligned, identical to Unaligned PER, regardless of range.
     No helper call at all; deliberately reverted back to plain `per_get_few_bits`/`per_put_few_bits`
     after an earlier attempt wrongly aligned these too. Confirmed via the same real capture: the
     1-bit extension-presence flag and 2-bit `NGAP-PDU` CHOICE index that precede `procedureCode`
     are packed with zero padding between them.
   - `per_opentype.c`'s open-type wrapper (used for NGAP's real, non-patched open types, and
     internally structurally identical to what this project's own `ConcreteProtocolIE-Field.value`
     OCTET STRING does by hand) needed two separate fixes: its decode-side `memset(&spd, 0, ...)`
     was silently zeroing the new `aligned` field for the nested sub-decode (forcing every open
     type's *contents* into Unaligned PER regardless of the outer call -- since this project's own
     concrete-container IEs are wrapped exactly this way, this single bug affected every NGAP
     message field this project handles, not just real open types), fixed by propagating
     `spd.aligned = pd->aligned`. Its encode-side `uper_open_type_put` was hardcoded to call
     `uper_encode_to_new_buffer` unconditionally; fixed to call `aper_encode_to_new_buffer` when
     `po->aligned`.
   - `nbits > 16` is not exercised by this project's current NGAP message set for the VALUE or
     LENGTH rule; both helpers apply a best-effort octet-round-up rather than silently leaving it
     unaligned, but that path is explicitly unverified against a real peer -- flagged in the code
     comment, not silently assumed correct, per this project's disclosure rule.
4. `OCTET_STRING.c`/`BIT_STRING.c` also needed two further, narrower fixes specific to character
   string / fixed-size string encoding (X.691 clause 16 and 27):
   - **Per-character bit width rounds up to a canonical size** (`aper_char_unit_bits`, new helper):
     Aligned PER packs PER-visible-alphabet-constrained characters using the smallest of
     `{1, 2, 4, 8, 16, 32}` bits that fits the alphabet, not the exact `ceil(log2(alphabet size))`
     bits Unaligned PER uses. Found because `AMFName`'s 91-symbol `PrintableString` alphabet
     (range 32..122) needs only 7 bits exactly, but the real encoding uses 8 -- cross-checked again
     against UERANSIM's own reference decoder's debug trace (`"(32..122):8"`, and its own
     generated constraint table literally records `range_bits=7` for the *unaligned* interpretation
     while the runtime still packs 8 in aligned mode).
   - **Character content itself starts octet-aligned**, separately from its own length
     determinant's alignment: a length with `effective_bits<=8` (the common case, per the LENGTH
     rule above) is read with *no* preceding padding, so the bit position immediately after it is
     not itself byte-aligned -- an explicit align call was added right before the actual character
     data read/write in both the general (unconstrained-length) path and `OCTET_STRING_encode_uper`'s
     separate small-`effective_bits` fast-path branch (decode already funnels both cases through one
     shared loop, so only encode needed the second call site). Fixed fixed-size (`SIZE` non-
     extensible, `effective_bits==0`) octet/bit strings wider than 2 octets (16 bits) similarly, per
     X.691 #16.6 (`<=2` octets, no alignment) vs #16.7 (`>2` octets, aligned) -- this specific
     sub-case remains unverified against a real peer since no field in this build's current message
     set exercises a fixed-size character string, only fixed-size plain octet strings (e.g.
     `PLMNIdentity`, 3 octets) and small bit strings (`AMFRegionID`/`AMFSetID`/`AMFPointer`, all
     <=10 bits, under the 16-bit BIT STRING threshold where this rule doesn't change behavior
     anyway) -- flagged, not assumed.

**Distribution mechanics:** `scripts/patches/asn1c-aligned-per.patch` is a plain unified diff
(`diff -ru`, `skeletons/<file>` paths, applies cleanly with `patch -p1` from an extracted
`asn1c-0.9.29` tarball root -- verified against a fresh extraction, not just the already-patched
tree) and is committed (unlike everything under `build-tools/`, which is gitignored by the existing
`build-*/` glob -- the patch lives under `scripts/` specifically so it isn't swept up in that
pattern). Everything under `build-tools/` itself (the vanilla tarball, the patched build, the
earlier abandoned `osmocom/asn1c` and GNU-autotools-toolchain build attempts from investigating
Problem 2) is reproduced by `scripts/setup-asn1c.sh`, matching `scripts/gen-lab-pki.sh`'s existing
precedent for a setup script that produces gitignored local state. `libs/ngap-generated/
CMakeLists.txt`'s `find_program` failure message points here.

**Verification:** the full staged Stage 1 goal -- AMF (freshly rebuilt from a *clean*
`scripts/setup-asn1c.sh` run, not the already-patched tree left over from debugging) completing NG
Setup against the real, unmodified `nr-gnb` binary -- succeeds end-to-end: `nr-gnb` logs "NG Setup
Response received" / "NG Setup procedure is successful", AMF logs receiving and correctly
dispatching the real `NGSetupRequest` and sending a real `NGSetupResponse`. All 58 pre-existing
tests still pass unchanged. No `gtest`/`ctest`-integrated regression test exists yet for the ASN.1
PER codec itself (the verification above is the real-binary interop test the project's own
methodology treats as authoritative for this kind of protocol work, per ADR-0030's precedent) --
a dedicated automated round-trip test is deferred to Stage 6's documentation/verification pass
alongside `docs/TRACEABILITY.md`.

**Rejected alternative:** hand-roll a partial PER parser scoped to only this build's exact IE set,
skipping asn1c entirely. Rejected for the same reason the original plan gave: this project
explicitly committed to "a real ASN.1 PER codec generated from the actual 3GPP NGAP module, not
hand-rolled partial parsing" -- a scoped hand-rolled codec would silently stop being spec-traceable
the moment a new IE or message is added in a later stage, exactly the failure mode CLAUDE.md's
anti-fabrication rules exist to prevent.

**Consequence:** Stage 1 (NG Setup) is now complete and empirically verified. Stages 2-5 (Initial
UE Message/NAS Registration Request, Authentication, Security Mode, Registration Accept/Complete
ending in the real PCF call) can now build on a genuinely working Aligned PER codec rather than
inheriting this stage's Unaligned-only limitation -- though each stage may still surface further,
not-yet-exercised gaps in the patch (per the explicit `nbits > 16` and fixed-size-character-string
disclosures above), to be found and fixed the same way: real bytes, real peer, real oracle, not
assumed. **This is exactly what happened in Stage 2 -- see ADR-0032.**

