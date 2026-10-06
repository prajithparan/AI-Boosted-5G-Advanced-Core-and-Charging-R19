## ADR-0267: a real UE through RegistrationComplete to a real PDU session

### What was built

The last of the UE side of TS 24.501 that the N2 handover relay depends on, plus the test that
drives it. `tests/integration/ue_nas_driver.{hpp,cpp}` gains:

- **`build_registration_complete`** -- TS 24.501 §8.2.5, integrity protected and ciphered at
  security header type **0x02**, not SecurityModeComplete's 0x04: the context is no longer new by
  this message.
- **`build_pdu_session_establishment_request`** -- a real UlNasTransport (§8.2.10) whose payload
  container is a real 5GSM PDU Session Establishment Request (§8.3.1): EPD/pduSessionId/PTI/message
  type, the mandatory integrityProtectionMaximumDataRate, pduSessionType=IPv4 and sscMode=1, plus
  the transport-level IEs AMF routes on (PDU session ID, requestType, S-NSSAI, DNN in TS 23.003
  §9.1 label form).
- **`open_secured_downlink` / `extract_dl_nas_payload_container`** -- the UE finally reads AMF's
  downlink for real: MAC-verify, decipher, then pull the opaque N1 SM container out of the
  DlNasTransport.

`seal_uplink` factors the envelope every secured uplink message shares, so `build_security_mode_complete`
and the two new builders cannot drift apart on the MAC-covers-the-ciphered-bytes rule.

The new test, `RegisteredUeEstablishesARealPduSession`, drives one real UE over one real SCTP
association through:

```
(ADR-0265/0266 registration, now factored into register_ue)
RegistrationComplete            (uplink_count=1)  -> AMF -> PCF AM Policy Association
UlNasTransport(PDU Session Est) (uplink_count=2)  -> AMF -> SMF CreateSMContext -> PCF SM policy
                                <- N1N2MessageTransfer -> DlNasTransport (downlink_count=2)
                                   carrying SMF's PDU Session Establishment Accept
```

It spans the widest set of real processes in the suite -- NRF, UDR, UDM, AUSF, AMF, PCF, SMF --
over real NGAP/SCTP for N1/N2 and real TLS 1.3 + mTLS HTTP/2 for every SBI hop.

### Why this was the prerequisite, not a detour

AMF prepares a handover **per PDU session**, asking SMF for each session's real N2 transfer keyed
on an `smContextRef` it only holds if it really created an SM context (ADR-0249/0258). A session it
has no ref for is skipped, not filled with a fabricated tunnel, and a HandoverRequired whose
sessions are all skipped is answered with HandoverPreparationFailure. So a registered UE with no
PDU session can never reach the target gNB, however correct the rest of the chain is. That is what
this increment fixes.

### What the assertions actually prove

The uplink messages are MAC'd with the UE's own KNASint at the exact NAS COUNTs AMF's phase machine
expects. A wrong count or key is rejected in silence, not answered, so reaching the next step at
all is the check. AMF's own log confirms each step rather than the green test being taken on trust:

```
amf-ngap: RegistrationComplete verified OK for SUPI imsi-999700000000001
amf-ngap: AM Policy Association established with PCF -- UE registration procedure fully complete
amf-ngap: PDU Session Establishment Request verified OK, pduSessionId=5, dnn=internet
```

The downlink is the genuinely new kind of check. Until this increment every downlink assertion only
read the security-header byte AMF wrote, which a wrong key would not have changed. The UE now
MAC-verifies and deciphers RegistrationAccept (downlink_count=1) and the DlNasTransport
(downlink_count=2) with keys AMF never saw, and asserts on the deciphered content: message type
0x42 for the Accept, and for the PDU session, a 5GSM message whose PTI and PDU session ID are
echoed back and whose type is 0xC2. Those bytes were encoded by **SMF**, a separate process that
never touched this driver's key material, from PCF's real policy decision -- AMF only carried them.

### Disclosed: what this test does not cover

- **No UPF.** SMF logs `no UPF Sx Association established yet, skipping N4 Session Establishment`
  and still builds and delivers the Accept, which is the behaviour under test here. The session
  therefore carries no real N3 F-TEID, so this UE's session is not yet one AMF could hand over --
  the next increment adds UPF for exactly that reason.
- **No CHF**, so SMF logs a failed `Nchf_ConvergedCharging_Create`. Not in this test's scope.
- After the assertions pass, the test's own teardown kills AMF while SMF is still reading AMF's
  N1N2MessageTransfer response, so SMF logs `AMF N1N2MessageTransfer call failed`. That line is
  teardown noise **after** delivery, not a failure to deliver: the test could not have passed
  without the Accept arriving, being MAC-verified and decoding correctly.
- The UE still does not check SQN freshness (ADR-0265's disclosed deviation, unchanged).

### Two stale comments corrected

Same discipline as ADR-0250/0256/0257/0258/0265 -- a comment that justifies current behaviour with
an obsolete reason is worse than none:

- `decode_registration_complete` claimed it was "NOT CURRENTLY CALLED ... unreachable in practice"
  because `encode_registration_accept` sent no 5G-GUTI. **ADR-0075 added a real 5G-GUTI**, so a real
  UE owes a RegistrationComplete, and `handle_uplink_nas_transport_registration_complete` has been
  calling this decoder ever since.
- `decode_ul_nas_transport` claimed "no RegistrationComplete is ever sent ... callers pass 1". Its
  only caller passes **2**, and explains why in its own comment. The code was right; the header was
  stale.

### Bounded receives, added here rather than after the first hung job

`ngap_core::SctpSocket` gains `set_receive_timeout`, and `NgapTestGnb::connect` sets 30 s on its
association. Zero -- still the default -- means block indefinitely, which is what every production
caller relies on (each association gets its own thread, ADR-0030, and an idle gNB legitimately
sends nothing for hours), so no NF behaviour changes. On expiry `receive()` returns an empty
vector, the same "nothing came back" answer a closed association already gives, which the tests'
existing assertions already report properly.

The reason is this increment specifically. Every earlier assertion in this file waited on AMF
answering on its own NGAP thread; the Accept arrives only after AMF -> SMF -> PCF -> SMF -> AMF
completes, and AMF does not close the association when SMF fails. An unbounded wait there hangs
until the CI job is killed, which is indistinguishable from the free-runner deaths this project
already sees -- so the failure would be diagnosed as the wrong thing.

The expiry path was checked directly rather than assumed, against a peer that accepts an
association and then deliberately sends nothing: `receive()` returned empty after 815 ms on an
800 ms deadline, instead of blocking or throwing. (Throwaway program, not committed -- a
committed test for this would have to spend its timeout in CI to prove anything.)

### A latent race the bounded receive immediately caught

The first CI run after the timeout went in failed `RegistrationCompletesAndAmfInstallsASecurity
Context` -- the ADR-0266 test, unchanged in substance by this increment -- at exactly the 30 s
deadline. The log gives the whole story:

```
17:16:29.417  amf-ngap: InitialUEMessage from RAN-UE-NGAP-ID=1, SUPI=imsi-999700000000001
17:16:29.423  amf-ngap: AUSF call failed: Could not connect to server
17:16:29.461  ausf: starting
```

`NgapTestGnb::connect` waits only for AMF's NGAP listener, which is the **first** thing ready --
AMF's SCTP listener was up 44 ms in while AUSF took 440 ms to start. The UE then sent its
RegistrationRequest, AMF called an AUSF that was not listening yet, and **AMF does not retry that
call** (`kAusfBase` is addressed directly, no NRF discovery, no backoff) -- so it answered the UE
nothing at all. The UE then waited for a message that was never coming.

This is a pre-existing latent flake, not a regression: the same test passed on the ADR-0266 commit,
which simply won the race. A fast local machine wins it; a loaded CI runner does not.

The fix is `wait_for_sbi_peers`, which blocks until every NF a test's procedures traverse answers
on its own SBI port before the gNB sends anything -- UDR/UDM/AUSF for registration, plus PCF and
SMF for the PDU session. Any completed HTTP response counts as ready (a 404 from a real listener is
exactly the proof wanted); only a connection failure means "not up yet". It measurably does
something: the registration test went from 158 ms to 1236 ms locally, and that ~1 s is real waiting
on listeners the test previously raced.

Worth stating because it is the whole argument for the previous section: had the receive still been
unbounded, this would have hung until the runner killed the job, which is indistinguishable from
the free-tier runner deaths this project already sees. The bounded receive turned an
undiagnosable hang into a failure that named its own cause on the first read.

### Next, with three traps recorded rather than rediscovered

What remains for the full relay is a second `NgapTestGnb` as the target gNB, NGSetup'd before
`HandoverRequired` is sent, and UPF in the lab. Three things will bite, and all three produce the
*same* symptom -- `HandoverPreparationFailure` that reads like an AMF or SMF bug and is actually
test sequencing:

1. **`handle_handover_required` blocks the source association** for up to 10 s awaiting the
   target's reply (ADR-0258). A single-threaded test that sends `HandoverRequired` and then reads
   the source socket will deadlock; the target gNB's receive must be driven on its own thread.
   Recorded since ADR-0265, and in `.claude/skills/ngap-message-support.md`.
2. **Spawning UPF is not sufficient.** SMF establishes the N4 session *inline* during
   CreateSMContext and, when no Sx Association exists yet, logs `skipping N4 Session
   Establishment` -- it does not come back to the session later. SMF's discovery retries on a 2 s
   cadence, so the test must gate on the association being up **before the UE sends the PDU
   Session Establishment Request**. The gate already exists in
   `tests/integration/test_smf_handover_n2sminfo.cpp`: create a session on a throwaway SUPI over
   SBI and probe `HANDOVER_REQUIRED` until it answers 200 instead of 500.
3. **AMF's stores are in a shared, long-lived Redis**, not per-process: each test spawns a fresh
   AMF, but `ue_contexts`/`ue_security_contexts`/`amf_ue_id_index` survive in the one container
   across tests and across runs (visibly, in the TMSI counter). So the relay test must establish
   its PDU session **in its own body** rather than leaning on
   `RegisteredUeEstablishesARealPduSession` having run first -- a stale `smContextRef` from an
   earlier run, POSTed to a freshly spawned SMF, 404s and the session is skipped. Same failure
   class as the `udr_amf_context` row that has to be deleted before a local full `ctest`.

---

