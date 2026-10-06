## ADR-0339: CHF could not be run twice on one host

**Date:** 2026-09-11. **Status:** accepted.

Batching lifts one process. The other half of the throughput problem is that the writer serializes
*per process*, so a second CHF is what adds parallel write capacity — and forking is safe here
because `ChargingDataRef` comes from `redis_->incr()`, an atomic shared counter, so instances
cannot collide on refs.

Except CHF could not be started twice. Three separate configurability gaps, each found only by
hitting it:

1. **`port` and `metrics_bind_address` had no env-override name.** `nf_config`'s own header states
   that any key may be overridden by `<SERVICE>_<KEY>` at deployment time; these two simply never
   passed one, so every instance bound the config's port and died with *"Address already in use"*.
2. **`kDiameterPort` and `kCapPort` were hardcoded constants.** Even with SBI and metrics moved,
   the Diameter and CAP listeners still collided. Now configurable, with the IANA-assigned 3868 and
   2905 kept as defaults — changing a protocol port silently would be wrong.
3. **My own first fix used `config.value()`**, which reads the file and *ignores the environment*.
   The override silently did nothing and the instances kept dying with the identical message. Only
   `nf_config::require` consults the environment.

That third one is worth recording as its own lesson: an override that is read from the wrong
accessor fails **exactly like no override at all**, and the error message is unchanged, so the
obvious conclusion is "my env var is wrong" rather than "my code never looked".

### Result

| | |
|---|---|
| Before | ~20 CDRs/sec, degrading with concurrency |
| After (batch 500 x 4 instances) | **235 CDRs/sec** |
| 1M CDRs | ~14 hours → **~71 minutes** |

Doris went from **16.5% CPU to 132%** under the same hardware. It was never resource-starved — it
was starved of *work*, which is why adding resources would not have helped and adding a second
writer did. Host load 8.39 on 8 cores with 8 GB still available.

**Beyond this soak:** CHF being runnable more than once on a host is a precondition for the
HA/clustering debt ADR-0049 names. It was assumed to work and did not.

