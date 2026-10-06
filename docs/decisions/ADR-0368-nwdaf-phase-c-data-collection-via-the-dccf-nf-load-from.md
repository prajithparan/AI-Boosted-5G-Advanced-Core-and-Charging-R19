## ADR-0368: NWDAF Phase C -- data collection via the DCCF, NF_LOAD from observations, Nnwdaf_DataManagement

**Date:** 2026-09-15. **Status:** accepted. ADR-0359 step 4.

**Sources.** TS 23.288 V19.7.0 clauses 6.2.6.2 (data collection from an NWDAF), 6.2.6.3.2 and
6.2.6.3.4 (collection via the DCCF, delivery via the Messaging Framework), 6.5.2 (NF load
analytics input: NF load and NF status from the NRF); TS 29.520 V19.7.0 clause 4.4
(Nnwdaf_DataManagement: Subscribe 4.4.2.2.2/.3, Unsubscribe 4.4.2.3.2, Notify 4.4.2.4.2, Fetch
4.4.2.5.2) -- fetched to `specs/3gpp/TS_29.520_j70.txt` for this increment -- and
`TS29520_Nnwdaf_DataManagement.yaml` (root `/nnwdaf-datamanagement/v1`, callback key
`myNotification`); TS 29.500 Annex B for the callback name.

**The AnLF as a data consumer.** At startup one NWDAF replica subscribes NRF NF-status data at
the DCCF (`Ndccf_DataManagement`, `nrfDataSub`) with `{self}/nwdaf-inbound/v1/notifications/nrf`
as the target; the MFAF's `NmfafDataRetrievalNotification` lands there and every
`nrfEventNotifs` element is appended to a Valkey sorted set keyed by receipt time
(`nfs/nwdaf/src/collection_store.hpp`), trimmed to `data_collection.observation_window_seconds`
and `max_collected_events`. Which replica holds the collection is a `SET NX` record plus a
heartbeat key the holder refreshes every `holder_heartbeat_seconds`; a replica that finds the
heartbeat lapsed deletes the dead holder's DCCF subscription and takes over, and a graceful
shutdown releases it -- so the DCCF, as the last consumer leaves, unsubscribes the NRF and
deconfigures the MFAF (verified in the test's own log). Every replica reads the same timeline.

**NF_LOAD from observations.** TS 29.520's `NfStatus` is "the percentage of time spent on
various NF states"; ADR-0358 could only report 100% in the current status from a single
discovery. `nwdaf::nf_load` (analytics.hpp) now takes the NRF snapshot AND the collected
observations: an instance with history inside the window gets time-weighted registered /
unregistered / undiscoverable shares (from its first observation or the window start), the mean
and peak of the loads its profiles reported, and is reported even after it left the NRF -- the
integration test deregisters an NSACF at the NRF and reads a registered share below 100 with an
unregistered share present. An instance never observed keeps the snapshot's single observation.
Unit-tested (`test_nwdaf_analytics.cpp`).

**Nnwdaf_DataManagement.** Subscribe (POST → 201 + Location, PUT → 200) with `notificURI` +
`notifCorrId` and oneOf `anaSub`/`dataSub`; served from the NRF collection when the request is
`nrfDataSub` or an `anaSub` whose events are NF_LOAD (the collected data is that analytic's
input); anything else is 400 `SUBSCRIPTION_CANNOT_BE_SERVED` -- TS 29.520 4.4.2.2.2 NOTE 1's
exact case ("can neither find an existing subscription to a data source nor construct one").
Per-UE `tgtUe` without `checkedConsentInd` is 403 `USER_CONSENT_NOT_GRANTED` under the DCCF's
consent policy knob. A `timePeriod` delivers the historical slice right after the response; every
later arrival is notified as it lands; `formatInstruct.consTrigNotif` buffers the notification
and sends `fetchInstruct` whose ids are redeemed once at `{self}/nwdaf-inbound/v1/fetch` (POST,
the YAML's callback shape). Notifications carry `3gpp-Sbi-Callback:
Nnwdaf_DataManagement_myNotification`; the EventsSubscription notifier now sets
`Nnwdaf_EventsSubscription_myNotification` too (the follow-up ADR-0365 named).

**ADRF completed.** A `dataSub` storage subscription with an NWDAF `targetNfId` is served by
Nnwdaf_DataManagement (ADR-0367's disclosed refusal lifted); the NWDAF's own
`SUBSCRIPTION_CANNOT_BE_SERVED` is passed through. Proved end to end: NRF → DCCF → MFAF → NWDAF
→ Nnwdaf_DataManagement → ADRF → Doris.

**Deviation from ADR-0359's plan, recorded.** ADR-0359 step 4 said Phase C "retires the
feature_store.cpp shortcut". It does not. ABNORMAL_BEHAVIOUR's inputs per 6.7.5.2 are UE
behaviour from AMF/SMF/AF; this project's AMF stores Namf_EventExposure subscriptions but never
fires a notification, and UPF/SMF event exposure is not built, so nothing for that analytic can
flow through the DCCF yet. CHF usage is the only real per-UE data in the lab, and the analytic
computed from it is real. Keeping it, with this paragraph, beats a Phase C that pretends. The
retirement moves to whichever increment makes the AMF's event exposure fire.

**Disclosed, not built.** `dataReports` (processing summaries), muting and the rest of
EnhDataMgmt, `delAlert`, `immReport` / DataAnaCollect, `terminationReq`, the Nudm_SDM consent
lookup, `adrfId`/`storeHandl` (accepted, logged). One `subscrCond` per NRF subscription: the
first configured type is subscribed, the rest logged. Features advertised: none. Seen in the
test's teardown, after every assertion, not attributed yet: one of the spawned NFs aborts with
`terminate called after throwing an instance of 'std::system_error'` -- `what(): Owner died` in
one run, `Invalid argument` in another -- each time right after `hello-nf` finishes its own
lifecycle and exits. A standalone NSACF SIGTERMed after an NRF was up exits 0, so it is not the
NSACF's shutdown; which process it is remains to be caught with a core file.

**Follow-up named.** The DCCF keeps a source subscription across an NRF restart that lost it
(NRF subscriptions are in-process and carry a `validityTime` the DCCF neither renews nor
re-creates); surfaced when two runs of this test straddled an NRF restart. A DCCF-side liveness
check / renewal is the fix, not in this increment.

**Tests.** `tests/integration/test_nwdaf_phase_c.cpp` (2 tests, real NRF/MFAF/Kafka/DCCF/
NWDAF/Valkey, plus ADRF/Doris for the second) and two new unit tests; Phase A, ADRF and DCCF
suites re-run green. Skips, not passes, without Kafka / Doris and under TSan for the MFAF path.

