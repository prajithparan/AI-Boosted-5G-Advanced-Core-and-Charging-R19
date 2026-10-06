## ADR-0302: NEF's first AF-facing API -- and a filter that never worked

**Date:** 2026-09-05. **Status:** accepted. **Closes:** the first of ADR-0294's 58-file finding.

ADR-0294 found that NEF's AF-facing half -- the "exposure" the NF is named for -- did not exist:
all 14 `Nnef_*` NF-facing files were built, while 58 of the 60 `TS29122_*`/`TS29522_*` AF-facing
files sat on disk unwired, and NEF's only outbound HTTP was NRF registration. This closes
`TS29522_TrafficInfluence.yaml`: **one of 58**, and the number is stated that way deliberately.

### Six operations, and a constraint the DTO cannot enforce

`/3gpp-traffic-influence/v1/{afId}/subscriptions` -- ReadAll, Create, Read, FullyUpdate,
PartialUpdate, Delete. Subscriptions are keyed by `{afId}/{subId}` so one AF cannot read or delete
another's; the resource is per-AF in the spec's own path, and a flat id namespace would quietly
make it global.

`CreateNewSubscription` validates the spec's two `oneOf` groups **in the handler** -- exactly one
of `afAppId`/`trafficFilters`/`ethTrafficFilters`/`trafficDataSets`, and exactly one of
`ipv4Addr`/`ipv6Addr`/`macAddr`/`gpsi`/`externalGroupId`/`anyUeInd`. ADR-0301 established that the
generated struct cannot enforce these (every field is `std::optional`), so if the handler does not
check them nothing does, and a subscription selecting no traffic and no target would be stored as
an influence rule that can never match anything.

### The point: it brokers

An AF's `TrafficInfluSub` becomes a real TS 29.519 `TrafficInfluData` in **UDR**, because that is
where SMF reads it from. A NEF that keeps influence data in an in-process map influences nothing.
DELETE removes the UDR record too -- an orphaned rule would keep steering traffic for a
subscription the AF believes is gone. PATCH re-pushes the *merged* subscription rather than the
patch, since UDR holds a whole record and sending only changed fields would erase the rest.

Two fields are deliberately NOT mapped, each for a real reason rather than an oversight:

- **`gpsi` and `macAddr`** have no `TrafficInfluData` counterpart -- it identifies a subscriber by
  `supi`, which the AF-facing schema does not carry (confirmed by direct read). GPSI->SUPI needs a
  UDM lookup this route does not perform, so nothing is invented.
- **`externalGroupId` is not assigned to `interGroupId`** despite the inviting name symmetry.
  External- and Internal-Group-Id are different identifier spaces and the mapping is UDM's job
  (`Nudm_SDM` GroupIdentifiers). Assigning one to the other would put an external id in an internal
  id field -- wrong in a way nothing downstream would detect.

### The bug the test found in UDR

Writing the test's read-back exposed a real, pre-existing defect in UDR's own influenceData
collection GET (ADR-0253). Its `influence-Ids` filter -- the ONE filter that route honours --
matched `doc["influenceId"]`, a field the stored documents never contain: `influenceId` is the
resource key from the URL path, not part of the real `TrafficInfluData` schema. **So the filter
silently returned an empty list for every request that used it**, while unfiltered requests worked
fine. Fixed by filtering on the store's actual key (`list_with_ids()`), not on a document field
that can never be there.

This is the second time in two ADRs that writing an honest cross-component assertion found a defect
neither component's own tests could see. UDR's tests exercised the filter's code path; nothing
checked that it ever matched.

### Two things I got wrong first, recorded because the pattern repeats

1. I mapped `supi` and `gpsi` between the two schemas **without checking either struct**. The
   compiler caught both. The corrected mapping carries only fields that genuinely exist on both
   sides.
2. The test's first read-back asserted an individual `GET .../influenceData/{influenceId}` and
   failed with 404 **while the broker was working perfectly**. TS 29.519 defines only `put` on that
   resource -- there is no individual GET, and UDR correctly does not invent one. The test was
   wrong, not the code; the collection GET with `influence-Ids` is the real read path, which is
   what then exposed the filter bug above.

### Still not done, so this is not read as "NEF is finished"

- **57 of 58 AF-facing files remain unwired**, and each needs a FULL rebuild because of the type
  collisions ADR-0301 documented.
- **No PCF path.** free5GC routes AF-session-specific requests to PCF as an `AppSessionContext` and
  provisioned ones to UDR, chosen by which fields the request carries. The UDR path is implemented
  properly rather than half-wiring both, because getting that split wrong would silently send
  session-scoped requests to the wrong place.
- **No notifications.** `subscribedEvents`/`notificationDestination` are accepted and stored; NEF
  never delivers an event to the AF. UDR's `onDataChange` webhook infrastructure (ADR-0171 onward)
  is the natural source when that is built.

