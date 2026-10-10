## ADR-0370: Nnwdaf_MLModelMonitor -- the AnLF measures its predictions, the MTLF re-trains on degradation

**Date:** 2026-09-15. **Status:** accepted. The second half of ADR-0359 step 5: TS 23.288
5C.1 / 6.2E.3 (AnLF-assisted MTLF ML model accuracy monitoring), TS 29.520 4.7. This closes the
loop ADR-0369 opened: a provisioned model is now watched in use, and a model that stops
predicting well is replaced without an operator.

**Who serves what.** The YAML is one API with two producers, and the roles split the same way
the spec does (4.7.2.1): the **MTLF** serves `Register`/`Deregister`
(`POST /nnwdaf-mlmodelmonitor/v1/registrations` → 201 + Location, `DELETE .../{id}` → 204,
`MLModelMonitorReg` with `modelId` and exactly one of `consumerId`/`consumerSetId`); the
**AnLF** serves `Subscribe`/`Unsubscribe` (`POST /subscriptions` → 201 + Location with
`immReport` under `immRep`, `PUT` → 200, `DELETE` → 204; `MLModelMonitorSub` with `modelIds`,
`notificationUri`, `notifCorrId`, `ACCURACY`, `accuThreshold`, `eventReportReq`) and sends
`Notify`. Both roles register `nnwdaf-mlmodelmonitor` in their NRF profile; the NWDAF now
registers its `nfServices` with an `ipEndPoints` entry (TS 29.510 NFService), which is how the
MTLF finds an AnLF's subscription endpoint from the `consumerId` of a registration -- 6.2E.3.3
allows "a service discovery procedure at the NRF" for exactly this, and it needs no config.

**The loop, in the spec's steps.** (6.2E.3.2 1-2) When the AnLF's holder replica takes a model
into use it registers it at the MTLF (`consumerId` = its nfInstanceId, `modelId`, `mLEvent`),
deregistering the previous model's registration and its own on shutdown. (6.2E.3.3 0-2) The
MTLF's loop subscribes at that AnLF for the model's accuracy (`accuThreshold` =
`mtlf.accuracy_threshold`, `notificationUri` = `{self}/nwdaf-inbound/v1/ml-monitor`,
`immRep`). (4-5) The AnLF logs every prediction it makes (`accuracy_monitor.cpp`, Valkey ZSET,
shared by every replica); a checker judges each prediction against the instance's next observed
load -- the model is a one-step forecast, so that is the ground truth -- as correct within
`accuracy_monitoring.tolerance` load points (the rule the MTLF trained with; 5C.1 NOTE 3 leaves
"correct" to the implementation and this is the implementation), and keeps outcomes for
`window_seconds`. (6) `MLModelMonitorNotify{notifCorrId, modelAccuInfos[{modelId, ACCURACY,
mlModelAcc = correct/total, inferenceNum, deviation = mean absolute, monitorInterval}],
accuMeetInd, mLEvent}` with `3gpp-Sbi-Callback: Nnwdaf_MLModelMonitor_myNotification` -- sent
while the accuracy is below the subscription's threshold (rate-limited by
`min_report_interval_seconds`), once when it meets it again, or every `repPeriod` under
`notifMethod PERIODIC`; never before `min_inferences` judged predictions. One replica reports a
subscription per tick (lease). (8) The MTLF marks the model degraded on `accuMeetInd=false` or
`mlModelAcc` below its threshold (degradation is "an internal procedure", NOTE 2: one report
suffices here, with `retrain_cooldown_seconds` between accuracy-triggered trainings) and (9)
re-trains it and notifies every provision subscriber with `modelUpdateInd=true`; the AnLF then
registers the new model and the loop continues on it.

**Proof.** `NwdafMtlf.AccuracyBelowThresholdIsReportedAndDrivesRetraining`: the AnLF's NRF
profile carries the monitor endpoint; a bootstrap model is provisioned; a smooth series is
injected and six predictions made; the test, subscribed at the AnLF as a monitoring consumer
of its own, then injects ground truth 60 load points above every prediction and receives the
report (`accuMeetInd=false`, `mlModelAcc < 80`, `inferenceNum ≥ 3`, `deviation > 10`, the
callback header); with the data-growth trigger disabled for the test, the MTLF's re-provision
with `modelUpdateInd=true` and a new `modelUniqueId` can only have come from the accuracy path.

**Disclosed.** The MTLF-based variant (6.2E.2: the MTLF computing accuracy itself from
inference data the AnLF stored in the ADRF under `inferDataForModel`) is not built -- the
AnLF-assisted variant is; `anaFeedbacks` (Analytics Feedback Information from an analytics
consumer, 6.1.1) is accepted, stored with the degradation marker and not acted on -- no
consumer of ours sends it; `modelAccuInd` / analytics-transfer re-association (6.2E.3.2 NOTE 1)
and `consumerSetId`-only registrations (no single endpoint) are accepted and not subscribed
for; the AnLF's own analytics-accuracy exposure to consumers (6.2D, `accuInfo` in
AnalyticsData) is not built. NF_LOAD is the only monitored analytics ID. A registration's
subscription is opened once and not re-verified (an AnLF that restarts registers again under a
new nfInstanceId; the stale registration and its subscription remain until the MTLF is
restarted with a clean Valkey -- the same liveness follow-up ADR-0368 named for the DCCF).

