## ADR-0241: Client handle pooling -- Phase 2 of ADR-0009's synchronous-HTTP debt

**Status:** Closed (work pushed; last citing commit 4922fc1 on origin/main, 2026-10-06).

### Context

ADR-0239 (Phase 1) gave every NF's server real request concurrency, and named the next bottleneck
explicitly rather than leaving it to be discovered: `Client::send()`
(`libs/sbi-core/src/http2_client.cpp`) took `std::lock_guard<std::mutex> lock(mutex_)` as its first
statement and held that lock across `curl_easy_perform` -- the entire blocking network round-trip.
Each NF holds one shared `Client` per downstream peer (e.g. UDM's `udr_client`), so once Phase 1
let multiple handlers run at once, every outbound NF-to-NF call still queued behind every other
call to the same peer. Phase 1 moved the bottleneck here exactly as that ADR predicted it would.

The serialization was not gratuitous. libcurl's own contract is explicit that a single easy handle
must never be driven by two threads at once, and the mutex was added (ADR-0050 Stage 5) in response
to a real, live failure -- `nfs/smf`'s `chf_report_client` gained a genuinely concurrent caller via
that stage's detached per-report thread design, and `curl_easy_perform` started returning
`CURLE_FAILED_INIT`/empty responses. The fix was correct; it was just the coarsest possible one.

### Implementation

`Client` now owns a POOL of easy handles instead of one:

- `checkout()` pops an idle handle under the mutex, or creates one with `curl_easy_init()` outside
  the lock when the pool is empty.
- `curl_easy_perform` runs with NO lock held. The mutex now guards only the `idle_` vector, for the
  duration of a push/pop.
- `checkin()` returns the handle. A private RAII `HandleLease` guarantees the return on every exit
  path from `send()`, including the early `tl::unexpected` returns on libcurl errors.
- `kMaxIdleHandles = 64` bounds RETAINED idle handles, not concurrency: checkout still creates on
  demand above that, but check-in destroys rather than retains, so one burst cannot leave hundreds
  of connections parked open for the process's lifetime.

`Client::send`'s public signature is unchanged, so no NF and no call site was touched.

Two things deliberately preserved rather than rewritten:

- **`curl_easy_reset` on checkout.** It clears per-request options but PRESERVES that handle's live
  connection, TLS session cache and DNS cache. That preservation is exactly what makes pooled reuse
  real HTTP/2 keepalive instead of a fresh connection per call -- the reason the original
  single-handle design reused a handle at all. Pooling keeps that property per handle.
- **The ADR-0050 Stage 5 provenance in the header comment.** The live CHF failure is the documented
  reason a thread-safety guarantee exists here at all; the mechanism description changed, the
  history did not.

### Lifetime contract -- pre-existing, now stated

A `Client` must outlive every in-flight `send()` on it. This was already undefined behaviour with
the single-handle design (the destructor never took the mutex either, so destroying a Client while
another thread was inside `send()` was already a use-after-free); pooling gives it a new expression
(a lease returning into a freed vector) but does not introduce it. Every NF holds its Clients for
process lifetime, which is why this has never bitten. Written into the header rather than
engineered around, since making the destructor safe against concurrent use would mean either a
lock the hot path does not need or a shutdown protocol no caller currently wants.

