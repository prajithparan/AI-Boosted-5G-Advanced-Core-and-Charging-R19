## ADR-0272: the handover reject branch, and a suite-wide readiness-timeout flake it surfaced

**Date:** 2026-09-04
**Status:** Closed (work pushed; last citing commit 40b7192 on origin/main, 2026-10-06). Accepted

### The reject branch

ADR-0269/0270/0271 cover the accept path from `HandoverRequired` through to the source gNB's
release. The refusal path -- the target gNB answering `HandoverFailure`, which AMF must relay to
the source as a `HandoverPreparationFailure` carrying the **target's own** cause -- was implemented
(ADR-0096) and had no end-to-end test, because the test gNB always admitted.

`NgapTestGnb::build_handover_failure` (mandatory IEs per `HandoverFailureIEs`: AMF-UE-NGAP-ID(10),
Cause(15), sending `no-radio-resources-available-in-target-cell` -- value 13, read from the
generated enum rather than remembered) and
`AmfNgapTestGnb.TargetGnbRefusalIsRelayedToTheSourceWithItsOwnCause`.

**Why the test asserts on the Cause and not just the message type.** Every other failure in this
chain also ends in `HandoverPreparationFailure` on the source association: an unregistered target,
a PER decode failure, the 10-second timeout, no preparable PDU session, a reply that is neither
acknowledge nor failure. So "the source received a HandoverPreparationFailure" proves almost
nothing. Asserting that the relayed cause is the one the **target** sent is what separates "AMF
relayed the refusal" from "AMF gave up for one of five other reasons and it happened to look the
same". Without that assertion this test would pass against an AMF whose relay branch was deleted.

The setup is the accept path's, unchanged through the target's answer -- real UE, real PDU session,
real UPF Sx association -- because AMF only contacts the target at all if it really prepared a
session.

### The flake this found, which was not in the handover code

CI run 33878218832's `sanitize (tsan)` leg failed on
`UdmSdmGapClosure228Integration.GroupAIndividualUdrRoutesReturnRealSeededData` -- unrelated to any
handover change. The evidence, from that job's own log:

```
13:58:02.926  nrf: registered new NF instance ... (type=UDR, status=REGISTERED)
13:58:03.011  test_udm_sdm_gap_closure_228.cpp:89: Failure -- "udr never became reachable"
```

UDR became reachable and the test gave up **85 ms apart**. `wait_reachable`'s budget there was 50
attempts at 100 ms = 5.0 s, and UDR under TSan took just over that to start, load config, connect
to PostgreSQL and register with NRF.

That budget was not one test's choice: **all 54 `wait_reachable` call sites across the suite used
the same 50**, so this was a latent, suite-wide flake that happened to land on one test. All 54 are
now 200 attempts (20 s), matching the budget `wait_for_sbi_peers` already uses in the handover
tests. Verified first that no call site waits for *un*reachability, which a longer budget would
have slowed down -- none does; every one expects success, and a successful wait returns on its
first attempt, so the change costs nothing when things are healthy.

Recorded plainly: this is a test-harness robustness fix, not a product fix. Nothing in any NF got
faster, and a genuinely dead NF now takes 20 s to report instead of 5.

---

