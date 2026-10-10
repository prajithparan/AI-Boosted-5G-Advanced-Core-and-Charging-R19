## ADR-0316: MonitoringEvent -- two enums and two identifier spaces that do not line up

**Date:** 2026-09-07. **Status:** accepted. **Third** of the 57 AF-facing services.

TS 29.122's MonitoringEvent is how an AF asks to be told when a UE becomes reachable, loses
connectivity, changes location or roams. It brokers to UDM's `Nudm_EE` ee-subscriptions, which is
what actually reports those events.

The CRUD is the same shape as the two slices before it. The substance is in the two translations
between the AF's vocabulary and the core network's, both of which are places a careless mapping
would silently monitor the wrong thing.

### The event-type enums overlap but differ

- `CHANGE_OF_IMSI_IMEI_ASSOCIATION` (TS 29.122, EPC vocabulary) is
  `CHANGE_OF_SUPI_PEI_ASSOCIATION` in `Nudm_EE` (5GC vocabulary). Same event, different era.
- **`UE_REACHABILITY` is one AF value but two EE values** -- `UE_REACHABILITY_FOR_DATA` and
  `UE_REACHABILITY_FOR_SMS`. Rather than guess, the AF schema's own `reachabilityType` (SMS|DATA)
  resolves it; absent, DATA is used, and that default is stated in the code rather than implied.
- The remaining five are name-identical and mapped 1:1.
- A `monitoringType` with no EE counterpart is **rejected with 400**, not stored. A monitoring
  subscription that can never report is worse than an explicit refusal, because the AF believes it
  is being watched.

### The identifier spaces differ too

The AF identifies a UE by external identifier (`externalId`, an NAI) or `msisdn`; `Nudm_EE`'s path
takes a GPSI. An external id IS a GPSI in `extid-` form and an MSISDN is one in `msisdn-` form, so
this is a formatting rule with a spec basis, not an invented mapping.

**Group subscriptions (`externalGroupId`) are not expanded.** `Nudm_EE` subscribes per UE, and
expanding a group needs a membership lookup this route does not perform. Such a request is stored
at NEF and logged as NOT brokered -- deliberately not treated as though it were monitoring
someone.

### Lifecycle

Create records the resulting `{ueIdentity, subscriptionId}` pair; DELETE removes the UDM
ee-subscription, so it cannot outlive the AF request and keep pushing events to a callback for a
subscription that no longer exists. UDM reports to NEF, which is what will notify the AF -- the same
reason ADR-0315 points PCF at NEF rather than at the AF.

PUT does not update the UDM subscription, disclosed exactly as in ADR-0315: `Nudm_EE` modifies via
its own PATCH shape, and approximating it risks a subscription reporting a different event than the
AF now asks for.

### Still absent across all three AF slices

NEF accepts `notificationDestination` and stores it, but **delivers no events to AFs**. UDM and PCF
will call NEF's callback URIs; nothing yet forwards those onward. That is one piece of work shared
by every AF-facing service rather than per-service, and it is the natural next increment.

580/580 against a fully current build.

