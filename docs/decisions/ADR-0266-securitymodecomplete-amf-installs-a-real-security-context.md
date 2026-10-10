## ADR-0266: SecurityModeComplete -- AMF installs a real security context for a test-driven UE

### What was built

The UE side of the security mode procedure, completing the registration ADR-0265 got as far as
`SecurityModeCommand`:

- **`derive_nas_keys`** -- the full TS 33.501 Annex A chain, derived by the UE independently of the
  network: CK/IK -> KAUSF -> KSEAF -> KAMF -> KNASint/KNASenc. SQN xor AK is taken from AUTN's own
  first six octets rather than recomputed, so it is what the network actually sent. KAMF is keyed
  on the SUPI with its `imsi-` prefix stripped and the TS 33.501 Annex A.7.1 default ABBA, matching
  AMF's own derivation exactly -- a mismatch anywhere produces a MAC AMF rejects.
- **`build_security_mode_complete`** -- integrity protected *and* ciphered with the new security
  context (security header type 0x04). The MAC covers the bytes **as transmitted**, i.e. the
  ciphered inner prefixed by the sequence-number octet -- the convention `nas_codec.cpp` documents
  and UERANSIM implements. MACing the plaintext instead is the classic way to produce a message
  that is silently rejected.

### Why this is the strongest assertion in the driver so far

AMF verifies this message's MAC using keys it reached **independently, through AUSF** -- it never
sees the UE's CK/IK. So AMF accepting the MAC is proof that two separately-derived key chains
agreed on the same KAMF. No amount of correct framing produces that.

Confirmed by AMF's own log rather than inferred from a green test:

```
amf-ngap: SecurityModeComplete verified OK for SUPI imsi-999700000000001 -- NAS security context active
amf-ngap: sent DownlinkNASTransport with RegistrationAccept (26 bytes, tmsi=00000007), AMF-UE-NGAP-ID=1
```

On a wrong key AMF logs `SecurityModeComplete MAC verification FAILED` and answers nothing, so the
test would fail on silence rather than pass on a technicality.

The test asserts on the `RegistrationAccept` being security header type 0x02 (integrity protected
and ciphered), which is only reachable once AMF has installed the context.

### What this completes

`ue_security_contexts` and `amf_ue_id_index` now hold a real entry for a UE this test suite drove
itself. That was the last prerequisite for the handover relay: `handle_handover_required` resolves
its UE through exactly those two stores.

### Next, with the trap already known

The remaining piece is a second `NgapTestGnb` as the target gNB, NGSetup'd before
`HandoverRequired` is sent. `handle_handover_required` **blocks the source association** for up to
10 s awaiting the target's reply (ADR-0258), so the target gNB's receive must be driven on its own
thread -- a single-threaded test that sends `HandoverRequired` and then reads the source socket
will deadlock. Recorded in ADR-0265 and in `.claude/skills/ngap-message-support.md`.

