## ADR-0460: AMFLocationUpdate end to end -- a real N2 handover through the real AMF process

**Date:** 2026-10-06. **Status:** Closed (work pushed; last citing commit 5311e57 on origin/main, 2026-10-06). accepted. LI increment 4, the second of the two items ADR-0457
left uncovered ("LocationUpdate end to end ... fires on N2 PathSwitchRequest / HandoverNotify,
which this flow does not drive"). The hook itself (ADR-0440, `ngap_handover.cpp` HandoverNotify)
already existed; what was missing was proof through the real process.

**Decision.** A second test in `li_amf_e2e_integration_tests`
(`LiAmfEndToEnd.RealAmfEmitsLocationUpdateXiriOnN2HandoverNotify`): the same LI-enabled AMF config
and "All" warrant as ADR-0457, plus a real UPF (without one SMF has no N3 tunnel and the relay ends
in HandoverPreparationFailure, ADR-0267), two gNBs on two real SCTP associations, a real UE
registered and PDU-session-established on the source, then the full HandoverRequired ->
HandoverRequest/Ack -> HandoverCommand -> HandoverNotify relay. `NgapTestGnb::build_handover_notify`
gained an optional `nr_cell_id` (default 1, so every existing caller is unchanged); the target
reports cell 0x2A, so the assertion is that the xIRI's location is the one the AMF parsed out of
THAT HandoverNotify, not the Registration's cell carried over.

**What it proves.** Exactly three xIRIs reach the loopback MDF2: Registration,
IdentifierAssociation, and one AMFLocationUpdate carrying the target IMSI and NR cell 0x2A, PLMN
999/70.

**Negative control (run, not assumed).** With the `nr_cell_id` argument removed from the test's
notify the test fails: `nr.ncgi.nr_cell_id` is 1, expected 42 -- the assertion has teeth.

**Found while doing it.** The new helper's first version fetched the OAuth token once, before NRF
was necessarily serving (NRF is not among `wait_for_sbi_peers`' probes); it now retries for up to
10 s. Separately, two local runs failed because a CI run's Test step on the self-hosted runner was
using the same loopback ports (the known collision, `feedback_ci_tests_collide_with_local`); CI run
37401442303 (docs-only, superseded by this push) was cancelled and the runs repeated clean.

**Still not covered, disclosed.** LocationUpdate on N2 PathSwitchRequest (the hook is wired at
ngap_task.cpp, not driven here) and StartOfInterceptionWithRegisteredUE (not wired: needs the POI
to see AMF registration state at X1-activation time). Fixed LI_X1 port 19821 unchanged.

