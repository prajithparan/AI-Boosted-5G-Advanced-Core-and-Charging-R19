## ADR-0290: a data race in my own TPS ceilings, found by asking when the accept thread starts

**Date:** 2026-09-05
**Status:** Closed (work pushed; last citing commit d22e609 on origin/main, 2026-10-06). Accepted

ADR-0285 (Diameter) and ADR-0288 (SS7/M3UA) both added a `set_tps_limit()` setter and read the
resulting `std::unique_ptr<TokenBucket>` from the per-connection message loop. That is a data race,
and it shipped.

### The specific mistake

Both `DiameterServer` and `CapServer` **start their accept thread inside their own constructor** --
`DiameterServer`'s header says so in its first line, and `CapServer::CapServer` ends with
`accept_thread_ = std::thread(&CapServer::accept_loop, this)`. So `main()` calls `set_tps_limit()`
*after* construction, which means it writes the pointer while connection threads may already be
reading it.

The SBI ceiling (ADR-0280) does not have this bug: `Server::set_tps_limit()` is called before
`server.start()`, and that server does not accept until `start()`.

The `TokenBucket` itself was never the problem -- it is internally mutex-protected. **The pointer to
it was.**

### The fix

An owner plus an atomic view: `std::unique_ptr` keeps the bucket alive, and the message loop reads
`std::atomic<TokenBucket*>` with acquire semantics against a release store. Release/acquire rather
than relaxed is load-bearing: a connection thread that sees the pointer must also see the fully
constructed object behind it.

### How it was found, and what that says

Not by a failing test -- **no test exercises either ceiling over the wire**, which is itself a gap
this ADR records. It was found by asking a question the code answers plainly: *when does this
server start accepting?* The header said "immediately", in its own first sentence, and I had not
read it before adding a setter.

In practice the window is small: `main()` sets the limit microseconds after construction, before
any peer connects. That is a reason it had not bitten, not a reason it was correct -- and this
project runs TSan in CI precisely so "small window" does not get to be the argument.

### Still missing, named rather than left implied

Neither the Diameter nor the SS7 ceiling has an end-to-end test. The SBI one does
(`TpsSpikeProtection.RealNfShedsWithA503AndKeepsServing`). Testing the other two means standing up
a Diameter peer and an M3UA/SCTP peer in the test suite -- the codecs are unit-tested
(`test_diameter_core.cpp`, `test_ss7_core.cpp`, `test_tcap_core.cpp`) but nothing drives either
server over its own transport. Until that exists, both ceilings rest on a unit-tested bucket plus
a few lines of wiring that have been read but not executed under test.

---

