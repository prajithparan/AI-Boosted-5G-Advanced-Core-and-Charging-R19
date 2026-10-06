## ADR-0048: Real rating engine -- CHF consults bss/product-catalog to grant real units

**Date:** 2026-08-10
**Status:** Accepted.

**Context:** ADR-0047 built a real, configurable product/tariff catalog but left it unconsulted --
CHF's `Nchf_ConvergedCharging_Create` still always returned an empty grant. This turn closes that
gap: CHF becomes a real HTTP client of `bss/product-catalog` and returns a real `GrantedUnit`
derived from real catalog data, approved before implementation given the several genuine design
decisions involved (rating-group assignment, subscriber-to-product selection, unit conversion).

**Real schema confirmed first.** Checked `TS32291_Nchf_ConvergedCharging.yaml` directly:
`MultipleUnitUsage.ratingGroup` is the one mandatory field (confirmed via the schema's own
`required:` block) -- neither SMF nor CHF populated or read it before this turn.

**SMF now sends a real `multipleUnitUsage`.** `nfs/smf/src/main.cpp`'s
`perform_n40_charging_data_create` adds one `MultipleUnitUsage` entry with a fixed
`kDefaultRatingGroup = 1` -- no real service-to-rating-group mapping exists in this codebase (that
is TS 32.298/32.299 charging-characteristics configuration, out of scope here), so every PDU
session is charged under one fixed group, disclosed rather than invented as something smarter.
`requestedUnit` is deliberately omitted: this build has no real traffic-volume estimator to request
a specific amount against, so CHF grants a full quota from its own rate-plan lookup instead of
SMF asking for one -- a real, disclosed design choice, not an oversight.

**CHF's real rating engine (`build_rating_grant`, `nfs/chf/src/main.cpp`).** Queries
`bss/product-catalog` (mTLS only, no OAuth2 -- product-catalog has no NRF-issued token source,
same trust boundary ADR-0047 already established) for the first `Active`+`isSellable`
`ProductOffering` with a price, fetches its first referenced `ProductOfferingPrice`, and converts
`unitOfMeasure` into a real `GrantedUnit`. No real per-subscriber product assignment exists (no
customer/subscription store) -- "whichever offering is first in the catalog" is the real, disclosed
simplification, same category as PCF's fixed-default policy (ADR-0028). Unit conversion is real
but deliberately narrow: TS 32.291's `GrantedUnit` has no generic "amount + unit" field the way
TMF620's `Quantity` does (`totalVolume`/`uplinkVolume`/`downlinkVolume` are raw octet counts,
`time` is raw seconds -- confirmed via their `Uint64`/`Uint32` typing with no accompanying unit
field) -- only `"GB"`/`"MB"` (decimal, matching 3GPP's own octet-counting convention, not binary
GiB/MiB) convert to `totalVolume`; any other unit string falls back to `serviceSpecificUnits`
carrying the raw amount unconverted. A narrow, real, disclosed conversion -- not a general
unit-aware rating engine. If the catalog is unreachable or has no matching offering, Create still
succeeds with an empty grant (the same fallback this build has always had), not a hard failure --
matching every other best-effort external-dependency call in this project.

**Live-verified with real seeded data, not a synthetic value.** Full stack (nrf, udm, udr, ausf,
pcf, chf, upf, smf, amf, product-catalog) started; a real `ProductOfferingPrice` ("10GB Monthly
Data", $25/month, `unitOfMeasure={amount:10, units:"GB"}`) and a real `ProductOffering` ("5G
Standard Plan") referencing it were seeded via real mTLS `curl` calls to the live catalog service.
A real `nr-gnb`/`nr-ue` PDU Session Establishment then triggered the real chain: SMF's N4 Session
Establishment, SMF's `Nchf_ConvergedCharging_Create` (now carrying `multipleUnitUsage`), CHF's real
catalog lookup, and a real grant. `chf`'s own log: `rating engine granted 10000000000 octets from
ProductOffering '5G Standard Plan' / ProductOfferingPrice '10GB Monthly Data'` -- the exact correct
conversion (10 GB × 10^9 = 10,000,000,000 octets). Verified at the wire level too, not just via
logs: a direct `curl` call to CHF's real `Nchf_ConvergedCharging_Create` endpoint returned
`{"multipleUnitInformation":[{"grantedUnit":{"totalVolume":10000000000},"ratingGroup":1}]}` in the
actual HTTP response body. The new `chf_rating_grant_total` Prometheus counter confirmed
independently via direct scrape (`1`, matching the one real grant issued).

**Consequence:** CHF now makes a genuine, catalog-derived charging decision instead of always
returning an empty grant -- the last major disclosed gap from ADR-0044 ("no real rating/quota
engine") is closed for the online-charging-quota-grant case specifically. Still deferred: quota
*consumption* tracking and re-authorization (this grants once at session establishment and never
checks whether it was used up), real per-subscriber product assignment (needs a customer/
subscription store that doesn't exist), and `Nchf_ConvergedCharging_Update` (which would be the
real trigger for a mid-session re-authorization) -- each a distinct, larger, not-yet-approved scope.

