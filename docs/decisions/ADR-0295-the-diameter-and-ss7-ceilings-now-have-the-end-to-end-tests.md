## ADR-0295: the Diameter and SS7 ceilings now have the end-to-end tests ADR-0290 said they lacked

**Date:** 2026-09-05. **Status:** accepted.
**Closes:** the ADR-0290 gap that `COMPLIANCE_P1_P15.md`'s P15 row and blocker 5 both recorded.

ADR-0290 fixed a real data race in the Diameter and SS7 TPS ceilings and, in the same entry,
recorded that neither had an end-to-end test. Only the SBI ceiling did. Three tests now exist
(`tests/integration/test_chf_protocol_ceilings.cpp`).

### The two protocols are asserted differently because they shed differently

**Diameter answers.** The test opens a real TCP connection, completes a real CER/CEA, then sends a
CCR-I and asserts the answer's command code is echoed, the hop-by-hop id is echoed, and the
Result-Code is `DIAMETER_TOO_BUSY` (3004). Nothing else in CHF produces 3004, so this cannot pass
by accident. The CER/CEA is asserted too, which rules out "the connection is simply broken" as an
explanation for whatever came back next.

**SS7/M3UA drops.** ADR-0288's deliberate choice: a TCAP abort costs nearly what serving the
message would, so the peer's own invoke timer recovers the dialogue. That makes silence the
expected outcome -- and silence is also what an unservable message produces, so an assertion on
silence proves nothing. The test therefore reads CHF's own log for the line the shed path emits.

### The control case, and a claim I had to walk back to earn

The SS7 test's first version carried a comment saying that without the ceiling this InitialDP
"would produce a reply", making the drop attributable to the ceiling. That was unasserted, and
there was good reason to doubt it: CHF's InitialDP path enters the charging engine, which needs
`bss/product-catalog` and `bss/balance-management`, and the test spawns neither.

Rather than delete the sentence, the control was built:
`Ss7InitialDpIsServedWhenNoCeilingIsConfigured` runs the same two processes and the same message
with no ceiling, and asserts the message reaches CHF's CAP handler. It does -- and it still does
not get a reply, because the InitialDP carries no IMSI and CHF logs exactly that. So the proven
contrast is **dispatched vs. shed**, not "answered vs. silent", and both tests now say so at that
strength and no higher. This is the same failure shape ADR-0293 caught one entry earlier: a green
test sitting next to a claim it does not support.

### Supporting changes

- `SpawnedProcess` takes an optional `log_path` that redirects the child's stdout and stderr into a
  file (`open`/`dup2`, both async-signal-safe, so legal between `fork` and `exec`). Reading an NF's
  own log as evidence is not new -- ADR-0269 and ADR-0270 did it by hand -- this makes it automatic.
- `CHF_DIAMETER_MAX_TPS`, `CHF_DIAMETER_TPS_BURST`, `CHF_CAP_MAX_TPS`, `CHF_CAP_TPS_BURST`:
  environment overrides via a new `chf_tps_setting()` helper, config-file-first then environment --
  the same precedence `nf_config::require` and `read_tps_limit`'s own `SBI_MAX_TPS` already use. No
  test value lives in checked-in config, per the standing rule (task #109).
- Both tests set a burst **below one token**. `TokenBucket` starts full at capacity and
  `try_acquire` needs a whole token, so the first request after the handshake sheds
  deterministically -- no sleep, no refill race, and no dependence on the charging backends, since
  both ceilings sit in front of dispatch. Each test also asserts the ceiling was actually applied
  ("... TPS ceiling active" in the log) before proceeding, because `set_tps_limit()` is called after
  the accept thread starts (ADR-0290) and a silently-unapplied ceiling would make the rest of the
  test prove nothing.

### Still open

**No load campaign.** These prove the mechanism fires; they do not measure behaviour under
sustained overload, and P15's own gap list still says so. The ceilings remain off unless
configured, which is an opt-in policy choice rather than a caveat.

