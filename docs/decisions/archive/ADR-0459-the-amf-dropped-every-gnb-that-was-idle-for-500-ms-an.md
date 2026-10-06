## ADR-0459: the AMF dropped every gNB that was idle for 500 ms -- an inherited SO_RCVTIMEO

**Date:** 2026-10-05. **Status:** Closed (work pushed; last citing commit e1d971d on origin/main, 2026-10-06). fixed. Severity: high -- a real gNB is idle between procedures
far longer than 500 ms, so in production the AMF would have kept tearing down N2 associations.

**Found as** a TSan-only CI failure of `AmfNgapTestGnb.FullN2HandoverRelayThroughExecutionAnd-
SourceRelease` (run 37264643538). The AMF log shows the target gNB's association "closed" at
00.982, almost exactly 500 ms after its NGSetup at 00.481, and the HandoverRequest 80 ms later
found no registered target ("no reply from target gNB (not registered...)" in the same
millisecond it was sent). Under TSan the source UE's register + PDU session took longer than
500 ms; in faster builds it happened to finish inside the window, which is why the test passed
elsewhere.

**Root cause.** ADR-0394 put a 500 ms `SO_RCVTIMEO` on the AMF's NGAP *listening* socket so the
accept loop could poll for SIGTERM. Linux copies that option to each accepted socket.
`SctpSocket::receive()` deliberately reports a timeout as "nothing came back" (the same as a
closed association), and the association loop treats that as "gNB association closed": it
unregisters the gNB and its UE and exits.

**Fix.** `SctpSocket::accept_or_timeout()` clears the receive timeout on the accepted socket, so an
association blocks indefinitely again, as it did before ADR-0394. Association threads are stopped
by `shutdown_now()` (the coordinator ADR-0394 built), not by a timeout, so nothing relied on the
inherited value. Fixed in the library, so any future listener using the poll pattern is covered.

**Proof.** `SctpAcceptTimeout.AnAcceptedAssociationDoesNotInheritTheListenersPollTimeout`: listener
poll timeout 100 ms, client idle 500 ms then sends three bytes; the accepted side must receive
them. Results recorded below.

**Results.** With the fix the test passes; with only `sctp_socket.cpp` reverted it fails ("receive()
returned before the client spoke"). With the fix, all 15 AMF NGAP / deregistration / LI end-to-end /
SCTP tests pass locally, including the handover relay that failed in CI. The other red job in that
run (ASan) was infrastructure: its Build step was cancelled, not a test failure.

