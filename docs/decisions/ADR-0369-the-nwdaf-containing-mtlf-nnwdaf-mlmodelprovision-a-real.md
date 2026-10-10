## ADR-0369: the NWDAF containing MTLF -- Nnwdaf_MLModelProvision, a real training pipeline, ONNX inference in the AnLF

**Date:** 2026-09-15. **Status:** accepted. Step 5 of ADR-0359's build order, first half:
provisioning (TS 23.288 6.2A, TS 29.520 4.5) and the training/storage/inference chain. The
second half -- `Nnwdaf_MLModelMonitor` and the accuracy-driven re-training loop (6.2E.3) -- is
ADR-0370, split at the service boundary so each lands with its own green CI.

**One binary, two logical functions.** TS 23.288 5.1 NOTE 1: "NWDAF can contain an MTLF or an
AnLF or both". `config/nwdaf.json` `role` (`NWDAF_ROLE`) is `anlf`, `mtlf` or `both`; the NRF
profile's `NwdafInfo` follows the role -- `eventIds` for an AnLF, `mlAnalyticsList[].
mlAnalyticsIds` (TS 29.510 `MlAnalyticsInfo`, "supported by the Nnwdaf_MLModelProvision
service") for an MTLF -- so consumers discover the right instance by analytics or by ML model.
An `mtlf`-only instance runs no collector and no events notifier; its Valkey keys
(`nwdaf:mlprov:*`, `nwdaf:mlmodel:*`, `nwdaf:mlstoragesub:*`, `nwdaf:mltrain:*`) are separate
from the AnLF's, so replicas of either role never serve the other's subscriptions; the
`nwdaf:next_id` counter is shared, which is what makes `modelUniqueId` unique across MTLF
replicas with no coordinator (TS 23.288 6.2A.1 NOTE 2 leaves the id's form to stage 3). The
compose lab runs `nwdaf` (anlf, 7798) and `nwdaf-mtlf` (mtlf, 7797) from one image.

**Nnwdaf_MLModelProvision** (`nfs/nwdaf/src/mtlf.cpp`, `/nnwdaf-mlmodelprovision/v1`):
Subscribe (POST → 201 + Location), modify (PUT → 200), Unsubscribe (DELETE → 204/404). Each
`MLEventSubscription` is checked against the events this MTLF trains (`mtlf.events`); events it
does not are reported in `failEventReports` with `UNAVAILABLE_ML_MODEL`, and a request with none
accepted is the spec's 500 `UNAVAILABLE_ML_MODEL_FOR_ALLEVENTS` (4.5.2.2.2). `posModelReqInd`
(LMF positioning models) is refused the same way. `eventReq.immRep` returns `mLEventNotifs` in
the response when a model exists. Notify (4.5.2.4.2) posts `array(NwdafMLModelProvNotif)` --
the YAML's callback body -- with `3gpp-Sbi-Callback: Nnwdaf_MLModelProvision_myNotification`;
each `MLEventNotif` carries `mLModelAdrf{adrfId, storTransId}`, `mLFileAddr{mLModelUrl}`,
`modelUniqueId`, `modelProviderId`, `modelUpdateInd` (true for a re-trained model, 6.2A.2 "ML
Model provide indicator") and `addModelInfo` with `modelMetric ACCURACY`, `accMLModel` and
`trainInpInfos` -- the NRF `NotificationEventType` values the training data came from
(`DccfEvent.nrfEvent`) and the sidecar's lineage as the free-form `dataStatisticsInfos` string
(data source, sample counts, MLflow run id, model type, tolerance, held-out MAE). What a
subscription has been told is kept per event (`delivered` version), and a replica delivers a
subscription's notifications only under a per-subscription Valkey lease per tick (ADR-0365's
rule, applied here) -- so N MTLF replicas notify each new model once, and a failed delivery is
retried on the next tick.

**Training, for real.** `nfs/nwdaf/training/train_nf_load.py` is the Python sidecar (CLAUDE.md:
training in Python, inference in C++). The MTLF runs it through a `TrainingExecutor` interface
(`training_executor.hpp`; `SubprocessExecutor` today -- `posix_spawn`, no shell, a timeout, the
interpreter/script/workdir/MLflow URI from config; a remote executor later is the same
`TrainingJob`/`TrainingResult`, CLAUDE.md's "swappable backend"). The model: a
`RandomForestRegressor` predicting an NF instance's NEXT reported `load` from its last four
observed loads, their mean and its registered share -- a one-step forecast whose horizon is the
AnLF's collection cadence; the feature list is one contract spelled twice (`FEATURE_NAMES` in the
script, `kNfLoadFeatureNames` in `model_runtime.hpp`, unit-tested to agree). Accuracy follows
TS 23.288 5C.1 -- "correct predictions divided by the total number of predictions", the rule for
"correct" being implementation-defined (NOTE 3): within `mtlf.accuracy_tolerance` load points of
the observed value, on a held-out 20%. Every run is an MLflow run (SQLite by default, a
self-hosted server by config) with the data source, the ADRF data set, the sample counts, the
accuracy and the ONNX artifact. Below `mtlf.min_samples` usable windows the script trains on a
labelled synthetic AR(1) bootstrap so the chain is provably functional before the network has
produced enough observations; the label travels in the report, the MLflow tag, the model record
and the notification -- never blended, never hidden. Local run this increment: 108 real windows
from an injected series → 100% within tolerance, MAE 4.5; the bootstrap → the CI's first model.

**Data and models through the ADRF (6.2B, 6.2E.2 step 8).** On its loop the MTLF asks the ADRF
to store the AnLF's NRF NF-status data -- `Nadrf_DataManagement_StorageSubscriptionRequest` with
an `nrfDataSub`, `targetNfId` = an AnLF discovered at the NRF by `nwdafInfo.eventIds`, tagged
`dataSetTag{dataSetId: mtlf.data_set_id}` -- which the ADRF serves by opening an
`Nnwdaf_DataManagement` subscription at that AnLF (ADR-0367/0368's path). Training data is the
`Nadrf_DataManagement_RetrievalRequest` by that data set: the merged record's
`dataNotif.nrfEventNotifs`, one series per NF instance in stored order. The trained ONNX is
stored through `Nadrf_MLModelManagement` (`mlModels[].mlModel` base64, `modelUniqueId` assigned
by the MTLF, `allowConsumerList` = the token subjects of the current subscribers); a consumer
that subscribes later is added by a PUT of the record (the owning replica re-reads the bytes as
owner) or, when this replica does not own the record (a restarted MTLF has a new nfInstanceId),
by a re-training that yields a record it owns. Re-training happens when the data set has grown
by `retrain_min_new_windows` usable windows since the last run (counted in C++, no Python), or
after `retrain_interval_seconds` (0 = off); one replica trains an event at a time (a Valkey
lease).

**The AnLF uses the model** (`ml_consumer.cpp`, `model_runtime.cpp`). One replica subscribes at
the MTLF (`mtlf_base_url`, the local configuration 6.2A.0 allows) with this NWDAF's inbound
URI `{self}/nwdaf-inbound/v1/ml-models` as `notifUri`, `mlEvRepCon{ACCURACY, threshold}`,
`immRep`; holder/heartbeat/takeover as for the DCCF collection, unsubscribe on graceful
shutdown. Every replica handles the notification (the active model per event in Valkey) and
retrieves the bytes from the ADRF on first use (`Nadrf_MLModelManagement_RetrievalRequest`
at `mLModelUrl`, 6.2B.7) into an in-process ONNX Runtime session -- a cache of an artifact, not
state. NF_LOAD is now what TS 23.288 6.5.3 defines: a request whose `ana-req.endTs` /
`extraReportReq.endTs` lies in the future gets **predictions** (Table 6.5.3-2 -- NF load per
instance with `confidence` = the model's held-out accuracy, `AnalyticsData.start/expiry` as the
validity period, no peak) from each instance's observed history; a past or absent period stays
**statistics** (Table 6.5.3-1). An instance without four loaded observations, or a lab without
an MTLF, keeps its statistics and carries no confidence -- never a guess dressed as a prediction.

**Proof.** `tests/integration/test_nwdaf_mtlf.cpp` (its own ctest binary with a 400 s TIMEOUT:
two trainings in one test): NRF + ADRF (Doris/PostgreSQL/Valkey) + an `anlf` + an `mtlf`
instance + the real sidecar. Refusal (500 `UNAVAILABLE_ML_MODEL_FOR_ALLEVENTS`), mixed
acceptance with `failEventReports`, 201 + Location; the first notification (bootstrap-trained,
`mLModelAdrf`/`mLFileAddr`/`modelUniqueId`/`ACCURACY`/lineage, callback header); the model
retrieved from the ADRF as the subscriber (real ONNX bytes); NRF observations with loads injected
at the AnLF's inbound delivery endpoint → stored by the ADRF through Nnwdaf_DataManagement → the
MTLF re-trains on them (`modelUpdateInd`, `data_source: adrf`, ≥ 40 real windows, a new
`modelUniqueId` and MLflow run); the AnLF answers a future-period NF_LOAD request with
`confidence` on every instance and a present-period one without. Unit tests: the feature
vector against the sidecar's rule, no features from short or loadless history, the runtime
rejecting non-ONNX bytes. CI: the sidecar's venv is prepared once in the runner's home
(rebuilt only when its imports fail) and `NWDAF_TRAINING_PYTHON` points the MTLF at it; the
compose image bakes the venv into `/opt/nwdaf-training`.

**Disclosed.** No NF in this lab writes `load` into its NRF profile, so real training data
appears only when NFs report load (or, in the test, when observations are injected); until
then the MTLF's models are bootstrap-labelled. Per-event timestamps are not carried in
`nrfEventNotifs` (TS 29.510 `NotificationData` has none), so the training series is
sequence-ordered at the collection cadence rather than time-indexed. The one-step horizon is
reported as the requested period's prediction. The ADRF record owner is the MTLF's
per-process nfInstanceId, so a restarted MTLF re-trains rather than updating its predecessor's
record. `Nnwdaf_MLModelTraining` (its only consumer is another MTLF -- horizontal federated
learning, 6.2C), `Ue_Positioning`/LMF models, `ModelSharing`/`VerticalFederatedLearning`
features, `inferDataForModel`, `useCaseCxt`-based model selection, `mLTargetPeriod`/
`expiryTime`/`timeModelNeeded` (accepted, stored, not acted on): not in this increment;
MLModelTraining moves to ADR-0359 step 6 with VFL. Features advertised: none.

**Found on the way, fixed.** (1) ADR-0358's NF_LOAD statistics carried `confidence: 100`
("reported by the NF itself"); TS 23.288 Table 6.5.3-1 gives statistics no confidence -- only
predictions (Table 6.5.3-2) carry one. Removed; the unit test now asserts its absence and the
integration test asserts a present-period request has none. (2) The ADRF stored records of a
shared collection under the FIRST subscriber's `dataSetTag` only, so a second
`StorageSubscriptionRequest` for the same spec with its own data set (the MTLF's, run-unique)
never saw a record: records of a shared collection are now stored once per distinct data set
tag among its storage subscriptions (4.2.2.3.2 NOTE 2 shares the collection, not the tags); a
subscriber that named no tag takes the collection's own, as before.
(3) A first cut fetched the model bytes through the NWDAF's shared HTTP client from inside
`compute()`, which the request path already runs under that client's mutex -- a deadlock that
also blocked SIGTERM; the consumer has its own client. (4) `nwdaf.Dockerfile` exposed
7797/9484 for a service configured on 7798/9485; both roles' ports are listed now.

**Rejected.** Committing a pre-trained ONNX and skipping training in CI (the CHF precedent):
the MTLF's whole claim is that it trains; a CI that never runs the sidecar would not test it.
Training inside the AnLF process (no MTLF role): the spec's split exists so that inference
replicas scale apart from training, and CLAUDE.md forbids Python on the inference path. Storing
models on a shared volume instead of the ADRF: 6.2B.5 names the ADRF, and a volume is not
addressable by a peer NF.

