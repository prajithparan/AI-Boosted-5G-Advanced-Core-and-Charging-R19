## ADR-0362: SMF hung on every orderly stop; Kafka in CI; LI before the rest of NWDAF

**Date:** 2026-09-14. **Status:** accepted.

**The hang.** The first CI run to reach the Test step since ADR-0353 (`main()` returns on
SIGTERM so destructors flush buffered CDRs) failed 16 tests by timeout, every one involving the
SMF. Reproduced locally in one test: the SMF logs "signal 15 received" and never exits.
`smf::PfcpPeer::~PfcpPeer()` joins its receive thread, which sits in a synchronous Asio
`receive_from`; Asio implements that with a non-blocking socket plus `poll()`, so the
`SO_RCVTIMEO` the constructor sets to wake the loop never fires, `stop_` is never observed, and
`join()` blocks forever. Before ADR-0353 `main()` never returned, so the destructor never ran and
the defect was invisible. The UPF had the same shape and ADR-0357 cured it with a loopback
nudge datagram; the SMF's destructor now sends the same nudge before joining, and the loop checks
`stop_` immediately after the receive. Orderly stop: 4 s, was >120 s. The test harness's
`SIGTERM + waitpid(…, 0)` is what turned a shutdown hang into a timeout per test -- correct
behaviour, and the reason the defect surfaced at all.

Seen in the same log and not yet root-caused: a `Fatal glibc error: tpp.c:83` and a `terminate:
Owner died` around the AMF/NSACF teardown of one test. Consistent with a detached thread (the NRF
heartbeat loop every NF runs) touching a static -- spdlog's registry, the OTel provider -- that
static destruction has already torn down, which is also new since main() started returning.
Recorded here so it is chased rather than forgotten; the local re-run of the 16 tests is the
first evidence either way.

**Kafka in CI.** User-directed: `apache/kafka:3.9.0` (KRaft, single node) is now a service in
both the build and sanitize jobs, and the Test steps carry `CHF_CDR_EVENT_BUS_BROKERS=
127.0.0.1:9092`, so `test_cdr_event_bus` runs instead of GTEST_SKIPping, and the MFAF's tests
will have a broker from their first commit. One advertised listener (`127.0.0.1:9092`) because
jobs run on the runner VM, not in a container -- deliberately different from the two-listener
lab compose service.

**LI before NWDAF.** User-directed, answering the standing question from the pre-NWDAF
checklist: Lawful Interception (production blocker #0, TS 33.127/33.128) is built before the
remaining NWDAF-ecosystem steps (ADR-0359 steps 1-6). ADR-0359's design and ADR-0360's
externalised state stand; the queue order changes. The MFAF design notes are kept for when the
queue returns to it.

