## ADR-0378: The AMF IRI-POI (LI increment 4) -- six AMF xIRI codecs, the in-AMF POI, and why the other five event hooks are blocked

**Date:** 2026-09-19. **Status:** accepted (partial -- Registration wired; other events blocked,
see below). Continues ADR-0364's LI programme after ADR-0377 (MDF2). This is the first POI: the
AMF gains the ability to detect a target UE's events and stream xIRIs to the MDF2.

**Event list approved first (standing rule).** TS 33.127 clause 6.2.2.4 lists 13 AMF xIRI events;
the user approved the "core mobility + identity" subset of six -- Registration, Deregistration,
LocationUpdate, StartOfInterceptionWithRegisteredUE, IdentifierAssociation,
IdentifierDeassociation -- before any code.

**Decision 1: the six xIRI codecs in `li_core::xiri`** (commits b022c38, f8d75fa, 170ece5,
394f8f2). Each maps its TS 33.128 clause-6.2.2.2.x record's mandatory members to a C++ struct and
BER-encodes it through the asn1c TS33128Payloads codec, in the increment-1 `AmfRegistration`
pattern: model the non-OPTIONAL / M members, add C/O members when a producer needs them. Shared
helpers (`fill_supi`/`fill_guti`, and the `Location` codec -- PLMNID/TAI/NCGI/ECGI plus a BIT
STRING fixed-width helper for the 36/28-bit cell identities) are factored once and reused across
events. `Location` is modelled to its NGAP-user-location floor (NR/E-UTRA cell), other branches
deferred. All six round-trip and are ASan-clean (8 Xiri conformance tests).

**Decision 2: the POI is an in-AMF module, not a separate process** (`nfs/amf/src/li_poi.{hpp,cpp}`,
commit 055a53a). It runs its own LI_X1 provisioning listener (the `li_core::x1` NE server on its
own port + mTLS -- off the service-based plane per TS 33.127, so no NRF registration; that DoD
step is N/A here, a spec fact), a process-lifetime target-identifier store the ADMF provisions
over X1, and a `li_core::X2X3Client` to the MDF2. Config-driven, disabled by default; when off the
AMF constructs no POI and every hook is a no-op. Delivery is best-effort -- a dead MDF2 is logged,
never allowed to break a UE procedure.

*This required proving the tightest constraint in the increment:* the AMF is the first binary to
host BOTH the NGAP asn1c codec and `li_core`'s TS33128Payloads codec, which share 83 type names
with different shapes. The containment (li_core is a shared object with hidden visibility,
`li_generated` linked PRIVATE inside it -- ADR-0364) holds: the `amf` target builds and links
against `li_core` with no undefined references, exactly the shape `test_li_ngap_coexistence.cpp`
exists to prove. `amf.Dockerfile` ships `libli_core.so` + `specs/etsi` (li_core's compile-time
`LI_ETSI_SCHEMA_DIR` for X1 validation).

**Decision 3: the POI is reached through a file-scope pointer, not a threaded parameter.** The
Registration hook lives five calls deep (`run_ngap_lifecycle` -> `run_association_thread` ->
`handle_association` -> `handle_uplink_nas_transport` -> `handle_uplink_nas_transport_smc_complete`).
Rather than thread a `LiPoi*` through five signatures, it is a file-scope pointer set once at
`run_ngap_lifecycle` entry -- matching `ngap_task.cpp`'s existing `g_next_amf_ue_ngap_id`
singleton, since the POI is process-lifetime state exactly like that allocator.

**Decision 4: only Registration is wired; the other five hooks are blocked, not deferred-as-easy.**
The functional test (commit 51be29d, `tests/integration/test_li_amf_poi.cpp`) proves the
Registration path end to end. The remaining five events were investigated against the real AMF and
each is blocked on a genuine prerequisite; wiring them now would fabricate a trigger or emit
non-conformantly, so none was wired. The blockers regroup into three prerequisites, not five:

1. **NGAP `UserLocationInformation` -> `xiri::Location` parse (highest leverage).** The AMF only
   presence-checks `UserLocationInformation` (`ngap_task.cpp` line 913, per the line-877 comment),
   never parsing the NR/E-UTRA cell. This one parse unblocks LocationUpdate *and* the mandatory
   `location` member of IdentifierAssociation. The `Location` codec is already built.
2. **X1 `IdentifierAssocationExtensions` / `EventsGenerated` gating parse in `li_core::x1`.** TS
   33.128 clause 6.2.2.2.1 generates IdentifierAssociation/Deassociation *only* when this
   per-target X1 parameter enabled them (and when enabled, the POI also emits AMFLocationUpdate).
   `li_core::x1` does not parse it; emitting unconditionally would over-report. Unblocks both
   identifier events.
3. **An AMF UE-deregistration procedure.** The AMF implements none (no NAS/NGAP deregistration
   handler), so Deregistration has no trigger. This is real 5GMM protocol work, larger than an
   LI hook, arguably not LI work at all.

Plus **StartOfInterceptionWithRegisteredUE**, which fires when a warrant is activated on an
already-registered UE and so needs the POI to see the AMF's registration state at X1-activation
time -- coupling that does not exist today.

**Disclosed.** Registration reports `registrationType=Initial` / `result=3GPP-access` (the lab
case) and fires at RegistrationAccept; a stricter POI would carry the UE's real registration type
(from the RegistrationRequest) and fire on RegistrationComplete. The X1 Destination operations are
accepted but not used for routing (a single configured MDF2 is the destination).

**Rejected.** *A separate POI process* -- the coexistence holds, so an in-AMF module is simpler and
avoids a local IPC hop. *Threading `LiPoi*` through five handler signatures* -- more edit surface
and churn than the file-scope singleton the file already uses for equivalent state. *Wiring the
five blocked events with placeholder triggers* -- would fabricate detection points or over-report,
the project's worst failure mode.

