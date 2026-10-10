## ADR-0480: AMF NAS algorithm selection core (TS 33.501 6.7.1.1), pure function, not yet wired

**Date:** 2026-10-10. **Status:** Proposed (core built and unit-tested; registration path NOT yet wired).

**Context.** The AMF hardcodes 128-NEA2/128-NIA2 in the Security Mode Command (`nas_codec.cpp`) and in key
derivation (`kNea2AlgorithmIdentity`/`kNia2AlgorithmIdentity`). TS 33.501 6.7.1.1 requires operator-configured,
priority-ordered lists (one for NAS ciphering, one for NAS integrity) from which the AMF selects the highest-priority
algorithm. The UE capability bitmaps are in TS 24.501 9.11.3.54 (octet 3 = 5G-EA0..EA7, octet 4 = 5G-IA0..IA7, identity
n <-> mask 0x80 >> n). A SNOW 3G provider (ADR-0479) may add NEA1/NIA1, so the AMF's implemented set is not fixed.

**Decision.** `nfs/amf/src/nas_algorithm_selection.{hpp,cpp}`: a pure `select_nas_algorithms(ue_capability, priority,
implemented_ciphering, implemented_integrity)` returning the first priority-list entry that is supported by the UE AND
implemented by the AMF, per direction, or nullopt if either direction has none (caller must reject; never silently
falls back to a weaker algorithm or to an algorithm outside the lists). Tests: `tests/conformance/test_amf_nas_algorithm_selection.cpp`
(6 cases, passing).

**Not done (disclosed).** Not called by any procedure yet: SMC still sends NEA2/NIA2. Remaining wiring: priority lists in
`config/amf.json`, SMC encoding and replayed UE capability with the chosen algorithms, K_NASint/K_NASenc derivation with
the chosen identity, NAS MAC/cipher dispatch per algorithm, reject cause on no common algorithm, handover (target AMF
selection), implemented-set derived from loaded providers. NEA1/NIA1 execution still needs the SAGE package (ADR-0479).

**Rejected alternatives.** (1) Pick by UE-side preference order: contradicts 6.7.1.1 (operator priority decides).
(2) Fall back to NEA0/NIA0 when nothing matches: unsafe, and NIA0 is not for normal use; absent from the lists means never chosen.
(3) Wire into SMC in the same change: larger blast radius touching crypto paths; separated so the policy core is reviewable alone.
