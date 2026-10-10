## ADR-0358: NWDAF Phase A -- the AnLF, two services, two analytics, from real data

**Date:** 2026-09-14. **Status:** Closed (work pushed; last citing commit 9909c49 on origin/main, 2026-10-06). accepted. Item 5 of the user's list; Phase 5 of PROMPT.md.

### What was built

`nfs/nwdaf`: an NF registered with the NRF as `NWDAF`, serving two of TS 29.520's ten R19
services at the API roots their YAML declares (`/nnwdaf-analyticsinfo/v1`,
`/nnwdaf-eventssubscription/v1`, ADR-0325) -- `GetNWDAFAnalytics`, `GetNwdafContext`, and the
seven operations on `/subscriptions` and `/transfers`. Two analytics IDs are computed, each from
the input TS 23.288 names for it:

- **NF_LOAD (6.5)** from the NRF. Table 6.5.2-1 says NF load and status come from the NF profile,
  so the analytic is a discovery over the configured NF types and the output carries what each
  profile carried. A profile without `load` yields no `nfLoadLevelAverage` -- absent, not zero.
- **ABNORMAL_BEHAVIOUR (6.7.5)** from the CHF feature store (ADR-0350). Two of the spec's nine
  `ExceptionId` values are derivable from per-subscriber mean/max/stddev and session count and are
  produced: `UNEXPECTED_LARGE_RATE_FLOW` (max more than `sigma_threshold` deviations above the
  subscriber's *own* mean, above an absolute floor) and `TOO_FREQUENT_SERVICE_ACCESS`. The other
  seven need AMF/SMF event data the bus does not carry yet and are not produced. Judging against
  the subscriber's own distribution is ADR-0340's lesson applied: a heavy line is not abnormal for
  being heavy.

Live against the real stores: NF_LOAD lists the NWDAF instance the NRF holds with
`statusRegistered: 100`; ABNORMAL_BEHAVIOUR over 147,237 feature rows finds
`UNEXPECTED_LARGE_RATE_FLOW` at level 5, trend UP across the two real days, 1,614 subscribers, ratio
2%, confidence 10 -- low because the least-sampled affected subscriber had few sessions, which is
the honest number. Six unit tests fix the rules; three integration tests prove the services against
a real NRF.

### What the compiler caught, and what the codegen changed

- TS 29.520's `NfStatus` is not the NRF's status enum. Its YAML: "the percentage of time spent on
  various NF states". The first draft assigned the enum across; the type system refused. With one
  observation per request, the honest value is 100% in the current status. A time-weighted share
  needs repeated observation -- DataManagement, Phase C.
- Adding the six missing Nnwdaf YAML files to the generator (full-coverage rule) re-cut the
  cross-reference cycle groups: `ReportingOptions` gained suffixes (NEF now names the UDM EE one)
  and `Nnef_PFDmanagement` and `Nlmf_DataExposure` merged into the common group (three includes
  repointed). The codegen's own CMake comment predicts exactly this; it is the price of generating
  from the complete YAML surface rather than a convenient subset.
- libmariadb 3.4 demands TLS by default and Doris' MySQL port has none. Same two option clears as
  CHF's writer, reached independently when the first smoke refused to connect.

### Disclosed

- `/transfers` is stored, not transferred: no second NWDAF exists to transfer to.
- Subscription and transfer state is **in-process**. Phase A only. The user has since directed that
  AnLF, MTLF, DCCF, ADRF and MFAF be built on a scalable architecture; that is ADR-0359 and it
  externalises this state before any of the other functions is built on top of it.
- `GetNwdafContext` answers 204: Phase A holds no per-UE context.
- Notifications are periodic on the configured interval; THRESHOLD and ON_EVENT_DETECTION methods
  are treated as periodic in Phase A.
- LI: TS 33.127 clause 7.18 requires an IRI-POI in the NWDAF. Not built (blocker #0).

