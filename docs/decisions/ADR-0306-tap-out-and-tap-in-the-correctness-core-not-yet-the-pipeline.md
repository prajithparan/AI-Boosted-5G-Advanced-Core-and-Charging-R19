## ADR-0306: TAP OUT and TAP IN -- the correctness core, not yet the pipeline

**Date:** 2026-09-06. **Status:** accepted. **Implements:** the settlement half of ADR-0300's C5.

`libs/tap3-core` has implemented GSMA TD.57 TAP 3.12 in both directions for a long time -- 112
encode/decode functions, all nine `CallEventDetail` variants -- and `bss/roaming-interconnect`
could encode and decode a whole `DataInterchange`. What did not exist, and what `store.hpp`
disclosed in its own comment, is anything that POPULATES a batch from this project's own roaming
usage or VALIDATES one a partner sent. **A codec is not a settlement path.**

### The property this exists to hold

`AuditControlInfo` must agree with the call events actually present. A batch whose
`callEventDetailsCount` or `totalCharge` disagrees with its own contents is returned via RAP, and
that costs an operator a settlement cycle. So:

- **Totals are DERIVED, never supplied.** `build_transfer_batch` computes `totalCharge`,
  `callEventDetailsCount` and the earliest/latest timestamps from the records it is given. There is
  no parameter for a total, so a caller cannot pass a wrong one. Making the error structurally
  impossible is worth more than checking for it afterwards.
- **`validate_transfer_batch` recomputes and reports every disagreement**, and
  `validate_tap_file` does the same for a partner's raw bytes.

`CallEventDetailList` stores each CHOICE alternative as a pre-encoded `Tlv` (it is an untagged
ASN.1 CHOICE), so validating means DECODING the events back out. That is deliberate: a batch this
project produced and a batch a partner sent are then checked by identical code, and a test asserts
a self-built batch survives its own encode -> decode -> validate round trip.

### The validator abstains rather than guessing

It can only walk GPRS charges -- the variant this project produces, and the only charge path
verified against the spec here. A partner file containing voice records has its events **counted**
correctly and its charge total reported as **"not verified"** rather than computed as zero.
Computing zero would flag every voice-bearing partner file as broken, and a validator that cries
wolf is one people learn to ignore. Honest abstention beats a false mismatch.

### Real TD.57 details, cited not guessed

`fileSequenceNumber` is `NumberString(SIZE(5))` -- five characters, leading zeros, rolling over at
99999 by specification rather than overflowing. `format_file_sequence_number` is exposed precisely
because a caller persisting the counter must render it identically.

### What this is NOT, so "TAP IN/OUT done" is not over-read

This is the correctness core. The operational pipeline around it does not exist:

- **Nothing selects** which CHF CDRs belong to a partner's batch for a period. That is a query over
  the Doris CDR store filtered by the `roaming` attribute ADR-0305 added, plus the partner PLMN.
- **Nothing persists the file sequence counter** across restarts. TD.57 requires it gap-free, and
  an in-process counter would reset and emit duplicate sequence numbers -- worse than none -- so it
  is an explicit caller responsibility stated in the header rather than faked here.
- **Nothing schedules** the outbound run or ingests files from a partner's drop.
- **Only the control-block and audit-agreement checks** are performed. TD.57's full validation rule
  set is far larger and is not claimed.
- **RAP (TD.32) and NRTRDE (TD.35)** remain genuinely absent and would need those documents.

556/556 against a verified-current build.

