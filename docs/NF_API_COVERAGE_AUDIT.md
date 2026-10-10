# Full NF API coverage audit -- every routed endpoint vs every R19 YAML path

**ADR-0252.** Method: for each NF, extract every path from its R19 YAML (`specs/5G_APIs-REL-19/`),
then test that path against the string table of the **compiled binary** (`build/nfs/<nf>/<nf>`).
Binary strings rather than regex over source: route registration concatenates API-root constants
and assigns path patterns to variables, so source regex cannot see the real routes -- an earlier
regex attempt reported 12 routes for UDR, which has 204 `add_route` calls. Abandoned and replaced.

**YAML is the prime authority here; the TS is reference.** Paths come from the YAML, never from
operationIds (`GetSupiOrGpsi` lives at `/{ueId}/id-translation-result` -- deriving a path from an
operation name produced a false negative earlier and is not done anywhere in this audit).

## Result: 12 of 16 NFs at 100% path coverage

| NF | Unrouted YAML paths | Status |
|---|---|---|
| amf | 0 | **complete** |
| bsf | 0 | **complete** |
| chf | 0 | **complete** |
| eir | 0 | **complete** |
| gmlc | 0 | **complete** |
| lmf | 0 | **complete** |
| nef | 0 | **complete** |
| nssf | 0 | **complete** |
| pcf | 0 | **complete** |
| scp | 0 | **complete** |
| smsf | 0 | **complete** |
| ausf | 1 | `/rg-authentications` -- disclosed deferred (5G-RG, out of Tier-1 5G-AKA scope) |
| udm | 1 | `/{supi}/am-data/update-sor` -- **disclosed deferred** (ADR-0257): needs the TS 24.501 9.11.3.51 SOR header encoding; that spec is not in `specs/` |
| smf | 0 | **complete** -- both closed by ADR-0257 |
| nrf | 5 | `/shared-data*`, `/scp-domain-routing-info*` -- disclosed deferred |
| udr | 1 (was 23) | 4 closed by ADR-0253, 10 by ADR-0254, 4 by ADR-0255, 4 by ADR-0256; remaining: `service-specific-authorization-data` -- **disclosed deferred**, not a gap (ADR-0160, user-confirmed twice) |

**Total: 22 unrouted paths of 328 -- and every single one is now an explicit, reasoned deferral. Zero undisclosed gaps, and zero "genuinely absent" paths.** (NRF 5, AUSF 1, UDR 1, UDM 1 across 8 distinct resources; SMF's 2 closed by ADR-0257.)

## The 32, verbatim

**This block is ADR-0252's original snapshot and is deliberately not
edited as paths are closed** -- it is the audit's baseline evidence. The live remaining count is
the table above (28 as of ADR-0255); the per-family "Update" sections at the end of this
document record which of these 32 have since been closed and by which ADR.

```
/rg-authentications
/scp-domain-routing-info
/scp-domain-routing-info-subs
/scp-domain-routing-info-subs/{subscriptionID}
/shared-data
/shared-data/{sharedDataId}
/pdu-sessions/{pduSessionRef}/transfer-mo-data
/sm-contexts/{smContextRef}/send-mo-data
/{supi}/am-data/update-sor
/aiot-data/af-authorization-data
/aiot-data/aiot-device-profile-data
/aiot-data/aiot-device-profile-data/{aiotDevPermId}
/application-data/bdtPolicyData
/application-data/bdtPolicyData/{bdtPolicyId}
/application-data/influenceData
/application-data/influenceData/subs-to-notify
/application-data/influenceData/subs-to-notify/{subscriptionId}
/application-data/influenceData/{influenceId}
/application-data/iptvConfigData
/application-data/iptvConfigData/{configurationId}
/application-data/pfds
/application-data/pfds/{appId}
/application-data/serviceParamData
/application-data/serviceParamData/{serviceParamId}
/application-data/subs-to-notify
/application-data/subs-to-notify/{subsId}
/data-restoration-events
/exposure-data/subs-to-notify
/exposure-data/subs-to-notify/{subId}
/exposure-data/{ueId}/access-and-mobility-data
/exposure-data/{ueId}/session-management-data/{pduSessionId}
/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}
```

## Verification performed (not assumed)

Every "unrouted" finding above was confirmed against the source, because the binary method has a
known false-negative mode -- see below. Confirmed by grep returning **zero** occurrences:
UDR `application-data`/`influenceData`, SMF `send-mo-data`, UDM `update-sor`. Confirmed
present-but-only-in-a-comment (i.e. documented as deferred, not implemented): NRF `/shared-data`
and `/scp-domain-routing-info` (`nfs/nrf/src/main.cpp` lines 14-16), AUSF `/rg-authentications`
(`nfs/ausf/src/main.cpp` lines 26, 648).

## Known limitation of this method, stated up front

**Dynamically-assembled routes produce false negatives.** UDM registers its four ack endpoints in a
loop over `{"sor-ack", "upu-ack", "subscribed-snssais-ack", "cag-ack"}`, so the full path literal
`/{supi}/am-data/sor-ack` never appears in the binary even though the route exists. Caught by
cross-checking against source before publishing, and those four are **excluded** from the 32.

15 further paths were flagged as likely-dynamic and are **not** counted as gaps: 4 UDM (confirmed
loop-registered), 3 SMF (`/pdu-sessions/{ref}/modify|release|retrieve` -- `pdu-sessions` appears 4x
in SMF source), 8 UDR (`provisioned-data` appears 18x, `smf-registrations` 2x in UDR source).
The SMF and UDR ones are **consistent with being routed but not individually confirmed** -- stated
as unconfirmed rather than counted either way.

## Highest-value finding: UDR's 23

UDR is the one NF with a large, real, undisclosed coverage gap. The missing set is coherent rather
than scattered -- three whole resource families:
- **`/application-data/*`** (12 paths): bdtPolicyData, influenceData + its subs-to-notify tree,
  iptvConfigData, pfds, serviceParamData, subs-to-notify. Zero occurrences in UDR source.
- **`/exposure-data/*`** (4 paths) and **`/aiot-data/*`** (3 paths, R19 Ambient IoT).
- 4 singles including `/data-restoration-events` and service-specific-authorization-data.

This matters against the free5GC/open5GS parity mandate: `/application-data/influenceData` is the
traffic-influence store a real NEF/AF path depends on, and NEF is built here (all 14 YAML files).

## Update (ADR-0253): influenceData family closed, and a flaw in this audit's own heuristic

UDR's `/application-data/influenceData` family is now implemented -- **4 paths, 9 operations**.
Remaining UDR gap: **19 paths**.

**A flaw in the tier-2 heuristic, found while verifying that number and stated rather than left to
mislead.** After the family landed, the script reported UDR's unrouted count dropping 23 -> 11 and
its "likely-dynamic" count rising 8 -> 16. That improvement is **not real**: the literal
`application-data` now exists in the binary, so still-unimplemented siblings
(`/application-data/pfds`, `bdtPolicyData`, `iptvConfigData`, `serviceParamData`,
`subs-to-notify`) started matching the all-segments-present test and were reclassified as
"probably routed dynamically".

The heuristic gets **weaker as coverage grows**, because shared path prefixes accumulate in the
binary. The reliable check is a full-literal `grep -F` for the exact path, which was run for each
of the nine paths above and is the basis for the 4/19 figures. Treat tier-2 counts as a hint only.

## Update (ADR-0254): `/application-data/*` complete

All 14 `application-data` paths are now routed, verified by exact-literal match against the
binary (not the tier-2 heuristic, which ADR-0253 showed is unreliable once prefixes accumulate).

**UDR: 23 -> 9 unrouted.** What remains: `/exposure-data/*` (4), `/aiot-data/*` (3, R19 Ambient
IoT), `/data-restoration-events`, and
`/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}`.

## Update (ADR-0255): `/exposure-data/*` complete

All 4 `exposure-data` paths are now routed (10 operations), verified by exact-literal match
against the binary. The family is NOT shaped like its `application-data` sibling and was read per
path: `session-management-data` has no PATCH, the `subs-to-notify` collection has no GET list, and
the individual subscription has no GET. Those three operations are absent from the spec and are
therefore absent here.

**UDR: 9 -> 5 unrouted.** What remains: `/aiot-data/*` (3, R19 Ambient IoT),
`/data-restoration-events`, and
`/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}`.

## Update (ADR-0256): AIoT data and data-restoration closed -- and a mis-categorisation in this audit corrected

`/aiot-data/*` (3 paths) and `/data-restoration-events` (1) are now routed, verified by
exact-literal match. **UDR: 5 -> 1 unrouted.**

**This audit got one row wrong, and the error is worth naming rather than quietly fixing.**
`/subscription-data/{ueId}/service-specific-authorization-data/{serviceType}` was listed above as
part of UDR's "large, real, undisclosed coverage gap". It is not undisclosed and it is not a gap:
**ADR-0160 investigated it in depth and the user explicitly chose to defer it -- twice**, once on
which response schema to return and again, after a deeper finding, on whether to build it at all.
It belongs in the same category as AUSF's `/rg-authentications` and NRF's `/shared-data` -- rows
this audit correctly marked "disclosed deferred" -- and this audit failed to check the ADR history
for it while doing so for the others.

The live reason it stays deferred is ADR-0160's, and one of the two reasons recorded in the source
comments had itself gone stale: those comments said it was blocked by a complex-object query
parameter with no parsing precedent, which ADR-0165's `GetNiddAuData` has since established. The
stale half is corrected in `nfs/udr/src/main.cpp` and `nfs/udr/schema.postgres.sql`; the resource
stays deferred on ADR-0160's still-valid schema/write-path grounds.

**UDR's real remaining count is therefore 1 deferred path and 0 undisclosed gaps.**

## Update (ADR-0257): the audit is discharged

`SendMoData`/`TransferMoData` are routed. **SMF: 2 -> 0.**

`/{supi}/am-data/update-sor` was re-examined rather than implemented on ADR-0234's stated
reasoning, and both of that ADR's blockers turned out to be wrong: the `CounterSoR` state machine
does exist (in AUSF, `kausf_store.use_counter()`, with real TS 33.501 6.14.2.3 wrap-around), and
the missing steering-list content is not blocking because `steeringContainer` is optional
everywhere it appears. The real blocker is that a UDM->AUSF relay needs a `sorHeader`, whose
construction is TS 24.501 9.11.3.51 -- **a spec not present in `specs/`**. Re-deferred with that
accurate reason.

**Every one of the 328 paths is now either implemented or an explicit, reasoned deferral. This
audit has no open findings.**

## Update (2026-10-07): re-measured with `tools/specs/nf_api_coverage.py` -- and a correction to "no open findings"

The audit above had no script in the repository, only a method description. `tools/specs/nf_api_coverage.py` now
reproduces it: every path of an NF's R19 YAML (`TS*_N<nf>_*.yaml`) against the built binary (`build/nfs/<nf>/<nf>`),
reading files only (no process started, no port opened). **Rule:** a path counts as routed when some path-like string
in the binary *ends with it* (placeholder names ignored). Two simpler rules were tried first and rejected: substring
matching counts `/pdu-sessions` as routed because `/pdu-sessions/{ref}/deliver` is (this is the flaw this document
already admitted under ADR-0253), and exact matching reports `/nf-instances` as unrouted because the compiler stored it
only as the tail of a longer string.

**Measured 2026-10-07 on `build/` at `6b9725d`: 23 NFs, 89 spec files, 424 paths, 34 flagged.** The 16 NFs of the original audit
give 359 paths (the original counted 328 -- the difference is not explained here, so the two totals are not comparable).
Not audited: `li-admf`, `li-mdf`, `li-mdf3` (no TS 29-series YAML) and the `hello-nf` template.

| Flagged | Count | Where | What it is |
|---|---:|---|---|
| Run-time-assembled route, so a string scan cannot see it | 18 | udm 4 (`-ack` loop), udr 8 (`provisioned-data/*` built from `provisioned_data_path_pattern`, `smf-registrations/{pduSessionId}`), udsf 6 (`records`/`timers` built from `root + ...`) | Tool limit, not a gap. Each was read in source; the individual operations were not each exercised here. |
| Deferral already disclosed in source or ADRs | 16 | ausf 1, nrf 5, udm 1 (`update-sor`), udr 2 (`service-specific-authorization-data`, `/subscription-data/shared-data` = GetSharedData), smf 4 (`/pdu-sessions` I-SMF collection: modify, release, retrieve, create), dccf 1 (`/transfer-data-sub`), mfaf 1 (`/mfaf-data-analytics`), nwdaf 1 (`/subscriptions/{subscriptionId}/unsubscribe-info`: the whole `Nnwdaf_MLModelTraining` API is disclosed as Phase D in `nfs/nwdaf/src/main.cpp`) | Real, known, reasoned. |
| Not disclosed anywhere | 0 | -- | None found among the 34. A first draft of this section listed the NWDAF path as undisclosed; that was wrong, the source header names the API. |

**Tool limit that hides gaps in the other direction.** A short path shared by several of an NF's APIs (`/subscriptions`,
`/subscriptions/{subscriptionId}`) is counted as routed as soon as *any* of that NF's services registers it. NWDAF shows this: only
`unsubscribe-info`, the one path unique to `Nnwdaf_MLModelTraining`, is flagged, although that API's `/subscriptions` and
`/subscriptions/{subscriptionId}` are equally unbuilt. So a clean row means "no unique path is missing", not "every operation
exists"; the same can hold for any NF with several services (NEF 14, PCF 10, UDM 10, NWDAF 10). Closing that needs a per-service
check, which this tool does not do.

**Correction.** The 2 Sep conclusion "every one of the 328 paths is implemented or an explicit, reasoned deferral; this
audit has no open findings" is not supported by this measurement. Five of the paths above appear to have been counted as routed by the old
substring heuristic (inferred from its published table, not re-run) although they are deferred: the four SMF `/pdu-sessions` operations and UDR GetSharedData. They are
disclosed in source, so the claim "zero undisclosed gaps" held then, but "no open findings" did not. NWDAF was not among the original 16 NFs. Counts are paths, not path x method operations.
