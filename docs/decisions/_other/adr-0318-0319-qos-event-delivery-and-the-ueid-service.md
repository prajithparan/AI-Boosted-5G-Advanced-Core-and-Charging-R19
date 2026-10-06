## ADR-0318/0319: QoS event delivery, and the UEId service

**Date:** 2026-09-08. **Status:** accepted. Slices four and a half of the 57 AF-facing services.

### ADR-0318: QoS events reach the AF

ADR-0315 told PCF to notify NEF; ADR-0317 built delivery for monitoring. The QoS callback had no
route, so PCF's events reached NEF's door and stopped. Same mechanism as ADR-0317, second event
source: correlation by URI (`/nnef-callback/v1/qos-notify/{afId}/{subscriptionId}`), 404 when the
AF has deleted its subscription so PCF stops rather than reporting to nobody, and the notification
rebuilt in the AF's own TS 29.122 shape with PCF's `EventsNotification` carried under
`_npcfEventsNotification` rather than re-modelled.

### ADR-0319: UEId -- two halves, one real and one honestly refused

The file splits cleanly, and the split is the point.

**Six provisioning operations are real and complete**: `UeIdMappingInfo` is a ProSe/Ranging
application-layer-id <-> GPSI pair, store-backed CRUD per AF.

**Three lookups (`/retrieve`, `/get-msisdn`, `/verify-msisdn`) answer 404**, which the real YAML
defines for all three. They ask "who is the UE at this IP address" and answering needs an
IP-to-identity source -- a UPF session lookup or an H-NEF mapping database -- that this project
does not have. The NF-facing twin (`nnef-ueid/v1/fetch`) already disclosed exactly this and answers
204.

`/verify-msisdn` is the one worth dwelling on: the obvious "implementation" is
`verifResult: false`. That is not a null answer, it is an **assertion that the MSISDN does not
belong to that UE** -- and an AF acting on a false negative could deny service to a legitimate
subscriber. The truth is that nothing here can determine it, so 404 with a ProblemDetails saying
the MSISDN can be "neither confirmed nor denied" is the only honest response.

### A misreading caught before it became code

The initial plan was for the provisioning operations to populate the lookups -- an AF provisions
identity mappings, the lookups resolve against them. Reading the actual schemas killed it:
`UeIdMappingInfo` carries `rsUeIdMappingInfo` (application-layer id + GPSI), which is a **different
mapping** from the IP-to-identity one the lookups need. Building it would have produced a service
that looked joined-up and answered the wrong question.

580/580 against a fully current build.

