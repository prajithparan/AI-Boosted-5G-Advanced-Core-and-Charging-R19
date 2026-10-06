## ADR-0089: gap-closure task #108 -- CHF real TS 32.298 CDR (BER) encoding

### Context

`docs/CAPABILITY_GAP_ANALYSIS.md`'s CHF section named a real, substantial gap: free5GC's CHF has
a genuine ~4,746-line `cdr/` module doing real TS 32.298 ASN.1 BER CDR encoding
(`cdr/cdrType/CHFRecord`, hand-written BER marshal/unmarshal), while this project's own
`nfs/chf/schema.clickhouse.sql` self-disclosed the matching gap ("this is NOT a conformant TS
32.298 CDR... TS 32.298 is not vendored in this repo"). Per ADR-0001's greenfield discipline,
closing it required obtaining and reading the real 3GPP spec directly, not adapting free5GC's own
Go structs. The user supplied the spec this turn (`specs/TS_32_298.pdf`, verified genuine via
`pdftotext -layout`: "ETSI TS 132 298 V18.8.0 (2025-04)... 3GPP TS 32.298 version 18.8.0
Release 18"). **Disclosed version gap**: v18.8.0/Release 18 is what was actually supplied and
read, not this project's own REL-19 baseline -- the CHF-CDR ASN.1 structure has not been
re-verified against a REL-19 text, same disclosed-gap shape as PFCP's own V14.3.0 (ADR-0039).

### Real spec content read (clauses cited, not guessed)

§5.1.5/§5.2.5.2: `CHFRecord ::= CHOICE { chargingFunctionRecord [200] ChargingRecord }`.
`ChargingRecord ::= SET` with real fields `[0]` `recordType` through `[45]`
`nSSAAChargingInformation` (46 fields total). The module declares `DEFINITIONS IMPLICIT TAGS`, so
every field's own context-specific tag REPLACES (not wraps) its underlying universal tag per
X.690 §31. Real cited enum values used: `RecordType::chargingFunctionRecord=200`,
`CauseForRecClosing::normalRelease=0`, `NetworkFunctionality` (22 real values mapped --
`cHF`=0, `sMF`=1, `aMF`=2, `sMSF`=3, `sGW`=4, `iSMF`=5, `ePDG`=6, `cEF`=7, `nEF`=8,
`pGWCSMF`=9, `mnS-Producer`=10, `sGSN`=11, `fiveGDDNMF`=12, `vSMF`=13, `iMS-Node`=14, `eES`=15,
`mMS-Node`=16, `pCF`=17, `uDM`=18, `uPF`=19, `tSN-AF`=20, `tSNTSF`=21, `mB-SMF`=22),
`SubscriptionIDType::eND-USER-IMSI=1`, `TimeStamp` (real 9-byte BCD `YYMMDDhhmmssShhmm`, format
cited verbatim from the spec's own field comment).

### Decisions

1. **Reuse, not rebuild**: `libs/tcap-core`'s own generic X.690 BER primitives (`Tlv`,
   `encode_tlv`/`decode_tlv`/`decode_tlvs`, `encode_integer`/`decode_integer`,
   `UniversalTag`/`TagClass`) are reused directly, the same "reuse, not rebuild" precedent
   ADR-0067's own Decision 2 established for TAP3 -- a third real reuse of this file, not a new
   codec.
2. **Real, disclosed, narrow scope**: only the `ChargingRecord` fields this project's own CHF has
   genuine data for are populated -- `recordType` [0], `recordingNetworkFunctionID` [1],
   `subscriberIdentifier` [2], `nFunctionConsumerInformation` [3] (networkFunctionality only),
   `listOfMultipleUnitUsage` [5] (conditional on a real `ratingGroup`), `recordOpeningTime` [6]
   (Create only), `causeForRecClosing` [9]=0 (Release only), `localRecordSequenceNumber` [11],
   `chargingSessionIdentifier` [16], `invocationTimestamp` [40]. The remaining 36 real fields are
   deliberately NOT populated: most need a separate, unvendored spec
   (`pDUSessionChargingInformation`/`pDUContainerInformation` need TS 32.255,
   `iMSChargingInformation` needs TS 32.260), or don't apply to this project's own current CHF
   scope (no real MMTel/SMS/ProSe/edge/MBS/NSACF consumer exists yet).
3. **Additive, not a replacement**: this is a second, additional real encoded form stored
   alongside the existing structured ClickHouse columns (ADR-0058), which stay the real,
   queryable source for this project's own gap-detection analytics (`CdrWriter::detect_gaps`) --
   matching how a real billing-mediation system actually consumes CHF-CDRs, as an exported blob,
   not a live SQL query.

### Implementation

New `nfs/chf/src/cdr_asn1.{hpp,cpp}`: `encode_chf_cdr(const CdrRecord&, const std::string&
recording_network_function_id)` returns the real, byte-correct `[200] chargingFunctionRecord`
BER encoding, or an empty vector (real, disclosed, not an error) if the record's own
`node_functionality` has no real `NetworkFunctionality` mapping. `nfs/chf/src/cdr.hpp` gained a
`recording_network_function_id` field on `CdrRecord`; `cdr.cpp`'s `write()` now also appends a new
`asn1_cdr String` ClickHouse column (schema migration applied live to the real, already-running
`chf-test-clickhouse` container: `ALTER TABLE cdr ADD COLUMN IF NOT EXISTS asn1_cdr String DEFAULT
''`). `tests/conformance/test_cdr_asn1.cpp` (new, `ChfCdrAsn1.*`, 4 tests): unmapped
`NetworkFunctionality` encodes empty, top-level tag is real `[200]`, a full generic-release-record
field-by-field decode+assert (including a real BCD timestamp byte check), and
`listOfMultipleUnitUsage`/`usedUnitContainer` round-trip. All 4 pass. Reused the pre-existing
`conformance_tests` NF-private-code pattern (`test_ai_inference.cpp`'s own precedent) to compile
`nfs/chf/src/cdr_asn1.cpp` directly into the shared test binary.

### Real bug found and fixed via live verification (not self-consistency testing)

Live-verified with a real curl Create->Release flow against a real running CHF, then inspected
the actual stored row directly (`docker exec chf-test-clickhouse clickhouse-client --query
"SELECT ..., hex(asn1_cdr) FROM cdr WHERE charging_data_ref='chg-16'"`), manually decoding the
real hex byte-by-byte against this ADR's own field/tag scheme. Found: `recordingNetworkFunctionID`
(tag `[1]`) encoded as an empty IA5String (`8100`) in a real stored row, not CHF's real instance
UUID. Root cause: a SECOND, previously-unnoticed real CDR-write code path exists --
`charging_engine.cpp`'s own `write_converged_charging_cdr()`/`charge_one_usage()`, called from 5
real sites total (`main.cpp`'s HTTP Create and Update handlers, `cap_server.cpp`'s CAP path,
`diameter_server.cpp`'s two Diameter Gy paths) -- only `main.cpp`'s separate, manual Release-path
`CdrRecord` construction had been threaded with the new field. Fixed by adding a
`const std::string& recording_network_function_id` parameter to both `write_converged_charging_cdr`
and `charge_one_usage` (declarations in `charging_engine.hpp`, definitions in
`charging_engine.cpp`) and updating all 5 call sites: `main.cpp`'s two HTTP handlers now pass the
real `chf_instance_id`; `cap_server.cpp`/`diameter_server.cpp`'s three legacy-protocol paths pass
a literal `""` with a disclosed comment, since those paths' own `node_functionality` values
(`"CAP-gsmSSF"`/`"Diameter-Gy"`) have no real TS 32.298 `NetworkFunctionality` mapping anyway, so
the resulting `asn1_cdr` blob is already empty regardless of this field.

**Re-verified live after the fix**: rebuilt `chf` clean (raw log grep, not the potentially-masked
task-notification exit code), restarted against the real `chf-test-clickhouse` container
(`CHF_CLICKHOUSE_PASSWORD=chf_clickhouse_lab`, a pre-existing disclosed-deferred config field, not
a code change), confirmed real startup (`"connected to ClickHouse (CDF)"`,
`"registered with NRF (HTTP 201)"`). Real curl Create (`chg-17`, HTTP 201) -> Release (HTTP 204)
over mTLS against CHF's real `certs/` local dev CA. Direct ClickHouse hex-decode of both the real
Create and Release rows: `recordingNetworkFunctionID` (`81 24`, 36-byte IA5String) now decodes to
`344f52e6-7290-4be8-bf72-3f1c13ac3fea` -- an exact match against CHF's own real, independently
logged `nfInstanceId` from the same process's own startup log line, confirmed byte-for-byte, not
assumed.

Full `conformance_tests`: **325/325 pass** (up from 321, the 4 new `ChfCdrAsn1.*` tests), zero
regressions.

### What this ADR does NOT include

RAP/NRTRDE-class file-level mediation/export (no transport requirement implied by this scope);
any of the 36 real `ChargingRecord` fields listed above as deliberately not populated; a REL-19
re-verification of the ASN.1 structure (disclosed version gap, v18.8.0/Release 18 only);
CAP/Diameter paths' own always-empty-blob behavior (real, disclosed, not a defect -- those
protocols' own `NetworkFunctionality` values have no real TS 32.298 mapping). **This closes task
#108.**

