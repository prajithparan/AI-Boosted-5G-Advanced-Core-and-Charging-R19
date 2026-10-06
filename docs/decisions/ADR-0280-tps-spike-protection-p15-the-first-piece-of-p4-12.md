## ADR-0280: TPS spike protection (P15) -- the first piece of P4.12

**Date:** 2026-09-05
**Status:** Accepted

### What was missing

P15 ("protocol-level spike protection / TPS governance") has been on the principle list since
ADR-0044, which recorded it as "explicitly named as P4.5's own deliverable; not addressed". It was
never built. The honest position until now: a signalling storm -- mass re-registration after a RAN
outage, a runaway peer, a retry stampede -- had **no defence anywhere in this project**. Every NF
accepted whatever arrived and failed on its own terms.

### What was built

`sbi_core::TokenBucket` plus a shed path in `sbi_core::http2::Server`, wired into **all 22** NF and
BSS servers.

- **Token bucket, not a fixed window.** A window's boundary admits 2x the intended rate across its
  edge -- precisely the spike this exists to stop.
- **Sheds after routing, before the handler.** A 404 stays a 404 (an unroutable request is a client
  error, not load worth defending against), but a real request is refused before any work is done.
  Shedding that costs as much as serving is not protection.
- **The response is the spec's own**: `503` + `ProblemDetails`, the shape
  `TS29571_CommonData.yaml` defines for Service Unavailable. `Retry-After` is added as plain HTTP
  (RFC 9110 §10.2.3) -- TS 29.571 defines no retry hint, so that is standard HTTP layered on the
  spec's status, and is labelled as such rather than presented as a 3GPP requirement.
- **The bucket starts full.** An NF receiving its configured burst immediately after startup is not
  in overload; starting empty would shed the legitimate startup surge this is meant to survive.

### Off by default, which is what made 22 NFs safe in one pass

`max_tps` absent, null or <= 0 means no ceiling. Every NF that has not opted in behaves exactly as
before, so wiring the mechanism everywhere changed no behaviour anywhere. `SBI_MAX_TPS` /
`SBI_TPS_BURST` override per deployment, following the config-file-first-then-env convention
`nf_config::require` already uses.

**One NF broke the uniform pattern and is worth recording**: UPF builds its SBI server on its own
thread inside `run_sbi_server`, which never received the config object. It now takes the parsed
`TpsLimitConfig` as a parameter rather than the whole config -- the function needs a ceiling, not a
configuration source, which keeps "config is read in main()" true everywhere.

### Test

`tests/integration/test_tps_spike_protection.cpp`:

1. **Bucket semantics, deterministically** -- the burst is admitted, the ceiling is enforced with no
   time elapsed, capacity returns as time passes. No processes, no timing luck.
2. **`read_tps_limit({})` is disabled** -- the property that made the 22-NF wiring safe.
3. **A real NRF driven past a 5 req/s ceiling over real HTTP/2 + mTLS**: asserts it sheds with
   `503` carrying the spec's ProblemDetails and a `Retry-After`, and -- the assertion that actually
   matters -- that **it is still serving two seconds later**. A limiter that wedged the NF would
   pass a "did we see a 503" check and fail this one.

### Disclosed: what "validated under load" does and does not mean here

P4.12's wording is "per-protocol TPS spike protection **validated under load**". What exists is
validation of the **mechanism** under a small, deliberate overload from a single client. That is
not a load campaign, and this ADR does not claim P15 is discharged by it.

Also genuinely not done, named rather than implied:

- **SBI only.** The Diameter (Gy/Rf/Sy) and SS7/M3UA front doors have no ceiling yet. P15 says
  "per-protocol", and one protocol of three is covered.
- **No shed metric is exported yet.** `Server::shed_count()` exposes the number in-process and the
  P12 alarming increment is where it becomes a Prometheus counter and an alert rule.
- **No adaptive/predictive element.** P4.8's capability 5 ("predictive TPS spike protection") is a
  separate, still-deferred item; this is a static configured ceiling.

---

