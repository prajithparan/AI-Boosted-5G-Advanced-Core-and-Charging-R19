## ADR-0265: a real UE through authentication -- NAS on top of ADR-0264's test gNB

### What was built

`tests/integration/ue_nas_driver.{hpp,cpp}` -- enough of the UE side of TS 24.501 to carry a real
subscriber through authentication against this project's real AMF -> AUSF -> UDM -> UDR chain, plus
the NGAP UE-associated signalling ADR-0264's test gNB needed to carry it (`InitialUEMessage`,
`UplinkNASTransport`, and `DownlinkNASTransport` extraction).

The new test drives, over one real SCTP association:

```
RegistrationRequest (SUCI, null scheme)  -> AMF
                                         <- DownlinkNASTransport: AuthenticationRequest (RAND, AUTN)
verify AUTN's MAC, compute RES*
AuthenticationResponse (RES*)            -> AMF
                                         <- DownlinkNASTransport: SecurityModeCommand
```

### Why this is not testing the code against itself

The encoders here produce bytes AMF's own decoders accept, so the *framing* is mirror-derived. That
would be circular if those decoders had never met an independent implementation -- but they have:
registration was originally built and verified against UERANSIM's own UE (ADR-0031/0032/0037), a
separate codebase by a different author. Re-encoding to a format an independent implementation
already agreed on is not self-agreement.

The genuinely independent check is the **cryptography**, and it is what the test asserts on. RES* is
computed here from the subscriber's real TS 35.207 K/OPc and sent to AUSF, which independently
derives its own expected value from what UDM/UDR hold. Well-formed NAS framing does not get past
that -- only correct crypto does. So the assertion is *"AMF proceeded to SecurityModeCommand"*,
which happens only after AUSF confirms RES*, and never on this driver round-tripping its own output.
It passed on the first run: AMF logged `sent DownlinkNASTransport with SecurityModeCommand`.

The UE also verifies AUTN's MAC-A before answering, so a wrong vector fails loudly here rather than
being answered anyway.

### One deliberate deviation from real UE behaviour, disclosed

**This UE does not check SQN freshness.** A real UE rejects an out-of-range SQN with
`AuthenticationFailure(SYNCH_FAILURE)`; this project's seeded subscriber uses a fixed TS 35.207 SQN
that always trips exactly that on first contact, and AMF implements the resynchronisation which
follows (ADR-0037). That path deserves its own test. Making *this* test depend on resync working
would couple a handover-enablement fixture to an unrelated procedure, so the test UE accepts any
SQN and says so in the source, rather than quietly relying on resync.

### Stale comment corrected

`nfs/amf/src/nas_codec.hpp` still claimed "Resynchronization ... is NOT implemented -- disclosed
gap, see ADR-0032". ADR-0037 implemented it (`ngap_task.cpp`'s `sqn_resync_attempted` and the
`resync_info` it passes to AUSF). Corrected here rather than left to justify current behaviour with
an obsolete reason -- same discipline as ADR-0250/0256/0257/0258.

### What this unlocks, and what is still missing

A registered UE means AMF now has the `amf_ue_id_index` -> `ue_security_contexts` entries the
handover relay resolves against. The remaining step before the full relay can be driven is
`SecurityModeComplete` -- an integrity-protected uplink message, so it needs
`aka_crypto::nas_security` and the NAS key derivation, which this increment deliberately stops
short of. After that, a second `NgapTestGnb` as the target gNB completes the picture.

One trap already identified for that next increment, recorded now rather than rediscovered:
`handle_handover_required` **blocks the source association** awaiting the target gNB's reply
(10 s, ADR-0258). A single-threaded test that sends `HandoverRequired` and then reads from the
source socket will deadlock -- the target gNB's receive must be driven concurrently.

