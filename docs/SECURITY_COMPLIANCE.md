# Security compliance against the 3GPP 33-series (Release 19)

**Status:** first assessment, 2026-09-13. Sources fetched from the official 3GPP archive by
`tools/specs/fetch_3gpp_specs.py`; exact versions in `specs/3gpp/MANIFEST.tsv`. ADR-0354.

## How to read this document -- and what it does not claim

Twenty-two specifications were requested. Twenty-one exist and were fetched at their newest
Release-19 version; every one of them *has* a Release-19 version, so nothing here is assessed
against an older release standing in for R19.

**TS 33.258 does not exist.** The 3GPP archive's 33-series contains 33.250, 33.256 and 33.259 --
no 33.258. It is recorded here as *not a published 3GPP TS* rather than quietly dropped, in
keeping with this project's rule never to invent a TS number.

**What "read completely and in depth" means here, stated honestly.** The corpus is 109,817 lines
of text, roughly 2,500 pages. For each specification the *Scope* clause was read to decide
applicability, the clause structure was mapped, and every clause that places a requirement on a
network function this project implements or has in scope was read in full. Each finding below
cites the clause it came from and the evidence in the codebase it was checked against -- a file, a
test, or a live experiment. Clauses placing requirements only on components this project does not
build (the gNB, the UE, an EPC, a UAS or V2X function) were read for scope and are marked out of
scope, not compliant.

**Verified vs. recorded.** A row marked *implemented* was checked in code or by running something.
A row marked *not implemented* was checked the same way and found absent. A row marked *not
assessed* was not checked and should not be read as either.

## Findings, most serious first

### F1. Lawful Interception is entirely absent -- TS 33.126 / 33.127 / 33.128

`grep -rli 'lawful intercept'` across `nfs/`, `libs/` and `docs/` returns nothing. There is no
Point of Interception, no LI_X1/X2/X3 interface, no ADMF/LIPF client, no SIRF in the NRF.

This is not a peripheral gap. TS 33.127 V19.7.0 places LI requirements on **every control-plane NF
this project has**:

| NF | Clause | What the NF *shall* provide |
|---|---|---|
| **CHF** | 7.22 | An IRI-POI generating `ChargingDataEvent` xIRI whenever a Charging Data Request arrives over **Nchf or Rf** for a target, keyed on SUPI, PEI, GPSI, IMPU, IMPI, IMSI, MSISDN or IMEI; both H-CHF and V-CHF |
| AMF | 6.2.2 | IRI-POI for registration, deregistration, location update, identifier (de)association, handover, service accept, UE context update and unsuccessful procedures; SUPI *and* current SUCI in every event |
| SMF | 6.2.3 | IRI-POI for PDU session establishment/modification/release and start-of-interception; CC-TF triggering the UPF over LI_T3 |
| UPF | 6.2.3 | CC-POI duplicating user-plane packets on rules received from the SMF |
| UDM | 6.2.4 / 7.2.2 | IRI-POI for service-area registration events |
| SMSF | 6.2.5 | IRI-POI |
| NRF | 6.2.6 | A SIRF emitting LI_SI notifications on NF registration, update, deregistration and service-chain change |
| NEF | 7.9 | IRI-POI for NIDD, device triggering, parameter provisioning, AF session with QoS |
| NWDAF | 7.18 | IRI-POI (relevant the moment NWDAF is built) |

Transport is fixed by 5.4: LI_HI1 uses ETSI TS 103 120; LI_X1 uses ETSI TS 103 221-1. Stage 3
detail is TS 33.128 (28,910 lines, the largest document in the set), which was read for structure
and for the CHF `ChargingDataEvent` definition, not clause by clause.

TS 33.106 (also requested) states of itself in its own Scope: *"historical ... Rel-14 and earlier
... replaced by TS 33.126 from Rel-15 onwards and should only be used as a historical reference."*
TS 33.107 and 33.108 are its 3G/EPS architecture and handover companions. They are recorded as
superseded for this project's purposes; 33.126/127/128 are the applicable set.

**Why this outranks everything else in this document.** The commercialization mandate (ADR-0049)
targets carrier deployment. In essentially every jurisdiction a public network operator is legally
required to support LI before carrying traffic. A core that cannot be intercepted cannot be
deployed by a licensed operator, regardless of how good its charging is. TS 33.126 §4 makes the
same point from the other side: which requirements apply is jurisdiction-specific, but *some* set
always does.

### F2. Mandatory NAS algorithms missing -- TS 33.501 §5.5.1 / §5.5.2

The AMF **shall** support NEA0, 128-NEA1, 128-NEA2 for ciphering and NIA0, 128-NIA1, 128-NIA2 for
integrity. 128-NEA3/NIA3 (ZUC) are "may".

`libs/aka-crypto/include/aka_crypto/nas_security.hpp` line 15 discloses that only 128-NEA2/NIA2
(AES) are implemented and that SNOW 3G and ZUC are "out of scope". SNOW 3G is not optional: a UE
whose security capability offers only 128-NEA1/NIA1 cannot complete Security Mode Command against
this AMF. ZUC's absence is conformant. NEA0 (null ciphering) is also required and absent; NIA0
"shall be disabled" outside unauthenticated-emergency deployments, so its absence is conformant.

### F3. Duplicate JSON keys are silently accepted -- TS 33.117 §4.3.6.3

*"The occurrence of the same name (or key) twice within such a structure leads to an error and the
rejection of the message."* Tested directly against the project's parser (nlohmann/json, as used
by every SBI endpoint):

```
{"ratingGroup": 1, "ratingGroup": 99}   ->   parsed OK, ratingGroup=99
```

No rejection; the last value wins silently. This is a real attack surface, not a formality: a
payload carrying a key twice can pass one value to whatever validated the first occurrence and a
different value to whatever consumed the last. Fixable in one place (a parser callback that
rejects a repeated key) since all SBI bodies go through `sbi_core::http2::parse_json_body`.

### F4. NRF does not authorize discovery -- TS 33.501 §13.3.1.3

*"The NRF shall check that the values of the authorization parameters in the NF (Service) Profile
of an NF Service Producer allows an NF Service Consumer to discover the NF Service Producer. In the
response message, the NRF shall only return information of those NF Service Producer instances
that the NF Service Consumer is authorized to discover."*

`grep allowedNfTypes|allowedPlmns|allowedNssais|allowedNfDomains nfs/nrf/src/` returns nothing.
Every consumer that can authenticate can discover every producer.

### F5. TLS 1.3 only -- TS 33.210 §6.2.1 (mandated by TS 33.501 §13.1.0)

*"TLS 1.2 as specified in RFC 5246 shall be supported. TLS 1.3 as specified in RFC 8446 shall be
supported."* `http2_client.cpp:114` pins `CURL_SSLVERSION_TLSv1_3`. A profile-conformant peer that
offers only TLS 1.2 cannot connect. This is the one finding where the non-conformant choice is the
*more* secure one; it is recorded as a decision for the architect, not silently changed.

### F6. SNI is never sent -- TS 33.501 §13.1.0

*"TLS clients shall include the SNI extension."* All 53 base URLs in `config/*.json` are
`https://127.0.0.1:<port>`. SNI cannot carry an IP literal, so no lab client ever sends it. The
client code would send SNI for a hostname; the lab never exercises that path. A lab-configuration
gap, but one CI inherits, so nothing proves SNI works.

### F7. SBA certificate profile not verified -- TS 33.501 §13.1.0 -> TS 33.310 §6.1.3c

Certificates "shall be compliant with the SBA certificate profile specified in clause 6.1.3c of TS
33.310". The lab certificates carry `DNS:<nf>, DNS:localhost, IP:127.0.0.1` in SAN and `CN=<nf>`.
Whether that satisfies §6.1.3c cannot be judged without TS 33.310, which was not in the requested
set. **Not assessed** -- recorded as the next document to fetch.

### F8. SEPP / N32 not implemented -- TS 33.501 §5.9.3, §13.2, §13.5

Every inter-PLMN requirement (application-layer N32 protection, PRINS, roaming-hub handling,
N32-c/N32-f) presupposes a SEPP. There is no `nfs/sepp`. Already in Tier-2 scope in CLAUDE.md;
recorded here because the roaming charging paths this project *does* have (TAP, V-CHF/H-CHF) are
exactly the traffic §13.2 exists to protect.

### F9. TS 33.528 (PCF SCAS) is an unapproved shell

Line 14 of the fetched V19.0.0: *"The present document has not been subject to any approval
process by the 3GPP Organizational Partners and shall not be implemented."* Its clauses 4.2-4.4
are headings that defer to TS 33.117 with 60 lines of body. Nothing to assess; the applicable
content is TS 33.117.

## What is implemented and verified

| Requirement | Clause | Evidence |
|---|---|---|
| Mutually authenticated TLS 1.3 on every SBI | 33.501 §13.1.0 | `http2_server.cpp`, `http2_client.cpp`; every NF log line "TLS 1.3 + mTLS" |
| NRF as OAuth 2.0 authorization server, signed tokens | 33.501 §13.4.1.1 | `jwt.cpp:44` ES256 sign, `:60` verify. CLAUDE.md's "unsigned fake OAuth2 token" debt is **stale** -- resolved |
| 5G-AKA, Milenage | 33.501 §6.1.3.2 | `test_milenage.cpp`, `test_ausf_ue_authentication.cpp` |
| EAP-AKA' | 33.501 §6.1.3.1 | `test_eap_aka_prime.cpp` |
| SUCI concealment / SIDF de-concealment, ECIES Profile A/B | 33.501 §6.12, §5.8.2 | `test_suci.cpp`; `nfs/udm/src/main.cpp:74-180` |
| NAS integrity + ciphering (128-NIA2 / 128-NEA2) | 33.501 §6.4 | `test_nas_security.cpp` |
| Steering of Roaming protection | 33.501 §6.14 | `test_sor_mac.cpp` |
| UE Parameters Update protection | 33.501 §6.15 | `test_ausf_upu_protection.cpp` |
| Overload handling (TPS ceilings, 503/DIAMETER_TOO_BUSY) | 33.117 §4.2.3.3.1 / §4.2.3.3.3 | `test_chf_protocol_ceilings.cpp`, ADR-0290/0295 |
| Robustness against unexpected input | 33.117 §4.2.3.3.4 / §4.4.4 | libFuzzer targets in `tests/fuzz/` |
| JSON parser executes no code, loads no external resources | 33.117 §4.3.6.2 | nlohmann/json by construction |
| Idempotency / duplicate-request detection | 29.500 §5.2.8 (not 33-series; recorded for completeness) | ADR-0352 |

## Per-specification summary

| TS | Title (from the document) | Version | Applicability | Status |
|---|---|---|---|---|
| 33.501 | Security architecture and procedures for 5G system | V19.7.0 | **Core** | Partial -- F2, F4, F5, F6, F7, F8 |
| 33.210 | Network Domain Security (NDS); IP network layer security | V19.3.0 | **Core** (TLS profile) | Partial -- F5 |
| 33.117 | Catalogue of general security assurance requirements | V19.2.0 | **Core** (SCAS) | Partial -- F3; §4.2.3 baseline (RBAC, password policy, security-event logging, log transfer) **not assessed** |
| 33.126 | Lawful Interception requirements | V19.3.0 | **Core** | **Not implemented** -- F1 |
| 33.127 | LI architecture and functions | V19.7.0 | **Core** | **Not implemented** -- F1 |
| 33.128 | LI protocol and procedures, Stage 3 | V19.7.0 | **Core** | **Not implemented** -- F1 |
| 33.528 | SCAS for PCF | V19.0.0 | Core | Unapproved shell -- F9 |
| 33.535 | AKMA | V19.0.0 | In scope, not built (AAnF, Tier 2) | Not implemented |
| 33.122 | Security of CAPIF | V19.4.0 | In scope, not built (CAPIF, Tier 3) | Not implemented |
| 33.203 | Access security for IP-based services (IMS) | V19.1.0 | In scope, not built (IMS AS, Tier 3) | Not implemented |
| 33.220 | GBA | V19.1.0 | Not in scope. **Note:** this project's `nfs/bsf` is the 5G Binding Support Function (TS 29.521), not the GBA BSF | Out of scope |
| 33.224 | GBA Push Layer | V19.0.0 | Not in scope | Out of scope |
| 33.102 | 3G security architecture | V19.1.0 | Reference only (Milenage lineage; CAMEL/CAP paths) | Out of scope |
| 33.401 | EPS security architecture | V19.2.0 | Out of scope -- no EPC here; 4G *charging interfaces* only | Out of scope |
| 33.106 | 3G LI requirements | V19.0.0 | Superseded by 33.126 per its own Scope | Superseded |
| 33.107 | 3G/EPS LI architecture | V19.0.0 | Superseded by 33.127 | Superseded |
| 33.108 | 3G/EPS LI handover interface | V19.0.0 | Superseded by 33.128 | Superseded |
| 33.246 | MBMS security (UTRAN/GERAN/E-UTRAN) | V19.0.0 | Out of scope -- 5MBS security lives in 33.501 | Out of scope |
| 33.511 | SCAS for gNodeB | V19.3.0 | Out of scope -- RAN | Out of scope |
| 33.256 | Security of UAS | V19.2.0 | Out of scope | Out of scope |
| 33.536 | Security of V2X | V19.0.0 | Out of scope | Out of scope |
| 33.258 | *(requested as "Security in the OSS")* | -- | **Does not exist in the 3GPP archive** | Not a published TS |

## Not yet assessed, in priority order

1. TS 33.117 §4.2.3 baseline: RBAC (§4.2.3.4.6.2), password policy (§4.2.3.4.3), security event
   logging and centralised log transfer (§4.2.3.6), session inactivity timeout (§4.2.3.5.2). Most
   bear on the management plane / GUI, which is Phase 7.
2. TS 33.310 -- needed to close F7.
3. TS 33.128 clause by clause for the CHF `ChargingDataEvent` xIRI schema, once F1 is scheduled.
4. TS 33.501 clause 6.2 key hierarchy (K_AUSF, K_SEAF, K_AMF derivation) against `aka-crypto` --
   implemented, but the derivation-vector tests should be checked against Annex A test data.
