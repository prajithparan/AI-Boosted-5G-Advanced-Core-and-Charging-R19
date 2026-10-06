## ADR-0264: a real gNB for tests -- the NGAP handover chain gets its first end-to-end coverage

### The gap this closes

Every NGAP procedure this project has built -- the whole N2 handover chain across ADR-0090, 0095,
0096, 0248, 0249, 0258 and 0261 -- had been verified **by construction and review only**. ADR-0258
and ADR-0261 both had to disclose that in their own text, and ADR-0261 named itself "the
weakest-verified item in the ADR-0258..0261 sequence".

The reason was never laziness: UERANSIM (ADR-0016), the vendored RAN simulator, **implements no
handover procedure in its gNB at all**. Grep-confirmed rather than assumed -- `HandoverRequired`
appears in its generated ASN.1 (`src/asn/ngap/`) and never once in `src/gnb/`. It cannot drive
these paths, and modifying the vendored copy would change ADR-0016's license analysis.

`SctpSocket::connect()` has existed since ADR-0090, added explicitly for "a real hand-crafted NGAP
test client" -- which was never committed. This ADR commits it.

### What was built

`tests/integration/ngap_test_gnb.{hpp,cpp}` -- a real gNB for tests. A real SCTP association to
AMF's real N2 listener (`127.0.0.5:38412`, from `config/amf.json`), carrying real Aligned-PER NGAP
PDUs built with this project's own `ngap_core` codec and `ngap_generated` ASN.1 -- **the same codec
AMF itself uses**. Not a stub, not a recorded byte fixture.

Deliberate API choice: the header exposes **no ASN.1 type**. Tests deal in byte vectors plus a
small `summarize()` that answers "which outcome, which procedure code". The generated NGAP headers
and their lifetime rules stay inside one `.cpp` where they can be got right once, rather than being
re-learned in every test that wants to send a PDU.

Identifiers were read from source, not recalled: procedure codes and the mandatory IE sets for
`HandoverRequired` and `HandoverCancel` come from `specs/NGAP/ngap-17.9.asn`.

### First three assertions

| Test | What it proves |
|---|---|
| `NgSetupOverRealSctpSucceeds` | A real `NGSetupRequest` with a real `GlobalRANNodeID`/`globalGNB-ID` is answered with a real `NGSetupResponse` |
| `HandoverRequiredForUnknownUeIsRejectedWithPreparationFailure` | `handle_handover_required`'s real decode path runs, the cold `amf_ue_id_index` lookup misses, and AMF answers a spec-defined `HandoverPreparationFailure` -- rather than dropping the association or going silent |
| `HandoverCancelForUnknownUeIsStillAcknowledged` | **ADR-0261's AMF half, over the wire.** Its handler acknowledges a cancel whose UE it cannot resolve -- nothing to tell SMF or a target gNB, but the source gNB is still owed its answer. That was a written claim until now |

### Scope, stated rather than implied

These cover the paths reachable **without a registered UE**. The full relay
(`HandoverRequired` -> SMF -> `HandoverRequest` -> target gNB -> `HandoverCommand`) needs a real UE
security context in AMF's `amf_ue_id_index`/`ue_security_contexts`, which means driving NAS
registration from this driver -- authentication vectors, RES* computation, Security Mode Complete.
That is the next increment and is deliberately **not** faked here by seeding AMF's stores directly:
a test that installs its own security context would prove the handover code runs, not that it
interoperates.

The `handoverRequiredTransfer` and `SourceToTarget-TransparentContainer` this driver sends are
short opaque markers. AMF relays both verbatim without inspecting them, so that is honest for these
tests -- and the comment says so rather than letting the bytes imply a real RRC stack.

### One constraint worth recording

The test gNB's PLMN must match AMF's own `kMcc`/`kMnc` (999/70) exactly. AMF keys its
`GnbAssociationRegistry` on the **re-encoded `GlobalGNB-ID`**, so a gNB with a mismatched PLMN sets
up perfectly well and then can never be resolved as a handover target -- a silent failure, which is
why the driver duplicates AMF's own `encode_plmn_identity` rather than approximating it.

