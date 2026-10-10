## ADR-0352: the partition window that never existed, and TS 29.500 duplicate-request detection

**Date:** 2026-09-12. **Status:** accepted. Unblocked by the user supplying TS 29.500.

### 1. CI was red, and the cause was silent data loss

`CdrBillingChain.RatedUsageBecomesABillThroughRealDoris` and
`CdrBillingChain.ARowWithNoRatedCostProducesNoLineItem` failed with Doris reporting
`no partition for this tuple` for `recorded_date = 2026-08-29`.

ADR-0348 gave the CDR table dynamic RANGE partitioning with `dynamic_partition.start = "-400"`,
intended as a 400-day retention window. **Doris does not create the historical partitions a
negative `start` implies unless `dynamic_partition.create_history_partition` is also set, and it
defaults to false.** The live table had exactly four partitions -- today through today+3. The
400-day window was a comment, not a fact.

The consequence is worse than a red build. A CDR whose `recorded_date` fell outside that four-day
window was **rejected and lost**: `CdrWriter::write` throws, `main.cpp` catches it, logs a warning,
and still answers the Nchf request successfully. The only backstop is ADR-0282's sequence-gap alarm
at Release. A load generator stamping `now()` never trips it, which is why two runs of 1.9M and 3M
rows looked clean. CI caught it because `test_cdr_billing_chain.cpp` backdates a record into a
closed billing period -- which is what a delayed or replayed record from a real SMF looks like.

Fixed by setting `create_history_partition`. Verified rather than assumed: a fresh table built from
the corrected schema has **404 partitions, p20250808 through p20260915**, and the exact tuple CI
rejected now inserts. The running 3M corpus was checked and has zero rejected writes (its generator
stamps `now()`), so it is unaffected.

**Note on the live table:** `ALTER TABLE ... SET ("dynamic_partition.create_history_partition" =
"true")` does not backfill partitions on an existing table -- the property is applied at creation.
Existing deployments need the partitions added explicitly or the table rebuilt. Disclosed rather
than left for someone to discover when a backdated record disappears.

### 2. Duplicate-request detection, TS 29.500 clause 5.2.8

ADR-0351's 3M run reported a steady ~0.3% of `release 404`s. Established by measurement that these
were **not** lost releases: the session was released, and the 404 came from a re-sent request
meeting CHF's own duplicate guard. The generator's accounting was corrected to call these replays,
which was honest but left the protocol-level problem unaddressed. It was left unaddressed
deliberately -- changing a 404 into a 204 is spec-facing, and TS 29.500 was not available offline.
The user then supplied it.

**What the spec actually says** (ETSI TS 129 500 V19.7.0, clause 5.2.8):

- Support "is optional for HTTP clients and servers" -- so the previous behaviour was conformant.
- Clause 6.10.x tells an HTTP/2 client it *should* retry a non-idempotent request when a GOAWAY
  frame proves it was not processed. libcurl does exactly this, and cannot always tell.
- When supported, the client **shall** include an idempotency key in `3gpp-Sbi-Request-Info` for a
  non-idempotent request and **shall** reuse it on a retry; the server **may** use it to detect a
  duplicate and "produce a proper response based on the current state of the resource/session
  context considering the original request has been processed."

Implemented on both halves:

- `sbi_core::headers::parse_request_info` / `request_info_idempotency_key` parse clause 5.2.3.3.12's
  real ABNF. The header had a constant and no parser.
- `chf::IdempotencyStore` remembers each answered response in Redis under its key, with a TTL that
  is the spec's own "operator configurable timer" -- in `config/chf.json`, never hardcoded.
- CHF's Create, Update and Release handlers replay the remembered response on a duplicate.
- The traffic generator sends a key on every POST. libcurl re-sends *that* request with *that*
  header, so the key is carried unchanged onto the retry, which is the "same key on retry" the
  clause asks for without the tool needing to observe the retry.

Verified end-to-end on a CHF instance isolated from the running corpus: same key three times gave
one session replayed three times; a different key gave a new session; **no key behaved exactly as
before**; a replayed Release returned **204**, while a keyless release of an already-closed session
still returned **404**. That last pair is the point -- the genuinely-unknown-ref 404 is preserved.

### A correction, and the fix that was actually needed

This ADR first claimed the release-404s were retransmissions arriving **after** the original had
completed, on the strength of a measurement described as decisive: the CDR store held more Release
rows than the generator had counted as successful. **That measurement was wrong and the conclusion
with it.** A Release CDR is written *during* request processing, while the generator increments its
counter only on receiving the response -- so with 48 concurrent workers the store legitimately runs
ahead. The excess was concurrency, not duplication.

What exposed it was shipping the completion-only cache and watching it detect nothing: 17,271 keys
stored, zero duplicates found, and the 404s continuing at the same rate. A retransmission would
have carried the same key and been caught. So the retransmissions were arriving **while the
original was still executing** -- precisely the gap this ADR had disclosed and deferred.

The evidence was in the data all along: subscriber `imsi-999700000023581` held `chg-1295` (Create
only, orphaned, still in the active set) and `chg-1296` (Create + 3 Updates + Release) created in
the same second. Two refs for one create. Roughly 0.6% of creates left an orphaned Create CDR --
a spurious row in the billing record, not a cosmetic defect.

So the key is now CLAIMED atomically (`HSETNX`) *before* processing, and a duplicate waits, bounded,
for the owner to publish. Verified against the real race rather than a sequential approximation of
it: **10 concurrent requests sharing one idempotency key produced exactly one session, one Create
CDR, and 9 logged detections.** The subsequent 3M run reports zero failures, zero replays and zero
orphaned creates, against 300+ replays and 32 orphans before.

The lesson is the one this project keeps relearning: a measurement that confirms the hypothesis is
not evidence until you have checked what else could produce the same number.

### A bug worth recording

The first implementation detected nothing. `req.headers` is a plain case-sensitive `std::multimap`
and HTTP/2 transmits field names lowercased (RFC 9113 clause 8.2.1), so a lookup keyed on the
specification's own mixed-case `3gpp-Sbi-Request-Info` never matched. It failed silently and
looked exactly like "no duplicates occurred". The lookup is case-insensitive now. The codebase's
existing convention -- `req.headers.find("authorization")`, lowercase -- was already the hint.

### Rejected

- **Making Release unconditionally idempotent (204 for any already-released ref).** It would have
  fixed the symptom without a key, but it discards the ability to distinguish a retransmission from
  a genuinely unknown ChargingDataRef, which is a real error an operator needs to see.
- **Disabling connection reuse in the client** (`CURLOPT_FORBID_REUSE`). Correct and far too
  expensive -- a TLS handshake per charging request.

