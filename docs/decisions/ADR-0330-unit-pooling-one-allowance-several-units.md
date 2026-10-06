## ADR-0330: unit pooling -- one allowance, several units

**Date:** 2026-09-10. **Status:** accepted. Completes the voice+data product.

ADR-0309 made a voice+data bundle a single product for **money**: one offering price whose
`ratingGroup` is an array, so both kinds of traffic debit the same balance. It explicitly did not
pool a single *unit* allowance -- "10 GB usable as data or as minutes" -- and the reason given was
that converting minutes to octets needs a rate no specification defines.

That was right about the specification and wrong about the conclusion. **3GPP does not define the
rate because it is not 3GPP's to define.** It is a commercial term the operator sets, exactly like
the price. So it becomes catalog data, carried on the offering price as a `unitPooling`
characteristic, and this project supplies none of its own:

```json
{"name": "unitPooling",
 "value": {"octetsPerSecond": 87381, "octetsPerServiceUnit": 1000000}}
```

Voice seconds and service units convert into their octet equivalent and draw against the single
volume allowance. Each factor is optional: absent means that traffic does not draw on the pooled
allowance, so an offering with no `unitPooling` characteristic behaves exactly as before.

### Three decisions worth naming

**The rate is pinned at Create, not re-read at Release.** `ChargingDataStore::set_unit_pooling`
records it against the session when the reservation is made. Looking the catalog up again at
Release would apply a tariff edited mid-session retroactively to traffic already carried at the
old rate.

**Converted dimensions are zeroed.** Once seconds and service units have been folded into the
pooled volume, they must not also proportion on their own axis -- that would charge the same
traffic twice. `proportional_debit` deliberately never sums the three dimensions, because octets,
service units and seconds are not commensurable. They become commensurable only when an operator
declares an exchange rate, which is precisely what this characteristic is.

**Unusable rates are refused, not coerced.** Zero, negative, non-finite and non-numeric factors
are all rejected. A negative rate would turn consumption into a refund; a zero rate makes a typo
indistinguishable from a deliberate giveaway. Neither surfaces as an error anywhere downstream, so
it has to be caught at parse time.

The conversion is one-way: other units convert INTO the pooled volume, never the reverse. A bundle
is sold as "10 GB, and voice draws from it at this rate", not as a convertible currency pair.

### Direction

`set_unit_pooling` uses `hset`, not `hincrbyfloat` like the `granted_*` fields beside it. Those
accumulate across quota re-authorisations; a pooling rate is a property of the product the session
was sold under and must not double when a second Update carries the same value.

589/589.

