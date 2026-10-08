## ADR-0471: NWDAF Nnwdaf_MLModelTraining: subscription CRUD now, federated-learning round logic as the next increments

**Date:** 2026-10-08. **Status:** Proposed (increment 1 built and tested locally 2026-10-08; increments 2-5 not started).

**Context.** TS29520_Nnwdaf_MLModelTraining.yaml (v1.1.0, TS 29.520 V19.5.0, apiRoot `/nnwdaf-mlmodeltraining/v1`) defines a subscription resource, a merge-patch, an unsubscribe-info operation and a notification callback. Its schemas are dominated by horizontal federated learning (HFL) fields. TS 23.288 V19 6.2C.2.2 is the procedure: an FL Server NWDAF subscribes FL Client NWDAFs (this service), each client trains locally and reports interim local model information, the server aggregates (step 5), repeats until a termination condition, then unsubscribes the clients with a cause. 6.2C.2.3 adds client leave (Notify with termTrainReq), reselection and delay handling. User direction 2026-10-08: build the CRUD first, then the FL round logic in full scope, without further confirmation.

**Decision (increment 1, coded).** The YAML is added to libs/sbi-generated. nfs/nwdaf (role mtlf or both) serves POST/PUT/PATCH (RFC 7396, only NwdafMLModelTrainSubscPatch members)/DELETE on /subscriptions and POST .../unsubscribe-info, with state in Valkey (`nwdaf:mltrainsub:*`, MlStore) and the service name `nnwdaf-mlmodeltraining` in the NRF profile. mLEventSubscs events the MTLF does not train go to failEventReports; none trainable is 500 UNAVAILABLE_ML_MODEL_FOR_ALLEVENTS (that cause string is reused from the provision service, not read from this YAML; to be checked against the TS 29.520 cause table before increment 2).

**Planned increments (not started).**
2. Notification sender (NwdafMLModelTrainNotif to notifUri; oneOf delayEventNotif / mLModelInfos / termTrainReq) and immReport.
3. FL client: local training round per roundInd, maxResTime from mLTrainRepInfo, DelayEventNotif on overrun, skipFlInd, mLAccChkFlg global-model accuracy, StatusReportInfo, termination via unsubscribe-info (FLServerTermCause).
4. FL server: select clients via NRF (flCapabilityType, TS 29.510), subscribe, collect, aggregate, next round, terminate with the final model, notify the Provision consumer (6.2C.2.2 steps 0-9, 6.2C.2.3).
5. NRF profile: register FL capability type and time interval (TS 29.510 flCapabilityType/flTimeInterval).

**Open design issue (blocks increment 3/4 model handling, default stated).** TS 23.288 does not define the interim local model encoding or the aggregation algorithm. The current NF_LOAD model is a RandomForestRegressor exported to ONNX (nfs/nwdaf/training/train_nf_load.py); trees cannot be meaningfully averaged. Default taken unless the user overrides: for FL-mode events only, add a parametric model family in the Python sidecar (training stays in the sidecar), exchange its parameters through the existing ADRF model path, aggregate by sample-count-weighted averaging (FedAvg) in the sidecar, and keep the random forest for non-FL events. This is a project choice, not a 3GPP requirement, and will be labelled so.

**Disclosed stubs in increment 1.** No notification is ever sent; FL fields are stored and not acted on; unsubscribe-info only removes the subscription; one local integration test (nwdaf_ml_training_sub_integration_tests, 1 test, passed against a real NRF, NWDAF and Valkey); the existing nwdaf_mtlf tests were NOT re-run after this change, and CI has not yet seen it.

**Rejected alternatives.** Averaging random-forest trees (no defined meaning). Building FL server and client in one step (an untestable change). A hand-written DTO (the YAML generates it).
