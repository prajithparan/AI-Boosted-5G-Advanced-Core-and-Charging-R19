## ADR-0243: CHF records the real TS 32.291 `invocationTimeStamp` instead of its own write time

### Context

Found by live verification during ADR-0192's follow-up (the training sidecar's real Doris fetch
path), not by reading the code: real `Nchf_ConvergedCharging` requests sent with
`invocationTimeStamp` values of `2026-08-31T14:MM:0NZ` landed in Doris carrying `19:39:37` /
`19:39:38` -- CHF's own wall-clock write time, with many rows sharing a single second.

`charging_engine.cpp`'s `write_converged_charging_cdr` set
`cdr.invocation_time_stamp = system_clock::now()` unconditionally, discarding the value the
consumer actually sent. `ChargingDataRequest_Nchf_ConvergedCharging::invocationTimeStamp` is a
**mandatory** (non-optional) field in the generated DTO -- it is always present, so this was
never a missing-data fallback, it was a real value being overwritten.

Two real consequences, both confirmed rather than assumed:

1. **A real 3GPP field was misreported.** `schema.doris.sql` already has `recorded_at DATETIME
   DEFAULT CURRENT_TIMESTAMP` for write time, so the two columns were duplicating each other while
   the real event time was lost outright. This also reached the encoded CDR: `cdr_asn1.cpp`'s
   TS 32.298 encoder reads `record.invocation_time_stamp` for its own timestamp, so every
   BER-encoded CHF-CDR carried the wrong instant too.
2. **It degraded a real model input.** `train_quota_sizing.py`'s `inter_invocation_interval_sec`
   feature is computed as the delta between consecutive rows' `invocation_time_stamp`. With write
   time substituted, rows written in the same second produce intervals of ~0 regardless of the
   real spacing between charging events.

### Decision

**New `sbi_core::parse_rfc3339` / `parse_rfc3339_to_time_t`** (`libs/sbi-core/.../datetime.hpp`),
the inverse of the `format_rfc3339` already living there, and kept in the same place for the same
stated reason: any NF with a `DateTime`-typed JSON field needs it, not just CHF. It accepts the
full RFC 3339 grammar TS29571_CommonData.yaml's `DateTime` permits -- not merely what this
project's own formatter emits, since a real peer NF is entitled to send any of it: `Z`/`z` or a
real numeric offset (`+05:30`, `-08:00`) applied to yield UTC, a fractional second of any length
(truncated to milliseconds, not rejected), and `T` or `t` as separator. It returns
`std::optional`, so a malformed value is a caller decision rather than a silently wrong instant,
and rejects calendar-invalid dates such as `2026-02-31` via `year_month_day::ok()`. Deliberately
hand-parsed with `std::chrono` calendar types rather than `strptime`: no locale/TZ dependence.

**Threaded through the real CDR path**: `write_converged_charging_cdr` and `charge_one_usage` take
a trailing `std::optional<std::time_t> invocation_time_stamp` (defaulted to `std::nullopt`, so
every pre-existing caller keeps compiling and behaving identically). The three real SBI sites --
Create, Update, and the Release-path CDR row in `main.cpp` -- pass
`sbi_core::parse_rfc3339_to_time_t(body->invocationTimeStamp)`.

### What is deliberately NOT changed, and disclosed rather than quietly left

- **Diameter Gy and CAP keep write time.** RFC 4006 carries an Event-Timestamp AVP, but this
  build's CCR decoding does not extract one, and `cap_server.cpp` has no event timestamp either.
  Neither caller has a consumer-supplied event time to record, so both fall back to write time
  with an explicit comment at the call site saying exactly that. This is a real, disclosed
  approximation -- not a claim the consumer sent the value.
- **`QuotaFeatureStore`'s own timestamp stays `now()`.** That records when CHF observed the usage
  for runtime AI feature lookup, a genuinely different quantity from the CDR's event time. Not
  changed on the theory that it looked similar.
- **Existing CDR rows are not backfilled.** The rows already in Doris carry write time and cannot
  be corrected -- the real event time was never stored. Disclosed, not silently left to look like
  clean data.

### Testing

8 new `Rfc3339DateTime.*` conformance tests covering the basic UTC form, the epoch, fractional
truncation (including sub-millisecond precision), numeric offsets in both directions plus an
explicit `+00:00` equivalence to `Z`, lowercase separators, a round trip against `format_rfc3339`,
leap-second clamping, and 14 malformed inputs that must each be rejected.

**A real error in the tests themselves, caught before it ever ran**: the expected epoch constant
for `2026-08-31T14:00:01Z` was hand-computed as `1788141601` and is actually `1788184801`. It was
found by computing the value independently rather than reading it back from this project's own
formatter -- a self-consistency check would have agreed with a shared offset bug. Same lesson the
crypto work already recorded: cross-check against something that is not the implementation.

