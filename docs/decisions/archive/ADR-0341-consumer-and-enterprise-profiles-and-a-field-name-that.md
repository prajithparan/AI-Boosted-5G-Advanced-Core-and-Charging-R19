## ADR-0341: consumer and enterprise profiles, and a field name that silently voided them

**Date:** 2026-09-11. **Status:** Closed (work pushed; last citing commit cc748ae on origin/main, 2026-10-06). accepted.

ADR-0340 showed that per-subscriber persistence is what makes a dataset learnable. This adds the
second kind of structure a real base has: **segments and products**. 20% enterprise, 80% consumer,
each with a product mix, and every profile **derived from the subscriber index** so any worker
computes the same customer without shared state.

Products exercised, each on its real rating path rather than as a label: consumer data bundles
(GB -> `totalVolume`), service-unit bundles (events -> `serviceSpecificUnits`), time-based
(MIN -> `GrantedUnit.time`), voice+data (rating-group set), enterprise **slice-scoped**
(`chargingScope` on DNN), **roaming** (serving PLMN != home), and shared/family buckets. Enterprise
lines sit ~7x above consumer in mean volume, which is the single most important structural
difference for a usage model.

### The bug that made all of it inert

TS 32.291 names the field **`pDUSessionChargingInformation`** — capital PDU. The generator sent the
natural-looking `pduSessionChargingInformation`.

A spec-generated DTO simply does not see it: **no error, no warning, no rejected request**. Every
PDU attribute — `dnnId`, `sNSSAI`, `ratType`, `hPlmnId`, `servingCNPlmnId` — was absent, so no
scope-constrained offering could ever match and no session was ever roaming. The run reported
success, the CDR count looked right, and the dataset contained **one unscoped product wearing
several names**.

It was found by bisecting: a scope on `subscriberIdentifier` (set from a different source) matched,
while scopes on `dnnId` and `ratType` never did. That isolated the fault to the PDU block rather
than to the scope mechanism — which works exactly as ADR-0303 built it.

### The second half: attributes must ride on every request

Fixing the name was not enough. The **Update** carries the usage, and each Update **re-rates** — so
a scope-constrained product only matches if the slice, DNN and PLMN are present *again*. With them
only on Create, the trainable rows had no grant and `is_roaming` was false for every roaming
subscriber. They now ride on Create, Update and Release.

| rating group | product | rows | grant | roaming |
|---|---|---:|---|---:|
| 1 | consumer data | 4,496 | volume | 519 |
| 2 | time-based | 1,151 | `time` (no CDR column) | 0 |
| 3 | service units | 414 | units | 0 |
| 4 | enterprise slice | 514 | volume | 0 |

### The shape of both defects

Both were **silent**. The wrong field name produced 201s; the missing Update attributes produced
valid CDRs. Neither surfaced as an error anywhere, and both left a dataset that passes every
easy check — right row count, right subscriber count, plausible volumes — while being wrong in the
one dimension it was built for. The only thing that catches this class is querying the data the way
its **consumer** will, and checking that each product actually appears.

