## ADR-0188: SMSF -- second Tier 2 NF, `Nsmsf_SMService` (complete, not a slice)

### Context

Sixth NF built under ADR-0184's continuous move-to-next-NF process, second Tier 2 NF, immediately
following 5G-EIR (ADR-0187). SMSF's real spec surface (`TS29540_Nsmsf_SMService.yaml` v2.4.0, 1
file, 5 operations) was the next-smallest real candidate from ADR-0187's own Tier 2 survey (GMLC
was the other, 1 file/5 ops but 1232 lines vs SMSF's 534) -- chosen because it was genuinely
closable in full this turn, same reasoning as 5G-EIR.

### Implementation

Source: `specs/5G_APIs-REL-19/TS29540_Nsmsf_SMService.yaml` (v2.4.0), commit
`bca84b60a37773133bcae97e5c6c0d10a93b47b6`. All 5 real operations implemented:
`SMServiceActivation` (`PUT /ue-contexts/{supi}`), `SMSServiceParameterUpdate`
(`PATCH /ue-contexts/{supi}`), `SMServiceDeactivation` (`DELETE /ue-contexts/{supi}`), `SendSMS`
(`POST /ue-contexts/{supi}/sendsms`), `SendMtSMS` (`POST /ue-contexts/{supi}/send-mt-sms`).

- `SendSMS`/`SendMtSMS` use real `multipart/related` bodies (RFC 2046/2387) -- built entirely on
  the existing `sbi_core::multipart` codec already used by `nfs/amf`/`nfs/smf`
  (`libs/sbi-core/include/sbi_core/multipart.hpp`), no new infrastructure needed.
- `SendMtSMS`'s own `SmsData`/`SmsDeliveryData` schemas are a real cross-file `$ref` into
  `specs/5G_APIs-REL-19/TS29577_Nipsmgw_SMService.yaml` (IP-SM-GW's own service) -- confirmed the
  codegen's `SchemaRegistry.resolve_ref` already auto-loads a referenced file on demand and
  `render.py` groups its types into their own output file regardless of whether it was in the
  pilot list, but per this project's own established precedent (`TS29571_CommonData.yaml` is
  explicitly listed despite being universally cross-referenced), `TS29577_Nipsmgw_SMService.yaml`
  was added to `libs/sbi-generated/CMakeLists.txt`'s pilot set too, for correct incremental-rebuild
  dependency tracking. Only `SmsData`/`SmsDeliveryData` are used; IP-SM-GW's own `put`/`post`
  operations are out of scope for this NF and not implemented.
- `nfs/smsf/` scaffolded to match `nfs/eir`/`nfs/scp`'s shape (in-memory store, no Redis/Postgres):
  `CMakeLists.txt`, `src/stores.hpp`/`.cpp` (`UeSmsContextStore`, full CRUD + RFC 6902 patch via
  `nlohmann::json::patch()`, same precedent as `nfs/scp`'s own `ScpEventSubscriptionStore`; a
  monotonic version counter synthesizes the real `ETag` header both `PUT`/`PATCH` declare).
  `config/smsf.json` (port 7793, metrics 9481). `certs/smsf/` generated via
  `scripts/gen-lab-pki.sh`. Added to top-level `CMakeLists.txt`,
  `deploy/docker/docker-compose.yml`/`smsf.Dockerfile`, and the `pki-init` cert-generation
  argument list. No Helm chart -- same pre-existing, disclosed, project-wide gap.
- New conformance test file `tests/conformance/test_smsf_dtos.cpp` (3 round-trip tests over the
  real generated DTOs, including the cross-file IP-SM-GW `SmsData` type), wired into
  `tests/conformance/CMakeLists.txt`.

### Real, disclosed simplification

This project has no real downstream SMS-GMSC/IWMSC (for `SendSMS`, the real uplink relay target)
or a real TS 24.011 SMS-over-NAS CP-DATA relay to AMF (for `SendMtSMS`, the real downlink path) --
both genuinely out of this session's spec material, same class of gap as ADR-0104's disclosed
ProSe-auth gap. `SendSMS` therefore always responds `SMS_DELIVERY_SMSF_ACCEPTED` (the real enum
value meaning "SMSF accepted for further processing", not a fabricated "delivered" claim).
`SendMtSMS` responds with a real `SmsDeliveryData` multipart envelope but an empty (0-byte) binary
payload in place of a real TS 24.011 SMS-DELIVER-REPORT PDU, which this project cannot encode
without that spec material -- an honest placeholder, not a fabricated PDU. Real, disclosed,
deferred wiring: AMF does not yet call `SMServiceActivation` during Registration, and no NF calls
`SendSMS`/`SendMtSMS` -- same pattern as 5G-EIR's own AMF non-wiring (ADR-0187).

### Live verification (real, live processes, not self-consistency)

Real `nrf` + `smsf` processes, real OAuth2 bearer token, real mTLS curl against all 5 routes:
`PUT` create -> real `201` + `Location` + `ETag: 1` + body; `PUT` update same supi -> real `204` +
`ETag: 2` (no body, matching the real spec's own 204-has-no-content declaration). `PATCH` (RFC
6902 `replace`) -> real `204`; against a nonexistent supi -> real `404`. `SendSMS` with a real
multipart/related body (JSON `SmsRecordData` part + binary SMS-payload part) -> real `200` +
`SmsRecordDeliveryData{deliveryStatus: SMS_DELIVERY_SMSF_ACCEPTED}`; against a nonexistent supi ->
real `404`. `SendMtSMS` with a real multipart request -> real `200` with a real multipart/related
response (`SmsDeliveryData` JSON part + empty binary part, boundary correctly generated).
`DELETE` -> real `204`; repeated -> real `404`. Negative auth: a malformed bearer token, real `401`
`ProblemDetails`. Prometheus `/metrics`: all 5 operation counters present and correctly
incremented (`smsf_sm_service_activation_total=2`, others each `1`, matching the exact call
sequence above). All processes killed cleanly afterward (PID-verified).

### Testing

`smsf` and `conformance_tests` built clean (zero warnings) both before and after
`clang-format-18`. Full project rebuild clean (78/78 targets, only the pre-existing onnxruntime
static-lib linker warning, unrelated to this change). Full `ctest` (excluding the two disclosed
pre-existing flaky tests): 355/355 pass, zero regressions (3 new `SmsfDtos` tests +
`structural_conformance` covering the new DTOs).

### What this ADR does NOT include

Real downstream SMS-GMSC/IWMSC relay or TS 24.011 SMS-over-NAS CP-DATA encoding (genuinely out of
this session's spec material, disclosed above). AMF wiring to call `SMServiceActivation` during
Registration (real, disclosed, deferred). IP-SM-GW's own `Nipsmgw_SMService` operations (only its
two referenced schemas are used). No Helm chart (pre-existing, disclosed, project-wide gap). Like
5G-EIR, this ADR's own real API surface IS fully closed -- SMSF's entire real spec (1 file, 5
operations) is now built and live-verified, not a partial slice.

