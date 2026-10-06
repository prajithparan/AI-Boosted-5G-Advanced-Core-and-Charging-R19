## ADR-0324: eight more AF-facing services, and two of them have no PATCH

**Date:** 2026-09-09. **Status:** Closed (work pushed; last citing commit f1fedd2 on origin/main, 2026-10-06). accepted. Thirteenth through twentieth of the 57.

DeviceTriggering, NpConfiguration, RacsParameterProvisioning, ReportingNetworkStatus and
ResourceManagementOfBdt (TS 29.122), plus AnalyticsExposure, ASTI and ImsEventExposure
(TS 29.522) -- 46 operations.

### The method set comes from each YAML, not from the pattern

The twelve services before these settled into a six-operation shape, and it would have been quick
to stamp that shape eight more times. Two of these do not have it: **AnalyticsExposure and ASTI
define no PATCH operation**, so none is routed for them. A sixth route added for symmetry would
answer a method the specification does not define, and an AF discovering it would be discovering
something this project invented. `Bdt`'s DTO also lives in `TS26510_CommonData_grp.hpp`, not in
its own service header -- read from the generated tree rather than assumed.

That is the whole reason the method sets were re-read per service instead of reused: the pattern
was right ten times out of twelve, which is exactly the failure mode that makes a pattern feel
safe enough to stop checking.

### Downstream: none of the eight

Each of these has a real destination that this project either has not built or has not wired:

- **AnalyticsExposure** -> NWDAF, which is Phase 5 and does not exist yet.
- **ReportingNetworkStatus** -> NWDAF congestion analytics, same.
- **ResourceManagementOfBdt** -> PCF's background-data-transfer policy (PCF exists; the BDT
  policy path does not).
- **DeviceTriggering** -> an SMS/T8 device-trigger delivery path this project does not have.
- **NpConfiguration**, **RacsParameterProvisioning**, **ASTI**, **ImsEventExposure** -> UDM/UDR
  provisioned data and the IMS domain, none of it wired.

They are accepted, validated against their real generated DTOs, and stored at NEF. Nothing in the
core changes behaviour because an AF called one of them.

Running total: **20 of 57 services, 5 with a real downstream** (TrafficInfluence -> UDR,
AsSessionWithQoS -> PCF, MonitoringEvent -> UDM, ServiceParameter -> UDR, IPTV -> UDR) and 15
accepted-and-stored. The gap between those two numbers is now the larger part of the AF surface,
and it widens with every batch like this one -- the northbound API is being completed ahead of the
southbound wiring, deliberately, but it should not be mistaken for exposure that works end to end.

