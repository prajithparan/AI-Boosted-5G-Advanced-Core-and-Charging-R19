## ADR-0323: the four `/pp` parameter-provisioning services

**Date:** 2026-09-09. **Status:** Closed (work pushed; last citing commit 78e993c on origin/main, 2026-10-06). accepted. Ninth through twelfth of the 57 AF-facing services.

CagInfoParamProvision, AddressingParamProvision, SliceParamProvision and
GroupParametersProvisioning -- 24 operations, one shape, one increment.

### A path difference worth not papering over

Every AF service before these scopes its collection per-AF (`/{afId}/subscriptions`). **These four
do not**: their collections are a flat `/pp`, with no `{afId}` segment anywhere in the spec.

So they share one namespace, because that is what the paths say. It would have been easy to invent
a per-AF scoping to match the earlier services -- the store already supports it -- but that would
give the resources an isolation property TS 29.522 does not define, and an operator relying on it
would be relying on something this project made up. An AF wanting per-AF isolation here needs it in
the specification first.

### Downstream: none, and that is the honest half

These provision parameters that belong in UDM's `Nudm_PP` (implemented here, ADR-0237) or UDR's
provisioned data. **Wiring each to its correct destination is per-service work** -- CAG information,
static IP addressing, slice parameters and group data land in different places -- and it is not
done. They are accepted, validated against their real generated DTOs, and stored at NEF.

Running total after this batch: **12 of 57 services, 5 with a real downstream** (TrafficInfluence
-> UDR, AsSessionWithQoS -> PCF, MonitoringEvent -> UDM, ServiceParameter -> UDR, IPTV -> UDR) and
7 accepted-and-stored. The count of "services implemented" and the count of "services that affect
the network" are different numbers, and both are stated whenever this progresses.

580/580 against a fully current build.

