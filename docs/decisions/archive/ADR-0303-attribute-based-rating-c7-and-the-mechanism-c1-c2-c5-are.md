## ADR-0303: attribute-based rating -- C7, and the mechanism C1/C2/C5 are expressed in

**Date:** 2026-09-05. **Status:** Closed (work pushed; last citing commit cc748ae on origin/main, 2026-10-06). accepted. **Implements:** ADR-0300's C7, first item in its
revised order.

**User direction:** *"10GB is usable on Slice ID 1 OR 5GB on Slice ID 10 OR UPF ID 5. Such model
MUST be supported. Ideally any attribute coming to CHF on N40/N28 shall be used for Charging and
model product."*

That is broader than the three slice models ADR-0300 originally offered to choose between, and it
supersedes them. The requirement is not "S-NSSAI as a rating dimension" -- it is that the rating
decision see **the request**, not just its `ratingGroup`.

### The scope is catalog data, never code

An offering price carries a `chargingScope` characteristic in its TMF620 `prodSpecCharValueUse` --
the same extension mechanism `ratingGroup` and the quota-policy fields already use. Principle P7
requires this: hardcoding slice ids in C++ would make every new product a code change.

```json
{"sNSSAI": {"sst": 1, "sd": "000001"}}                          10 GB on slice 1
{"sNSSAI": {"sst": 10, "sd": "00000a"}}                         5 GB on slice 10
{"uPFID": "upf-5"}                                              tied to one UPF
{"sNSSAI": [{"sst":1,...},{"sst":10,...}], "dnnId": "internet"} one bundle, two slices, one DNN
```

Semantics, chosen to be the least surprising rather than the most powerful:

- **Every key must hold.** Two constraints means both -- a product scoped on slice *and* UPF
  applies only where both match.
- **An array means any-of**, which is what makes "usable on slice 1 OR slice 10" a single product
  rather than two catalog entries that must be kept in sync.
- **An attribute the request does not carry fails closed.** Matching an offering scoped to
  something the request never stated would charge a subscriber against a product that does not
  apply to them.
- **An absent or empty scope constrains nothing.** Every offering configured before this existed
  keeps behaving exactly as it did, and a test pins that specifically -- it is the backward
  compatibility guarantee, not an implementation detail.

### The attributes are the spec's, named as the spec names them

`collect_charging_attributes()` flattens the real TS 32.291 request: `ratingGroup` and `uPFID` from
`MultipleUnitUsage`; `sNSSAI`, `dnnId`, `ratType`, `chargingCharacteristics`, `hPlmnId`,
`servingCNPlmnId`, `pduSessionID` from `PDUSessionChargingInformation.pduSessionInformation`;
`subscriberIdentifier` and `tenantIdentifier` from the request itself. Names are taken verbatim
from the generated DTOs so a scope written against the specification matches without a translation
table -- and each was confirmed by direct read, after an earlier draft of this function referenced
an `accessType` field that PDUSessionInformation does not have (the compiler caught it; the field
list I had read spanned a neighbouring struct).

### Why this came before C1, C2 and C5

ADR-0300 originally scheduled C7 last. That was wrong once the requirement became attribute-based:

- **C5 roaming rating is a scope over `hPlmnId` vs `servingCNPlmnId`** -- not a separate feature. A
  test in this ADR already demonstrates it.
- **C1 group scoping** and **C2 time-denominated grants** both plug into the same matching.

Building any of them first would have meant building them twice.

### Disclosed: Diameter Gy and CAP match only unscoped offerings

Both call `charge_one_usage` with no attributes, because neither has a TS 32.291 request to draw
them from -- RFC 4006 CCR and CAP InitialDP carry different information. They therefore match only
offerings with no `chargingScope`. That is the honest outcome rather than a fabricated slice
context, but it has a real product consequence: **a slice- or UPF-scoped product is N40-only
today.** Extending it means mapping Gy/CAP fields onto the same attribute names, which is real work
and is not claimed here.

### Evidence

`charging_scope_matches` lives in its own translation unit for the same reason
`proportional_debit` does (ADR-0297): it is pure predicate logic, and a test for it should not link
the HTTP/2 client, CDR writer and ONNX quota sizer that `charging_engine.cpp` pulls in. Seven tests
state the cases as product statements -- slice-scoped, multi-slice, UPF-scoped, all-constraints,
fail-closed, roaming, and the unscoped compatibility guarantee. 542/542 against a verified-current
build.

