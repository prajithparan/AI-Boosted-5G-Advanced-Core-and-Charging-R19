## ADR-0305: roaming as a derived attribute -- C5's rating half

**Date:** 2026-09-06. **Status:** accepted. **Implements:** the rating half of ADR-0300's C5.

C5 recorded that "rating does not distinguish roaming from home traffic". ADR-0303 made that
expressible -- an offering scoped `{"servingCNPlmnId": {...}}` matches one visited network -- but
expressible is not usable: **a roaming tariff applies to every visited network**, so scoping by
exact PLMN would mean one offering per roaming partner and a catalog edit for every new agreement.

So `roaming` is added as the one **derived** attribute in `collect_charging_attributes`. Every
other key there is copied straight off the request; this one is computed as
`servingCNPlmnId != hPlmnId`. That is derivation, not invention -- it is the definition of roaming,
and both operands are real fields of the TS 32.291 request this code already reads.

### Omitted, not defaulted

When either PLMN is absent, `roaming` is **left out of the attribute set entirely** rather than
defaulted to `false`. The asymmetry matters and it is the expensive direction to get wrong:
defaulting to false would rate genuinely-roaming traffic at the home price whenever a consumer
omitted a PLMN field, and that error is silent, systematic and in the operator's disfavour. With
the attribute omitted, a scope constraining `roaming` simply does not match (ADR-0303's
fail-closed rule), so neither a roaming nor a home tariff is applied on a request that cannot
answer the question. Both directions are pinned by test.

### Composability was the point of doing C7 first

`roaming` is just another key, so "roaming traffic on slice 1" is one scope
(`{"roaming": true, "sNSSAI": {...}}`) rather than a special case in the rating engine. This is the
concrete payoff of moving C7 ahead of C5 in ADR-0300's revised order: C5's implementation is one
derived field and three tests, because the mechanism already existed.

### What C5 still does not have

**Settlement.** `libs/tap3-core` implements GSMA TD.57 TAP 3.12 in both directions (112 codec
functions, all nine `CallEventDetail` variants), and `bss/roaming-interconnect` can encode and
decode a whole `DataInterchange` -- but nothing collects this project's own roaming CDRs into a
batch with sequence numbering and audit totals, and nothing ingests a partner's file to rate the
inbound records. That is TAP OUT and TAP IN processing, the next item, and it needs no further
documents. RAP (TD.32) and NRTRDE (TD.35) remain genuinely absent.

README's roaming row moves from "Partial" to "Partial -- rating done, settlement not", which is
more precise than either "Partial" or "Supported" would be.

550/550 against a verified-current build.

