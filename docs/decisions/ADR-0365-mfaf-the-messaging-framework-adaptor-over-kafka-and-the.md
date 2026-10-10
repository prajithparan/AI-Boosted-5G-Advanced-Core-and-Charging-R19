## ADR-0365: MFAF -- the Messaging Framework Adaptor over Kafka, and the AnLF notifier lease

**Date:** 2026-09-15. **Status:** accepted. ADR-0359 step 1. User-directed sequencing: the
NWDAF ecosystem (AnLF, MTLF, DCCF, ADRF, MFAF, "properly built with proper scalable
architecture") before the remaining Lawful Interception increments; LI resumes at ADR-0364
increment 2 afterwards.

**Sources.** TS 23.288 V19.7.0 clause 5A.3.2 and procedure 6.2.6.3.4 (Data Collection via
Messaging Framework, steps 5-10 and 14); TS 29.576 V19.7.0 clauses 4.2.2.2 (Configure: POST
create, PUT update), 4.2.2.3 (Deconfigure), 4.3.2.2 (Fetch), 4.3.2.3 (Notify), 4.4.2.2
(ContextManagement_Transfer), feature tables 5.1.8-1/5.2.8-1/5.3.8-1; the three TS 29.576 R19
YAML files, now in the codegen list (they were not: only `FetchInstruction` had ever been
generated, transitively). API roots are the YAMLs' `servers[0].url`, checked by
`api_root_conformance`.

**What the MFAF is here.** `nfs/mfaf`, the 3GPP-shaped adaptor over Apache Kafka in both
directions of TS 23.288 figure 5A.3.2-1: a Data Source's `Nnf_EventExposure_Notify` lands at the
MFAF Notification Target Address the MFAF chose, is produced to a Kafka topic keyed by the
`mfafCorreId`, and is delivered by whichever replica's consumer-group member owns the partition
as `Nmfaf_3caDataManagement_Notify` -- as `dataAnaNotif` directly, or as a `FetchInstruction`
with the payload buffered until the consumer's `Fetch`. Configuration, the correlation index and
the fetch buffers are in Valkey (`nfs/mfaf/src/config_store.hpp`), so any replica serves any
request. `libs/event-bus` is the NF-neutral librdkafka Producer (acks=all, idempotent) and
consumer-group Consumer (commit after the handler) the MFAF uses; the CHF's `CdrEventProducer`
(ADR-0355) is the same code in its own words and will migrate to it -- disclosed, not hidden.

**What the MFAF chooses, and why it may.** TS 29.576 4.2.2.2.2 has the MFAF "determine the MFAF
notification information" when the DCCF sends none, and 4.3.2.2.2 has the consumer fetch from
"the URI {fetchUri} which was previously provided by the MFAF". This implementation's are
`https://{advertised}:{port}/mfaf-inbound/v1/notifications/{mfafCorreId}` and `.../fetch`, under
a prefix no 3GPP API uses and deliberately not named `*Root` in code -- they are callbacks a
Data Source treats as opaque, not API roots.

**The inbound body has no 3GPP schema.** What a Data Source posts is its own notify type
(`AmfEventNotification`, `NsmfEventExposureNotification`, ...); TS 29.576 only defines the
outbound `NmfafDataAnaNotification`, whose `DataNotification` (TS 29.575) has one typed bucket
per source NF. `nfs/mfaf/src/classify.cpp` places the body by the `3gpp-Sbi-Callback` header's
API-name prefix (TS 29.500 5.2.3.2.3, Annex B), then by body shape where a shape is unique
(AMF, NRF, UDM, NWDAF, NSACF, UPF, GMLC, LMF). SMF, NEF, AF and PCF all serialise as
`{notifId, eventNotifs}` and are indistinguishable without the header; such an inbound is
accepted (it is on the bus), counted (`mfaf_inbound_unclassified_total`) and not delivered --
never forwarded in a guessed bucket. This project's own notifiers do not yet send
`3gpp-Sbi-Callback`; that becomes required when the DCCF subscribes them to the MFAF.

**Validation the generated types cannot do.** `MfafConfiguration` and
`NmfafDataRetrievalNotification` are `oneOf`s; the handler rejects zero-or-both with a 400
carrying a TS 29.500 cause (`MANDATORY_IE_MISSING` / `INVALID_MSG_FORMAT`). An inbound for a
deconfigured `mfafCorreId` is answered 400 `RESOURCE_CONTEXT_NOT_FOUND`, the 29.500 cause whose
definition is exactly "callback URI still exists ... resource context ... not found".

**Features.** None advertised. `MfafTransfer`: a POST with `mfafTransferInfo` is 400; the
`Transfer` operation itself is served (configs + buffered notifications handed over, local
resources removed) but this MFAF never initiates one. `DataAnaCollect` (`notifEndpoints`),
`MultiProcessingInstruction`/`DataProcess` (`procInstruct`, `multiProcInstructs`, summary
reports) and `ReportingOptions` (clubbing, windows, periodic, cross-event) are accepted, logged
as not applied, and not applied. `adrfId` is logged; ADRF storage is step 3.

**The AnLF was not scalable either, and now is.** ADR-0360 shared the subscription state across
NWDAF replicas; the periodic notifier still ran in every replica over that shared state, so a
consumer received one copy of each notification per replica. `SubscriptionStore::
claim_notification` takes a `SET NX PX` lease per subscription per interval; the replica that
wins delivers. `NwdafPhaseA.ReplicasDeliverEachNotificationOnce` runs two replicas on a 2 s
interval against a receiver and fails on any two deliveries closer than the lease. The
`main.cpp` header that still said "state is in-process" was corrected.

**Tests.** `tests/integration/test_mfaf.cpp` -- two MFAF replicas, real NRF/Valkey/Kafka: the
DCCF configures A, the source notifies B with A's URI, exactly one delivery arrives in the AMF
bucket with the callback header; consumer-triggered mode round-trips a fetch through the other
replica and a second fetch is 404; an unclassifiable inbound is accepted and never delivered;
transfer hands the configuration over and a later inbound is `RESOURCE_CONTEXT_NOT_FOUND`.
Skips (not passes) without a broker and under ThreadSanitizer (ADR-0364 CI note 3). Compose:
`mfaf` service, scalable with `--scale mfaf=N`; diagram tile now built.

**Rejected.** *An in-process delivery path when no broker is configured* -- a second code path
whose behaviour differs from the one the spec's Messaging Framework has (no durability, no
work-splitting); the MFAF refuses to start without brokers instead. *Guessing the source bucket
for `{notifId, eventNotifs}` bodies* -- a wrong bucket is data corruption the consumer cannot
detect. *A GET on the configuration* -- not in the YAML.

