## ADR-0239: Real server-side request concurrency -- Phase 1 of ADR-0009's synchronous-HTTP debt

### Context

Following ADR-0238's own stated order (close the synchronous-HTTP-client debt before
benchmarking), investigation found the real problem was worse than "the client blocks":
`libs/sbi-core/src/http2_server.cpp`'s `Server::Impl` drove its `boost::asio::io_context` via a
single bare `ioc.run()` call in every NF's `main()` -- meaning every NF process could only ever
process ONE request at a time, full stop, with zero concurrency for even two unrelated local-only
requests. Compounding this: `Client::send()` (`libs/sbi-core/src/http2_client.cpp`) already
serializes all calls through one `std::mutex`-guarded curl easy handle per `Client` instance, and
each NF holds exactly one shared `Client` per downstream peer (e.g. UDM's own `udr_client`) --
so even fixing server concurrency alone would just move the bottleneck to that one mutex for the
17 NFs that make outbound calls.

Chosen scope for this turn (a real, user-approved phased plan): Phase 1, real server-side request
concurrency, landing first since it needs zero changes to `Handler`'s public signature (still
synchronous `std::function<Response(const Request&)>`) or to any NF's route-handling code. Phase 2
(pooling `Client` instances so outbound calls stop serializing on one mutex) and Phase 3 (true
async I/O, deferred until real benchmark data shows Phase 1+2 concurrency is insufficient) are
explicitly out of scope for this turn.

### Implementation

- New `sbi_core::run_multi_threaded(io_context&)` (`libs/sbi-core/include/sbi_core/
  io_context_pool.hpp`, `.cpp`): runs `ioc` on `max(2, hardware_concurrency())` worker threads
  (the calling thread plus N-1 more) instead of a bare `ioc.run()`. Wired into all 17 NFs' `main()`
  in place of the old single-threaded call.
- `Server::Impl::Connection` (the class actually doing per-connection I/O) restructured to make
  driving it from multiple OS threads safe: each `Connection` now owns its own
  `boost::asio::strand`, with every touch of its own `nghttp2_session*`/`streams_` map (handshake
  completion, read completion, write completion, response submission) bound to that strand via
  `bind_executor`/`post` -- required because `nghttp2_session` itself is not thread-safe, and
  before this change a single connection's own I/O steps could otherwise run concurrently from
  different worker threads.
- The actual route `Handler` invocation in `handle_request_complete` is posted OFF the connection's
  strand, onto the shared `io_context` directly, so a slow or blocking handler for one HTTP/2
  stream never stalls that connection's own further I/O (or, now that the io_context has real
  worker threads, any other connection's). The response is submitted back by posting onto the
  connection's own strand again (`submit_response`), since that's the only safe place to touch
  `nghttp2_session_submit_response`/the stream map.
- Real correctness requirement this uncovered, not theoretical: with the handler dispatch above,
  `do_write()` can now be invoked "out of band" (from `submit_response`, once an off-strand handler
  finishes) while a PREVIOUS `async_write` for that same connection is still in flight -- a real
  buffer-corruption race on `write_buf_` without a guard. Fixed with `socket_op_in_flight_`/
  `write_pending_`: a `do_write()` call that arrives mid-write just marks `write_pending_` and
  returns; the in-flight write's own completion handler re-invokes `do_write()` once done. This
  serializes MULTIPLE WRITES against each other only.
- Real, pre-existing OpenSSL requirement this project never hit until real concurrency made it
  reachable: `SSL_CTX_set_session_id_context()` was never called on the server's `SSL_CTX`, which
  OpenSSL requires (its own documented anti-cross-context-session-reuse safeguard) once concurrent
  handshakes/session-cache writes become possible -- manifested as intermittent, real
  `"session id context uninitialized"` handshake failures once multi-threading was enabled. Fixed
  by setting a fixed, non-empty session id context (`"5gc-sbi-core"`) on every NF's server
  `SSL_CTX`; confirmed via 16/16 clean repeated runs after the fix (0/16 before it, reproduced
  reliably).

### A real design decision this ADR arrived at only after testing, disclosed explicitly (not
### silently chosen up front)

An earlier version of the `do_write()`/`do_read()` guard also serialized writes against a
concurrently-outstanding read (reasoning at the time: `boost::asio::ssl::stream` has a real,
documented history of internal races between overlapping read+write engine operations even under
one strand -- see `chriskohlhoff/asio#791`). That stronger guard was implemented, built, and then
REVERTED after live testing surfaced a real, 100%-reproducible DEADLOCK: a response queued behind
an outstanding read that is itself waiting for the client's next frame -- which, for this
project's own synchronous, one-request-at-a-time `Client` (used by every NF-to-NF caller in this
codebase), never arrives until the very response it's blocking is delivered. Root-caused via
direct reproduction (`NrfGapClosureIntegration.OptionsNFInstancesReturnsRealAcceptEncoding` hung
for the ctest default 25-minute timeout with the stronger guard in place; passed in ~250ms once
reverted). A confirmed hang is strictly worse than a narrower, documented, disclosed data-race
risk in a third-party library boundary this project does not control, so this project accepts
genuine concurrent read+write per connection (what `boost::asio::ssl::stream` is actually designed
to support) and instead suppresses the specific race category this triggers under TSan.

### TSan suppression (`.tsan-suppressions`), real and disclosed, not silently ignored

Running the new concurrency-proving test repeatedly (25+ stress runs) under ThreadSanitizer
surfaced real data races, but consistently inside vendored OpenSSL 3.6.3 / boost::asio's own
internals during concurrent handshake and concurrent read+write -- never in this project's own
code. Confirmed as a known, still-open category of upstream issue, not something newly introduced
here: `https://github.com/openssl/openssl/issues/22351` (and related `#24480`/`#24672`) for the
OpenSSL-internal side (`ASN1_STRING_cmp`/`ASN1_STRING_set` in `crypto/asn1/asn1_lib.c`, `match_key`
in `crypto/hashtable/hashtable.c`), and `chriskohlhoff/asio#791` ("SSL and multithreading problem",
a real, still-open, unresolved report matching this exact `recv1`/`bio_write` symptom) for the
boost::asio-internal side. A first suppression attempt scoped only to `engine::perform` proved
unreliable -- that frame is compiler-inlined away at some call sites (observed: the read+write
path) but not others (observed: the handshake path), so real instances were silently missed. The
suppression file now targets the real, stable frames present at every observed call site instead
(`engine::perform`, `io_op`, `socket_ops::recv1`, `reactive_socket_recv_op_base` -- all
SSL-stream-specific in this codebase; confirmed by direct grep that no other raw `boost::asio` TCP
socket exists anywhere in this project, so none of these rules can mask a race in a genuinely
unrelated part of the codebase). Wired into CI via `TSAN_OPTIONS=suppressions=...` on the shared
`Test` step (harmless on the `asan-ubsan` leg, which doesn't read `TSAN_OPTIONS`).

In every case examined, the race is between threads independently computing/copying the same
canonical value against per-connection or per-process state (the one shared CA certificate loaded
once into each NF's server `SSL_CTX` at startup, or a connection's own private buffers) --
confirmed functionally benign (every test, including 25+ repeated stress runs of the concurrency-
critical tests and a full 461-test regular-build suite run, passes with correct results), but still
real undefined behavior per the C++ memory model, disclosed here rather than silently suppressed
without explanation.

### Live verification

New `tests/integration/test_sbi_core_concurrency.cpp`
(`SbiCoreConcurrencyIntegration.ConcurrentRequestsOnDifferentConnectionsRunInParallel`): an
in-process `sbi_core::http2::Server` with a deliberately slow (300ms) handler, driven by
`run_multi_threaded`; 6 concurrent client threads (each its own `Client` instance/connection, since
this project's `Client` is still synchronous and one-request-at-a-time per instance -- this test
proves the "different connections handled in parallel" dimension of Phase 1; it cannot yet exercise
"multiplexed streams on one shared connection," since no caller in this codebase can have two
requests in flight on one `Client` instance until Phase 2). Real proof of concurrency: 6 x 300ms
requests complete in ~320ms (confirmed repeatedly), not the ~1800ms a fully-serial (the old,
pre-ADR-0239) server would produce -- the test's own bound (70% of the fully-serial time) cleanly
distinguishes genuine concurrency from serial behavior while tolerating sanitizer instrumentation
overhead.

Full regular-build `ctest` suite (461/461, 6 legitimately skipped for missing local Postgres env
vars) re-run clean at `-j1` after every change in this ADR, including after the deadlock was found
and the guard reverted. No strays before or after any run (`ps aux` checked explicitly).

### What this ADR does NOT include

Phase 2 (client-instance pooling, so outbound NF-to-NF calls stop serializing on one shared
`Client`'s mutex) and Phase 3 (true async I/O) remain open, per this ADR's own stated phased plan.
The reverted stronger read/write serialization means this project has NOT eliminated every
theoretical race inside `boost::asio::ssl`/OpenSSL's own internals when genuine concurrent
read+write happens on one connection -- it has confirmed those races are functionally benign in
every case examined and disclosed/suppressed them, which is a different (and, given the confirmed
deadlock alternative, deliberately chosen) bar than "zero TSan findings anywhere in the dependency
tree."

