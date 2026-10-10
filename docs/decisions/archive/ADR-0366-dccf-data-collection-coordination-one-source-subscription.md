## ADR-0366: DCCF -- data collection coordination, one source subscription per data spec, fanned out through the MFAF

**Date:** 2026-09-15. **Status:** Closed (work pushed; last citing commit e5c1d09 on origin/main, 2026-10-06). accepted. ADR-0359 step 2.

**Sources.** TS 23.288 V19.7.0 clause 5A.2 (coordination: "determine whether the data are
already being collected"), 5A.3.2 (delivery via a Messaging Framework), procedure 6.2.6.3.4
steps 1-14; TS 29.574 V19.7.0 clauses 4.2.2.2 (Subscribe, analytics and data, create and
update), 4.2.2.3 (Unsubscribe), 4.2.2.4-6 (Notify, Fetch, Transfer), 4.3.2.2-4 (ContextManagement
Register/Update/Deregister), application errors table 5.1.7.3-1, features table 5.1.8-1; its two
R19 YAML files, now in the codegen list (`DataSubscription` and the notification DTOs had been
generated transitively from TS 29.575; the subscription resources had not).

**What the DCCF is here.** `nfs/dccf`. A consumer's `NdccfDataSubscription` /
`NdccfAnalyticsSubscription` is fingerprinted on its source-facing part (the source subscription
with its notification target and correlation fields removed, canonical JSON, FNV-1a) -- the
DCCF's own definition of "the same data", since TS 23.288 leaves the matching logic unspecified.
A fingerprint with no collection yet: the DCCF configures the MFAF (Nmfaf_3daDataManagement
Configure, one `MessageConfiguration` carrying the consumer's `dataNotifUri`/`dataNotifCorrId`),
takes the `mfafNotiInfo` the MFAF assigned, and subscribes the Data Source with the MFAF's
address as the notify target and the `mfafCorreId` as the correlation. A fingerprint with a
collection: the consumer is appended to that MFAF configuration (PUT); the source is not
touched. Unsubscribe reverses it; the last consumer's departure unsubscribes the source and
deconfigures the MFAF (steps 12-14). Consumer subscriptions, collections and profiles live in
Valkey (`nfs/dccf/src/subscription_store.hpp`); the DCCF holds no state and any replica serves
any request. Delivery is the MFAF's `Nmfaf_3caDataManagement_Notify` -- "the same content as
those sent via a Ndccf_DataManagement service" (5A.3.2).

**Data Sources served.** The ones this project has producers for: AMF (`Namf_EventExposure`),
NRF (`Nnrf_NFManagement` NFStatusSubscribe), NSACF (`Nnsacf_SliceEventExposure`); and the NWDAF
(`Nnwdaf_EventsSubscription`) for analytics subscriptions. `smfDataSub`, `udmDataSub`,
`nefDataSub`, `afDataSub`, `upfDataSub`, `gmlcDataSub`, `lmfDataSub`, `pcfDataSub` are answered
`400 SUBSCRIPTION_CANNOT_BE_SERVED` -- TS 29.574's own cause for "cannot ... construct
[a subscription] based on the received subscription contents". Peers are located by config base
URLs (`config/dccf.json`), the pattern `nfs/smf` uses for its AMF/PCF; `targetNfId`/`targetNfSetId`
are accepted and logged, not used for selection.

**User consent.** TS 29.574 4.2.2.2 conditions the Nudm_SDM consent check on "local policy and
regulations". `user_consent_policy` in config: `consumer-checked` rejects a per-UE subscription
(`supi`/`gpsi` in the source subscription) that lacks `checkedConsentInd` with
`403 USER_CONSENT_NOT_GRANTED`; `not-enforced` accepts. The DCCF-side lookup of user-consent
subscription data at the UDM, and the subscription to its changes, are **not built** -- this
project's UDM/UDR carry no user-consent data yet. Disclosed, not hidden behind the policy knob.

**Not built, disclosed.** Features: none advertised (UserConsent, DataAnaCollect, EnhDataMgmt).
`/transfer-data-sub` is not registered (a 404, not an invented status). DCCF-side Fetch and
direct Notify (the MFAF serves both). `notifEndpoints`, `storeInd`/`storeHandl`, `adrfId`,
`timePeriod`, `immReport`: accepted and logged, not applied (ADRF is step 3). Formatting and
processing instructions are handed to the MFAF, which applies what ADR-0365 says it applies. A
PUT that changes the fingerprint leaves the old collection and joins/opens the new one under
the same `subscriptionId`. Our own NRF/NSACF notifiers still send no `3gpp-Sbi-Callback`; the
MFAF places their bodies by shape (`nfInstanceUri`; `report` + `notifyCorrelationId`), which is
why the SMF-type sources are not served yet -- their bodies are shape-ambiguous.

**Two dependencies worth naming.** NRF collections carry no correlation in the notification
body (TS 29.510 `NotificationData` has none), so they rely entirely on the MFAF's inbound URI
carrying the `mfafCorreId` in its path (ADR-0365) -- a change to that URI shape would break NRF
collections and nothing else. And `Nmfaf_ContextManagement_Transfer` drains buffers with GETDEL,
so a `Fetch` racing a transfer on another replica finds one of them empty; atomic, never
duplicated, but the loser gets a 404.

**Tests.** `tests/integration/test_dccf.cpp` with real NRF, MFAF, NSACF, Valkey, Kafka: the NRF
as Data Source (subscribe through the DCCF, spawn an NWDAF, its `NF_REGISTERED` reaches the
consumer through the MFAF in the `nrfEventNotifs` bucket); NSACF with two consumers -- one
source subscription, one slice admission delivered to both; the first consumer's unsubscribe
leaves the other served, the last one's removes the source subscription and the MFAF
configuration (a further admission reaches nobody); the refusals (`SUBSCRIPTION_CANNOT_BE_SERVED`,
`USER_CONSENT_NOT_GRANTED`). Skips without a broker and under TSan (ADR-0364 CI note 3).

**Rejected.** *Direct delivery from the DCCF as a second path* -- ADR-0359's one-path rule; the
MF path is the scalable one and the spec says the content is the same. *Serving every
`DataSubscription` alternative by forwarding blind* -- a subscription to a source with no
producer here would be accepted and never deliver; the spec's own error exists for this.
*Fingerprinting the whole request* -- two consumers with different notification targets would
never share a source subscription, defeating 5A.2.

