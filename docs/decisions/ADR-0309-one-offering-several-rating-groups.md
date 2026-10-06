## ADR-0309: one offering, several rating groups

**Date:** 2026-09-06. **Status:** accepted. **Implements:** ADR-0300's C4.

A voice+data bundle is one product that applies to several rating groups, because voice and data
arrive on different ones. The `ratingGroup` characteristic had to be a single integer equal to the
request's, so one offering served exactly one rating group and a combined bundle had to be modelled
as **two separate offerings** -- which can drift apart in price, validity and lifecycle without
anything detecting it.

An ARRAY now means any-of. A single integer behaves exactly as before, pinned by test.

### Same semantics as chargingScope, on purpose

ADR-0303 already gave array values any-of meaning in `chargingScope`. Using the identical rule here
rather than inventing a second convention is deliberate: two different meanings for "an array in a
catalog characteristic" is the kind of inconsistency an operator discovers the expensive way, in
production, on a bill.

The two mechanisms stay orthogonal -- `ratingGroup` says WHICH services a bundle rates,
`chargingScope` says WHERE it applies -- so a roaming voice+data bundle is expressed by both, and a
test covers that combination.

### A misconfigured characteristic matches nothing

A string, an object, a null or an array of strings matches **no** rating group rather than every
one. This direction is load-bearing: an offering whose rating-group scope cannot be read must not
silently become the offering that rates every request in the system.

### What is and is not "combined", stated exactly

- **Newly true:** one product definition -- one price, one validity, one lifecycle -- spanning
  voice and data rating groups.
- **Already true, not new:** the MONEY was always pooled. Both rating groups reserve and debit
  against the same balance bucket (the subscriber's, or their shared family bucket since ADR-0307),
  so a subscriber's spend has always been combined.
- **Still not true:** a single UNIT allowance decremented by both -- "10 GB usable as either data
  or voice minutes". Each rating group still receives its own `GrantedUnit` sized by the price, and
  pooling units across dimensions would need exactly the minutes-to-octets conversion ADR-0297 and
  ADR-0304 both declined to invent. Monetary pooling is the honest form of a combined bundle here,
  and it works.

