---
name: ngap-message-support
description: Add support for a new NGAP (N2/TS 38.413) message type in this 5GC repo - the ASN.1 container patch asn1c requires, where to read procedure codes and IE ids, and how to build/decode PDUs. Use when implementing or testing any NGAP procedure AMF does not already handle.
---

# Adding a new NGAP message type

## The trap: asn1c cannot resolve parameterized IE containers

`specs/NGAP/ngap-17.9.asn` is **deliberately patched** (ADR-0031): the message types this build
handles have their `protocolIEs ProtocolIE-Container {{XxxIEs}}` replaced by a concrete
`ConcreteProtocolIE-Container`, because asn1c 0.9.29 cannot resolve the parameterization.

A message that has *not* been patched generates as `ProtocolIE_Container_NNNNNPk_t`, and
`ngap::find_ie`/`add_ie` — which take `ConcreteProtocolIE_Container_t` — will not compile against
it. The error looks like:

```
invalid initialization of reference of type 'const ConcreteProtocolIE_Container_t&'
  from expression of type 'const ProtocolIE_Container_11963P0'
```

Fix by extending the existing patch, keeping the comment convention:

```
HandoverCancel ::= SEQUENCE {
	protocolIEs		ConcreteProtocolIE-Container,	-- see ConcreteProtocolIE-Container's own comment; real IEs per HandoverCancelIEs below, ADR-0031/ADR-XXXX
	...
}
```

This changes **no wire encoding** — per X.691 clause 10.9 an open type's PER encoding is exactly
the octet-string-wrapped blob these helpers already produce. Say so in the ADR.

Editing the `.asn` regenerates all of `ngap_gen`, so it forces a full rebuild. Batch ASN.1 changes.

## Read identifiers, never recall them

Everything is in `specs/NGAP/ngap-17.9.asn`:

- Procedure code: `grep -n "id-HandoverCancel.*ProcedureCode ::=" specs/NGAP/ngap-17.9.asn`
- IE ids and PRESENCE: `awk '/XxxIEs NGAP-PROTOCOL-IES/,/^}/' specs/NGAP/ngap-17.9.asn`
- Struct shape (which fields are OPTIONAL — this differs from what the name suggests):
  `sed -n '/^typedef struct/,/^} /p' build/generated/ngap_gen/<Type>.h`

Quote the file and line in the ADR. Fabricating a TS number or IE id is the project's single worst
failure mode.

## Building and decoding PDUs

`libs/ngap-core`'s `ngap_codec.hpp` is shared by AMF, SMF and tests:

- `make_ie(id, criticality, &asn_DEF_T, &value)` + `add_ie(container, ie)` to compose
- `find_ie(container, id)` + `decode_ie_value(&asn_DEF_T, ie)` to read (caller frees)
- `encode_pdu` / `decode_pdu` for whole PDUs
- `encode_value` / `decode_value` for a bare type embedded as an
  `OCTET STRING (CONTAINING T)` transparent container (the N2 SM info transfers)

Memory rules that matter: values added via `make_ie` are copied in, so free your local with
`ASN_STRUCT_FREE_CONTENTS_ONLY` after `add_ie`. Anything you `calloc` into a struct field is owned
by that struct. A `Cause` CHOICE can be value-copied for its five enumerated arms but **not** for
`choice_Extensions`, which is a pointer — copying it double-frees.

## Testing over real SCTP

`tests/integration/ngap_test_gnb.{hpp,cpp}` is a real gNB for tests: real SCTP association to
AMF's N2 listener (`127.0.0.5:38412`, `config/amf.json`), real PER PDUs. Use it rather than
UERANSIM for anything handover-related — **UERANSIM's gNB implements no handover procedure at
all** (`HandoverRequired` appears in its generated ASN.1, never in `src/gnb/`), which is why this
driver exists.

The test gNB's PLMN must match AMF's `kMcc`/`kMnc` (999/70) exactly: AMF keys its
`GnbAssociationRegistry` on the re-encoded `GlobalGNB-ID`, so a mismatched PLMN yields a gNB that
sets up fine but can never be found as a handover target.

Procedures needing a registered UE (the handover relay proper) need a real security context in
AMF's `amf_ue_id_index`/`ue_security_contexts`; without one AMF answers
`HandoverPreparationFailure`. Failure paths and `HandoverCancel`'s acknowledge path are reachable
without registration.

`tests/integration/ue_nas_driver.{hpp,cpp}` (ADR-0265) is the UE side: RegistrationRequest with a
null-scheme SUCI, AUTN verification, RES* from the seeded TS 35.207 credentials, and
AuthenticationResponse. It gets a UE as far as `SecurityModeCommand`. Two things to know:

- **Assert on the network accepting the exchange, not on your own encoder round-tripping.** The NAS
  framing is mirror-derived from AMF's decoders, so it proves nothing by itself; the real check is
  that AUSF accepts RES*, which it computes independently from UDR's credentials.
- **The test UE deliberately does not check SQN freshness.** The seeded subscriber's fixed SQN
  would otherwise trip `SYNCH_FAILURE` on first contact every time and drag ADR-0037's resync into
  every unrelated test.

**Deadlock warning for the full relay:** `handle_handover_required` blocks the source association
while awaiting the target gNB's reply (10 s timeout). A single-threaded test that sends
`HandoverRequired` then reads the source socket will hang -- drive the target gNB's `receive_raw()`
from another thread.
