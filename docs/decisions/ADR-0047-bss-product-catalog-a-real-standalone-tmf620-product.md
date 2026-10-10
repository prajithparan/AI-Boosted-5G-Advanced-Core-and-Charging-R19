## ADR-0047: `bss/product-catalog` -- a real, standalone TMF620 Product Catalog service

**Date:** 2026-08-10
**Status:** Accepted.

**Context:** User-requested review found a real gap: PROMPT.md's charging principles state "every
SID entity, NRM object and IOC element in scope must be CONFIGURABLE in the charging model --
product and tariff definition is data, not code", but no product/tariff data model existed
anywhere in this repo. CHF's `Nchf_ConvergedCharging_Create` has always returned an empty grant
(disclosed since ADR-0044, "no real rating/quota engine") -- there was nothing to rate *against*.

**Scope, approved before implementation.** Real `ProductOffering`/`ProductOfferingPrice`
Create/Get/List/Delete (real TMF620 PATCH-for-update and the `/listener/*` event-notification
callbacks deferred -- not needed to prove the data model is real and usable) against a real,
in-memory store. Explicitly NOT: wiring CHF to actually consult this catalog when rating a charging
event (a real rating engine -- separate, larger, not-yet-approved scope), and NOT a GUI --
`PROMPT.md`'s own Phase 7 design (confirmed by re-reading it directly, in response to the user
asking for "GUIs... in each domain similar to CHF") is **one generic JSON-schema-driven console
built later**, explicitly "so new NFs need no UI code" -- not bespoke per-NF/per-domain GUIs, and
not scheduled until after Phase 5/6. The user, after this was flagged, confirmed sticking to that
documented plan rather than pulling GUI work forward or applying TM Forum SID to non-charging NFs
(AMF/SMF/PCF/AUSF's own operational configuration, if it needs a real data model, is a real 3GPP
standard -- TS 28.541 NRM/IOC -- not TM Forum SID, which is a business/billing concept with no
equivalent for those NFs; the two are named separately in PROMPT.md's own DATA MODEL line, not
interchangeable).

**Why a standalone service, not code inside `nfs/chf`.** CLAUDE.md's own stated goal -- "align to
TM Forum ODA component boundaries so the BSS layer could be swapped for a commercial stack" --
argues structurally for a real, separate component here, the same reasoning `libs/bss-sid`'s own
file header already gives for staying independent of `sbi_core`. New top-level `bss/` directory
(distinct from `nfs/`, since this is not a 3GPP Network Function -- no NRF registration, no
`Nnrf_NFManagement`, nothing in TS 23.501's NF taxonomy describes it) holds standalone,
independently-deployable BSS/ODA-layer services; `libs/bss-sid/` (existing) holds the shared DTOs/
mapping code a service like this one links against.

**Real fields, directly verified -- not trusted secondhand.** Rather than relying on the earlier
research fork's summarized field lists, this turn downloaded TM Forum's actual TMF620 v4.1.0
swagger JSON (`github.com/tmforum-apis/TMF620_ProductCatalog`) and parsed it directly with Python
before writing any DTO -- confirming exact real field names/types for `ProductOffering`,
`ProductOfferingPrice`, `ProductOfferingPriceRef`, and TM Forum's common `Money`
(`unit`/`value`)/`TimePeriod` (`startDateTime`/`endDateTime`)/`Quantity` (`amount`/`units`) types,
and confirming the real schema marks **no field required at all** on either resource -- every
`std::optional` in `libs/bss-sid/include/bss_sid/product.hpp` reflects that exactly, not a
simplification. Real paths/methods also confirmed directly: `GET`/`POST /productOffering`,
`GET`/`PATCH`/`DELETE /productOffering/{id}` (same shape for `/productOfferingPrice`), real
`basePath: /tmf-api/productCatalogManagement/v4/`.

**Security: mTLS only, no OAuth2.** Reuses this lab's existing CA (`scripts/gen-lab-pki.sh`) for
real TLS 1.3 + mTLS -- same infrastructure every 3GPP NF uses, genuine transport security, not
"none". Does NOT layer OAuth2 bearer-token verification on top the way every 3GPP NF does: there is
no NRF-issued token source for a non-3GPP ODA component, and building a parallel BSS-side OAuth2/
OIDC stack is out of scope for "data model + API first" -- disclosed as a narrower security
boundary than the 3GPP NFs have, not silently omitted.

**Live-verified, real HTTP interop, not just unit tests.** 5 new unit tests
(`tests/conformance/test_bss_sid_product.cpp`) for the DTOs (Money/TimePeriod JSON shape,
`ProductOfferingPrice` round-trip, `ProductOffering` referencing prices by id, empty-array
omission) -- 127/127 total, zero regressions. Beyond that: the live service was driven with real
mTLS `curl` calls -- created a real `ProductOfferingPrice` (5GB/month, $20, real `id`/`href`
assigned by the server), created a `ProductOffering` referencing it, listed it, retrieved it by id,
deleted it (204), confirmed a second delete correctly 404s, and confirmed a request **with no
client certificate at all fails outright** (curl reports connection failure, not just a 401) --
proving mTLS is genuinely enforced, not just configured. Both new Prometheus counters
(`product_catalog_offering_create_total`, `product_catalog_offering_price_create_total`) confirmed
via direct scrape after the real calls.

**Consequence:** Product/tariff definitions are now genuinely configurable data (a real store
behind a real API), closing the gap PROMPT.md's charging principles named. CHF still does not
consult this catalog -- that wiring is a real rating engine, a distinct, larger, not-yet-approved
scope. No GUI exists or is planned before Phase 7, per the user's explicit confirmation.

