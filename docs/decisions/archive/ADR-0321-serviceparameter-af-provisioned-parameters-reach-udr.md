## ADR-0321: ServiceParameter -- AF-provisioned parameters reach UDR

**Date:** 2026-09-08. **Status:** Closed (work pushed; last citing commit 1d78580 on origin/main, 2026-10-06). accepted. **Fifth** of the 57 AF-facing services.

An AF provisions service parameters the core network should apply to its traffic; UDR's
`application-data/serviceParamData` is where the 5GC keeps them. Same principle as every slice
since ADR-0302: parameters an AF provisions that never reach UDR are parameters nothing applies.

### The field mapping is an intersection, not a translation

`ServiceParameterData` (AF-facing, TS 29.522) and `ServiceParameterData_Application_Data`
(UDR-facing, TS 29.519) -- the pair whose name collision ADR-0312 had to disambiguate -- share
their identifying fields, so what is carried is exactly that intersection and nothing is
synthesised: `afServiceId`, `appId`, `dnn`, `snssai`, `externalGroupId`, `anyUeInd`, `gpsi`,
`ueIpv4`, `ueIpv6`, `ueMac`, `paramOverPc5`, `paramOverUu`.

**Deliberately not carried:** `notificationDestination`, `requestTestNotification` and
`websockNotifConfig` are NEF-side subscription machinery, not service parameters. They have no UDR
counterpart, and sending them would put NEF's own plumbing into the core network's data store.

### PUT re-provisions here, unlike the earlier slices

ADR-0315 and ADR-0316 both disclosed that PUT does not update their downstream, because PCF and UDM
modify sessions/subscriptions through different shapes that would need a second translation. That
reasoning does not apply here: UDR's resource is a plain PUT-replaceable document with the same
shape, so a replacement is simply re-provisioned. DELETE removes it, because parameters must stop
applying when the AF withdraws them.

Worth noting only because the three slices now differ, and the difference is principled rather
than accidental.

580/580 against a fully current build.

