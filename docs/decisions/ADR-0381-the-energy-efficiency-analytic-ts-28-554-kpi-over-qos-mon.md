## ADR-0381: The energy-efficiency analytic -- TS 28.554 KPI over QOS_MON, exposed as a metric

**Date:** 2026-09-19. **Status:** accepted (user-directed, "then Energy-efficiency"). Builds the
path ADR-0380 left as a pending decision, the last of CLAUDE.md's three named R19 AI/ML additions.

**The constraint, re-verified against the frozen R19 YAML before building.** There is genuinely no
per-slice energy *or* volume telemetry on the 5G service-based interface:
- Nnwdaf_EventsSubscription has **no** energy `NwdafEvent` (confirmed: the enum carries none).
- The SMF `ENERGY_USAGE_DATA` event exists in `TS29508_Nsmf_EventExposure.yaml`, but its
  `EventNotification` has **no energy-magnitude field** (`usageInfo` is `IpAddrUsageInfo`, IP usage
  -- not energy). So a faithful SMF energy notification cannot carry a real consumption value.
- The only real energy datum in any in-scope YAML is CHF `NSPAContainerInformation`.
  `estimatedEnergyConsumption` (TS 32.291), but nothing in the lab produces NSPA charging.
- The Doris `cdr` corpus has volume but no S-NSSAI column (only `rating_group`); QOS_MON carries
  per-slice delays but, until now, no volume.

**Decision.** Compute the **TS 28.554 clause 6.7.1 Energy-Efficiency KPI** -- data volume divided by
energy consumption, per S-NSSAI -- at the NWDAF, and expose it as an **OAM-style Prometheus metric**
(`nwdaf_slice_energy_efficiency_bit_per_joule`, labelled by verbatim S-NSSAI), NOT as an Nnwdaf
analytics response (there is no eventId to answer on). Two real, buildable pieces feed it:
1. The SMF QOS_MON producer now also emits `ulDataRate`/`dlDataRate` -- **real TS 29.508 `BitRate`
   fields** -- giving a per-slice volume signal. Values are synthesized deterministically per tick,
   exactly as the delays already are (disclosed lab data; no real UPF throughput meter exists).
2. `nwdaf::energy_efficiency()` (analytics.cpp) means the collected rates over the window to a
   volume and divides by a **disclosed configurable power model** (`energy_static_watts_per_slice`,
   `energy_joules_per_gigabyte` in config) standing in for the TS 28.552 PEEC measurement.

**Why this is faithful, not fabricated.** The KPI formula (TS 28.554), the volume fields (TS 29.508
`ulDataRate`/`dlDataRate`), and the metric surface (OAM, per TS 23.288 6.16's routing of energy to
OAM) are all real spec artefacts. Only the *values* of the rate samples and the energy model are
lab synthesis, and both are disclosed -- the same discipline as SERVICE_EXPERIENCE's latency->MOS
mapping (ADR-0379) and ADR-0337's synthetic offered load driving the real charging engine. No API
field, eventId, or TS number is invented. Every S-NSSAI is grouped and echoed verbatim (any
standardised or operator-specific SST, any SD), per the generic-attribute rule.

**Disclosed / not conformant.** The energy term is a modelled stand-in, not a measurement: the
absolute EE figure is only as real as the power model, and a production deployment replaces the
model with a TS 28.552 PEEC feed from OAM. The volume is a mean-rate * window approximation of the
window's traffic, not an integrated byte count. The metric is computed on scrape over the QOS_MON
collection window, so it reflects only what QOS_MON subscribers are currently feeding.

**Rejected.** *An Nnwdaf energy analytic* -- no eventId/DTO exists; would be fabrication (#1 rule).
*Deriving volume from the CDR corpus* -- the `cdr` table has no S-NSSAI, so it cannot be grouped per
slice. *Building the CHF NSPA `estimatedEnergyConsumption` producer+consumer* -- a real path, but a
full subsystem (producer and consumer for a container nothing emits) beyond this increment; recorded
as the production-grade enhancement path.

