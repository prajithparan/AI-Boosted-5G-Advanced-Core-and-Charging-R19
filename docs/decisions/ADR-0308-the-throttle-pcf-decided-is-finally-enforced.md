## ADR-0308: the throttle PCF decided is finally enforced

**Date:** 2026-09-06. **Status:** accepted. **Implements:** ADR-0300's C3.

README listed tiered/fair-use throttling as "decided, not yet enforced", and that was exact: PCF's
`authSessAmbr` reached SMF, went into the NAS Establishment Accept, and **told the UE a rate limit
that nothing applied**. The subscriber's own modem was the only thing that knew about it.

### The gap was one message, not a subsystem

**UPF has had real QER enforcement since ADR-0071.** It decodes `CreateQer`, reads `GateStatus` and
`Mbr`, and calls `datapath->register_qer(teid, ul_gate, dl_gate, mbr_ul_kbps)`. Every piece was
already there and tested. SMF simply never sent a `CreateQer`.

So this ADR is small on purpose: SMF builds one from PCF's decision during N4 session
establishment. Finding that the enforcement half already existed is the reason C3 cost a day's work
rather than a datapath project -- and it is worth recording, because "throttling is not enforced"
read like a missing capability when it was a missing IE.

### Three decisions inside it

- **The AMBR is read ONCE, before the N4 establishment.** It was previously read further down,
  purely to build the NAS message. Reading it twice would let the rate the UE is told and the rate
  UPF enforces diverge -- a discrepancy neither side could diagnose. Both now come from one read.
- **Gate status is OPEN/OPEN.** This QER applies a RATE limit; closing a gate drops the session's
  traffic entirely. That is a different policy decision, expressed differently by PCF, and is not
  inferred from the presence of an AMBR.
- **An unparseable AMBR sends NO QER**, with a loud warning, leaving the session unthrottled. A
  guessed rate limit looks deliberate and is wrong silently; no limit at least matches the
  behaviour every previous release had.

### ambr_to_kbps, and why it earns its own tests

TS 29.571's `BitRate` is "1 Mbps" / "200 Kbps" / "5 Gbps"; TS 29.244's MBR IE is kbps. Every
failure mode of that conversion is **silent**: a factor-of-1000 error throttles a subscriber a
thousand times too hard or not at all, and neither appears in a log, a metric or a failed request
-- the only symptom is a complaint or a bill weeks later. So it lives in its own translation unit
(`nfs/smf/src/ambr.{hpp,cpp}`, the same pattern as `proportional_debit` and `charging_scope`) with
five tests covering every unit, case-insensitivity, fractional rates, refusal of the unparseable,
and the fact that `0 Mbps` is a real rate rather than a parse failure -- the two mean opposite
things to the QER-building code.

### Still not enforced

Only the **session** AMBR is installed. Per-QoS-flow MBR/GBR from PCF's `pccRules` is not, and this
QER is session-scoped rather than bound to individual PDRs. UPF's own downlink datapath limits
still apply (its header discloses no downlink GTP-U encapsulation), so downlink enforcement is
bounded by that, not by this change.

565/565 against a verified-current build, with C1's shared-bucket tests running for real against a
local PostgreSQL rather than skipping.

