## ADR-0328: N28/Sy enforcement -- a spending limit now changes the user plane

**Date:** 2026-09-10. **Status:** accepted. Closes the half ADR-0286 deferred.

ADR-0286 built the PCF -> SMF hop and named what it left undone: *"SMF records the pushed
decision; translating a specific decision into a PFCP re-authorisation is a separate increment
with its own N4 work."* Until now a subscriber who exhausted their quota had that fact recorded
against their session and kept running at full rate.

Now a changed `authSessAmbr` is re-authorised onto the session QER over N4. `config/pcf.json`'s
operator-owned action for the `quota-exhausted` counter is 1 Kbps up and down, so hitting a CHF
spending limit throttles the session.

### One derivation, two callers

Establishment already turned PCF's `authSessAmbr` into the session QER's MBR inline. Enforcement
needs the identical derivation, so it was extracted into `auth_sess_ambr_from` and both paths call
it. Two copies could disagree about what PCF asked for, and the symptom -- a subscriber throttled
to the wrong rate -- is invisible until someone complains.

`Create QER` is sent instead of `Update QER` when establishment installed none, which happens when
PCF's original decision carried no parsable AMBR. An Update against a QER that was never created is
rejected, and silently doing nothing would leave an over-limit subscriber unthrottled.

### The UPF bug this uncovered, which is the important part

UPF handled `Update QER` and `Remove QER` in a Session Modification but **not `Create QER`** --
which TS 29.244 Table 7.5.4.2-1 lists there. An unhandled Create QER fell through every branch
without setting `failed`, so **UPF answered RequestAccepted and registered nothing**. SMF would
have counted the subscriber as throttled, with a green metric, while they ran at full rate. An
accepted request that does nothing is worse than a rejected one: it manufactures evidence.

Reading UPF's modification path before asserting on the counter is what caught it. A test that only
checked `smf_pcf_policy_enforced_total == 1` would have passed and confirmed the wrong thing.

### A second incoherence, fixed rather than worked around

With no eBPF/XDP datapath (no privileges -- the normal state in CI and in this dev environment),
UPF **accepted** a Session Establishment while skipping QER registration, but **rejected** a
Session Modification for that same reason. It would accept a session it then refused to modify.

The environmental case is now separated from the malformed case: a Create/Update QER missing its
Mandatory IEs is still rejected; one that cannot be applied because no datapath exists is accepted
with a warning that says plainly that traffic is not rate-limited. This is a genuine inconsistency
independent of the test that exposed it -- and the malformed-request rejection was deliberately not
weakened to make anything pass.

### What the test proves, and what it does not

`N28SyEndToEnd` now spawns UPF and asserts `smf_pcf_policy_enforced_total`. Proven end to end:
a CHF spending-limit status change reaches PCF, PCF pushes the operator-configured decision to the
SMF that owns the session, SMF derives the new rate and sends a real PFCP Session Modification
carrying the re-authorised QER, and a real UPF process parses and accepts it.

**Not proven on this host: that packets are actually rate-limited.** The XDP datapath needs
privileges CI does not have. So the SMF log says "UPF accepted the re-authorised rate", not
"enforced on the user plane", and `smf_pcf_policy_enforced_total`'s own description says it counts
accepted PFCP modifications rather than confirmed packet-level limiting. The metric name is
stronger than what it can prove; the description corrects it rather than the name being quietly
trusted.

### The test was winning a race it did not know about

Adding the enforcement assertion made it fail two different ways, both pre-existing:

1. It created the SM context before SMF's Sx association with UPF was up -- SMF answers 201 and
   skips N4 establishment, so the session had no `upSeid` and there was nothing to modify. Now
   retried against a real readiness probe (`HANDOVER_REQUIRED` answers 200 only once a UPF N3
   F-TEID is on record), the same pattern `test_smf_n2sminfo_dispatch.cpp` already used.
2. It hardcoded `smpolicy-1`. PCF allocates sequentially and each retry creates another policy, so
   the id is now **counted** from the number of contexts actually created. The original value was
   correct only because the test had always created exactly one.

### Still not done, named rather than approximated

**Gate closure.** `TrafficControlData.flowStatus` is per-PCC-rule while this SMF installs one
session-level QER, so mapping one onto the other would assert that blocking a flow blocks the
session -- an enforcement semantic TS 29.512 does not state. Fully barring a subscriber is a PDU
session release, a different procedure.

**The GUI.** This closes the PCF+SMF half of the 2026-08-16 directive. The `policy_counter_actions`
data model it would edit is live in `config/pcf.json`; Phase 7 has not started and its stack
decision is still open with the user. That is now the only remaining half of that directive.

