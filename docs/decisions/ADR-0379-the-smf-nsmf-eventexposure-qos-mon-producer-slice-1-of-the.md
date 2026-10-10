## ADR-0379: The SMF Nsmf_EventExposure QOS_MON producer -- slice 1 of the NWDAF SERVICE_EXPERIENCE data path

**Date:** 2026-09-19. **Status:** accepted. First slice of NWDAF completion: giving the
SERVICE_EXPERIENCE analytic (TS 23.288 clause 6.4) a real input.

**Problem.** The mandated third NWDAF analytic (slice-SLA / service experience) had no input data.
The NWDAF's only feature source is `subscriber_features` (per-subscriber session *volume*) -- no
slice, no QoS, no MOS. TS 23.288 6.4 sources service experience from AF-reported `SvcExperience`
(`Naf_EventExposure`) and per-slice QoS; investigating the R19 YAML, the tractable source in this
project is the SMF `Nsmf_EventExposure` **QOS_MON** SmfEvent (TS 29.508 EventNotification: `snssai`
+ `ulDelays`/`dlDelays`/`rtDelays`). But the SMF implemented `Nsmf_EventExposure` as a
subscription-only shell (ADR-0201 disclosed "no real event notification delivery exists"). So
before any analytic, the SMF has to become a real producer.

**Decision.** `nfs/smf/src/qos_mon_producer.{hpp,cpp}` + a background emitter thread in the SMF.
A pure `build_qos_mon_notifications(subscriptions, sm_contexts, tick, timestamp)` turns the live
subscriptions and SM contexts into `NsmfEventExposureNotification`s: for each subscription that
subscribes to `QOS_MON`, one notification per live SM context matching the subscription's optional
per-event S-NSSAI filter, carrying a `QOS_MON` `EventNotification` with the session's SUPI, its
S-NSSAI, and synthesized `ul/dl/rtDelays`. The emitter thread (its own HTTP/2 client) POSTs each to
the subscription's `notifUri` every `qos_mon_interval_seconds` (config, default 30; 0 disables and
restores the old subscription-only behaviour). `EventSubscriptionStore`/`SmContextStore` gained a
`snapshot()` accessor for the emitter to iterate.

**Generic by construction (user directive).** Every S-NSSAI is matched and echoed **verbatim** as
an opaque JSON value -- a standardised SST (1 eMBB / 2 URLLC / 3 MIoT) and an operator-specific one
(128-255) with or without an SD are all handled identically, never coerced to or filtered against a
known-slice allow-list. Building the notification as JSON (rather than through a fixed DTO struct)
is what makes that round-trip exact. The same principle applies to every attribute the NWDAF path
carries.

**Disclosed.** The per-flow latency is **synthesized** (deterministic per session+tick) -- this
project has no real UPF QoS-monitoring measurement, so the numbers are lab data, disclosed. The
subscription matching, the notification shape (conformant to TS 29.508), and the mTLS delivery are
real. The producer does not attach an OAuth2 token to the notification POST yet (lab simplification).

**Verified.** `smf` builds and links with the emitter; `conformance_tests` `SmfQosMon.*` (4 tests)
pass, covering standard + custom slices echoed verbatim, exact per-slice filter matching, the
skip cases (non-QOS_MON / no notifUri / no matching session), and deterministic delays.

**Next (NWDAF completion):** slice 2 -- the NWDAF subscribes to the SMF's `QOS_MON` and collects
the reports; slice 3 -- the `SERVICE_EXPERIENCE` analytic aggregates per-slice latency into
`svcExprc`. Then energy-efficiency (the SMF `ENERGY_USAGE_DATA` SmfEvent exists) and the VFL hook.

