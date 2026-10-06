## ADR-0367: ADRF -- the analytics data repository on Doris, ML models on PostgreSQL, one collection per stored spec

**Date:** 2026-09-15. **Status:** accepted. ADR-0359 step 3.

**Sources.** TS 23.288 V19.7.0 clause 5B (ADRF) and the 6.2B storage/retrieval/deletion
procedures; TS 29.575 V19.7.0 clauses 4.2.2.2-4.2.2.9 (Nadrf_DataManagement: StorageRequest,
StorageSubscriptionRequest/Removal, RetrievalRequest, RetrievalSubscribe/Unsubscribe,
RetrievalNotify including the deletion alert of 4.2.2.8.3, Delete by id and by specification),
4.3.2.2-4.3.2.4 (Nadrf_MLModelManagement: StorageRequest create/update, RetrievalRequest,
Delete by record and by unique id), application errors table 5.2.7.3-1 (Nadrf_DataManagement
defines none, 5.1.7.3), features tables 5.1.8-1 and 5.2.8-1; the two R19 YAML files, now in the
codegen list.

**Where things live (ADR-0359's table, applied).** *Records* -- one Doris row per stored
`NadrfDataStoreRecord` (`nfs/adrf/schema.doris.sql`: the record whole as JSON, beside the
columns the operations select and delete on -- storeTransId, the spec fingerprint, dataSetId,
collected-at, expiry, the deletion-alert target). Doris because the ADRF's store *is* the
analytics repository: what the MTLF trains on next (step 5) is what the ADRF holds, and it should
be queryable there, not copied out of a relational store first. The cost is honest: point reads
and predicate deletes on an OLAP engine, done through a UNIQUE KEY merge-on-write table so
`DELETE ... WHERE` and `UPDATE` are plain statements. *ML models* -- PostgreSQL
(`nfs/adrf/schema.sql`), the model bytes included, so N replicas serve every model with no
shared volume; MLflow is the MTLF's training-side tracker (ADR-0359) and has no ADRF role.
*Control state* -- Valkey (`nfs/adrf/src/state_store.hpp`): storage subscriptions, collections,
retrieval subscriptions, fetch buffers, the two leases. No in-process state; any replica serves
any request; the lifetime reaper and the retrieval-window sweeper run under `SET NX PX` leases
so exactly one replica sweeps.

**"The same data."** A record's spec fingerprint is the DCCF's construction (ADR-0366) --
canonical JSON of the anaSub / dataSub with notification-target fields removed, FNV-1a -- so a
storage subscription, the records it produces, a retrieval subscription naming the same spec,
and a `remove-stored-data-analytics` naming it all land on one key. StorageSubscriptionRequest
opens ONE collection per fingerprint: the ADRF subscribes at the DCCF (Ndccf_DataManagement,
data or analytics) or directly at the NWDAF (Nnwdaf_EventsSubscription, analytics) with its own
`/adrf-inbound/v1/notifications/{fingerprint}` as the target; every further transRefId for the
same spec is mapped onto it (4.2.2.3.2 NOTE 2), the longest requested lifetime wins, and the
target is unsubscribed when the last transRefId (or the whole dataSetId) is removed.

**Storage policy.** `config/adrf.json` `storage_policy`: a requested lifetime is bounded by
`max_lifetime_seconds`, none means `default_lifetime_seconds`; `alert_lead_seconds` before
expiry the reaper POSTs `NadrfAlertNotification` to `delNotifUri` (callback
`Nadrf_DataManagement_storageAlertNotification` or `..._storageSubAlertNotification`, by what
created the record); a `200 {retrievalInd: true}` defers deletion by `alert_grace_seconds`.
Retrieval subscriptions deliver what is already stored inside `timePeriod` on creation and every
later arrival until `stopTime`; `consTrigNotif` turns content into `fetchInstruct` whose
correlation ids are redeemed once (GETDEL) by RetrievalRequest. Every outbound notification
carries `3gpp-Sbi-Callback` -- the ADRF is itself a Data Source into the MFAF
(`NadrfDataRetrievalNotification` is a DataNotification bucket), and the MFAF classifies on that
header (ADR-0365).

**ML models.** `mlModels[].mlModel` is OpenAPI `string, format: binary` inside
`application/json`; decoded as base64, the only way octets travel in a JSON string --
disclosed as an interpretation, not a spec statement. `mlModelInfo[].mlFileAddr.mLModelUrl` is
downloaded over mTLS; stored models are served at `{self}/adrf-mlmodel-files/v1/{modelUniqueId}`
(application/octet-stream), the address returned in `mlFileAddr`. Retrieval and file access are
allowed to the storing MTLF (token subject = `nfInstanceId`) and to `allowConsumerList`
members by `nfInstanceId`; anyone else gets 403 `RETRIEVAL_ML_MODEL_NOT_ALLOWED`
(`ml_model_access_policy: enforced`; `not-enforced` for labs). Partial failures come back as
`modelStoreResults`; all-failed-for-one-reason as the 404 / 500 causes of 4.3.2.2.2.

**Features advertised.** `EnhDataMgmt` (DataManagement no. 3) and `EnModelMgmt`
(MLModelManagement no. 1), negotiated by AND with the consumer's `suppFeat`.

**Disclosed, not built.** targetNfId is resolved to its nfType at the NRF but the address is
the configured DCCF / NWDAF base URL (our profiles carry no ipEndPoints); targetNfSetId selects
the DCCF. A dataSub with an NWDAF target is refused (Nnwdaf_DataManagement is step 4). The
storage approach 4.2.2.3.2 says to return in `storeHandl` has no attribute in
`NadrfDataStoreSubscriptionRef` (YAML wins, ADR-0250) -- applied and logged. A pure
`terminationReq` cannot be sent (the notification's oneOf needs content), so an ended retrieval
subscription is removed silently. A data set mixing analytics and data records is answered with
the kind of its oldest record. `nfSetId` entries of an allow list cannot be matched to a token.
`mlFileFqdn`-only addresses, `dsc`, 307/308, UpEvents/LocEvents/LmfEvents/PcfEvents (no
producers here): not built. Sanity limits: `ml_model_download_max_bytes`, `data_store.max_rows`.

**CI.** `postgres-adrf` service (15438) and the `adrf_data` Doris database in both jobs. Found
while adding them: the **sanitize** job had never applied CHF's PostgreSQL and Doris schemas
(only the build job had), which is why `test_cap_scoped_charging` answered `-1` from its
rating-decision query on the ASan/TSan legs alone. Both steps are now in that job too.
Confirmed 2026-09-16 (run 35010144850): with the schemas applied, `CapScopedCharging` passes on
both sanitizer legs; the diagnostic print in the test's query helper stays, it is cheap.

**Tests.** `tests/integration/test_adrf.cpp` against real NRF, Valkey, Doris, PostgreSQL:
store/retrieve/delete by id and data set with the lifetime bound and the oneOf refusal;
retrieval subscriptions (stored-then-notified, arrival-notified, unrelated spec silent, fetch
instructions redeemed once, unsubscribe); deletion alert honouring `retrievalInd`; a storage
subscription towards a real NWDAF (targetNfId via NRF discovery, two transRefIds one
subscription, removal); the same through DCCF + MFAF + Kafka with the NRF as source; ML models
inline and downloaded, served with the allow list enforced, PUT widening it, per-model delete
results. Skips, not passes, without Doris / PostgreSQL / Kafka and under TSan for the MFAF path.

**Rejected.** *PostgreSQL for the records too* -- simpler point access, but it makes the
"analytics repository" two stores and puts the MTLF's training data behind a copy; ADR-0359
chose Doris for exactly that reason. *Model bytes on a volume* -- a shared filesystem is a
deployment constraint replicas should not carry. *Consumer-driven fetch from the source's MFAF
buffer instead of the ADRF's own* -- the ADRF's fetch ids must resolve at any ADRF replica; the
MFAF's belong to the DCCF's consumers.

