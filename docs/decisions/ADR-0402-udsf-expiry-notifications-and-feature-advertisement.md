## ADR-0402: UDSF expiry, notifications and feature advertisement

**Status:** Accepted (2026-09-26).

**Decision.** One worker thread per replica sweeps each configured storage every
`expiry_sweep_interval_ms`: record ttl expiry (delete, then POST the RecordBody to the meta's
callbackReference with `Content-Location`); subscription expiry and advance notice (NotificationInfo
to expiryCallbackReference); timer expiry (Timer with timerId, without callbackReference;
PeriodicTimer repeats `repetitionCount` times; `deleteAfter` keeps an expired timer until then, else
it is deleted at its last expiry). Data-change notifications are queued in the same transaction as
the write. `3gpp-Sbi-Callback` values follow the TS 29.500 Annex B pattern with the YAML callback
names (`Nudsf_DataRepository_recordExpired`, `..._onDataChange`,
`..._subscriptionExpiryNotification`, `Nudsf_Timer_timerExpiry`) -- Annex B lists no UDSF value.
Features advertised: DR `6D` = AdvancedQuery, CombinedSearchRetrieve, BulkOperations,
PartialRecordUpdate, RecordDeletePartialSuccess; Timer `5` = PeriodicTimer,
TimerDeletePartialSuccess. Not advertised: Meta Schema (ADR-0401 #1), AdvancedCounting (#3).

**Disclosed.** Delivery is at most once (failed POSTs are logged and counted, not retried; a crash
between pop and POST loses the notification). Expiry resolution is the sweep interval. Bulk deletes
are best effort per record; partial failure is reported only when the PartialSuccess feature is
negotiated, else 500. No LI POI (TS 33.127 defines none for the UDSF). No NF consumes the UDSF yet
(AMF/SMF keep state in Valkey directly, P11) -- wiring a consumer is the follow-up.

