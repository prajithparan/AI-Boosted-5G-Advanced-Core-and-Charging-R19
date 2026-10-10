## ADR-0277: AMF and SMF call NSACF -- the wiring ADR-0276 disclosed as missing

**Date:** 2026-09-05
**Status:** Accepted

### What changed

ADR-0276 built NSACF complete and said plainly that nothing called it. This closes that.

- **AMF**, at the end of Registration (TS 23.502 §4.2.2.2.2, immediately after the AM Policy
  Association completes and the UE is really REGISTERED): `Nnsacf_NSAC NumOfUEsUpdate` with
  `updateFlag=INCREASE` for the UE's S-NSSAI.
- **SMF**, after a `CreateSMContext` that returned 201 (TS 23.502 §4.3.2.2.1): `NumOfPDUsUpdate`
  with `INCREASE`, carrying `(SUPI, pduSessionId, S-NSSAI)` -- NSACF keys sessions per
  (SUPI, pduSessionId), because one UE may hold several on one slice.

Both NFs reach NSACF through a config key (`nsacf_base_url`, env-overridable) and their own
per-peer OAuth2 client scoped `nnsacf-nsac`, following ADR-0273's rule and this codebase's
one-client-per-peer pattern.

### Proved by NSACF saying so, not by reading a log

`Nnsacf_NSAC` has **no operation that returns the current count** -- `QuotaUpdate` returns the
configured maxima -- so a test cannot ask NSACF how many UEs it holds. It can make NSACF report
it: `AmfNgapTestGnb.RegistrationAndPduSessionReachNsacf` subscribes to `NUM_OF_REGD_UES` with a
threshold of 1 before the UE exists, drives a real registration and PDU session over real
NGAP/NAS, and asserts on the `SACEventReport` that lands on its own TLS receiver.

The count in that report originated in AMF's call, made during a registration driven over a
different protocol, four processes away from the assertion. The NFs' own logs agree:

```
amf-ngap: NSACF admitted SUPI imsi-999700000000001 onto slice sst=1 (INCREASE)
nsacf:    reported NUM_OF_REGD_UES=1 for slice 1-000001 to subscription sac-sub-1
smf:      NSACF admitted pduSessionId 5 for SUPI imsi-999700000000001 on slice sst=1
```

### Reported, not yet enforced -- the boundary, stated

Both NFs **report** admission and neither **refuses** on rejection. A rejection is logged loudly
(`NSACF REJECTED ... this session is NOT being torn down`) and the count stays accurate.

That is a deliberate boundary, not an oversight. Refusing means failing the registration back to
the UE with the correct 5GMM cause, or tearing down a PDU session whose N4 session and PCF
association already exist -- a behaviour change to the procedures themselves, with its own failure
paths to get right, rather than the call this increment adds. Naming it here so nobody reads
"wired" as "enforced".

### A correct rejection this surfaced, worth recording

The test's own UPF/Sx gate creates throwaway sessions with `sNssai: {"sst": 1}` and **no SD**,
while `config/nsacf.json` configures `sst=1, sd=000001`. NSACF answers those `SLICE_NOT_FOUND`.
That is right, not a bug: per TS 23.003 an S-NSSAI with an SD is a different slice from one
without, and NSACF's canonical key (`slice_key`) reflects exactly that. The real UE's session,
which carries the full S-NSSAI, is admitted. Recorded because a reader seeing those log lines
would otherwise reasonably suspect a keying bug.

---

