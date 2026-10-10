## ADR-0294: the capability sweep the summary table said was missing, actually done

**Date:** 2026-09-05. **Status:** accepted. **Evidence:** `docs/CAPABILITY_GAP_ANALYSIS.md`, new
final section.

The user's item 3 was "free5c and open5c benchmark on product capabilities compared in earlier
sessions, please check this mandated". Checking found the mandate half-satisfied: the original
ADR-0075 sweep covered 9 NFs, and `CAPABILITY_GAP_ANALYSIS.md`'s own summary table has carried
"Not yet swept against free5GC/open5GS source" against NSSF, BSF, NEF, SCP, 5G-EIR, SMSF, GMLC and
LMF ever since they were built. That is now done, against real cloned/extracted source.

### The three findings that matter

1. **NEF's AF-facing half does not exist, and the audit that should have caught it was scoped too
   narrowly.** free5GC serves `/3gpp-traffic-influence/v1` (TS 29.522) and
   `/3gpp-pfd-management/v1` (TS 29.122) as real northbound APIs. This repository has 16
   `TS29122_*.yaml` and 44 `TS29522_*.yaml` files on disk; 2 are wired, and only as DTO sources.
   ADR-0193's project-wide YAML audit matched `N<nf>_*` filenames only, so those 58 files were
   never in its scope -- its "zero Tier-A gaps for NEF" verdict was narrower than it read. The
   audit's scope is corrected, not just its result. Compounding it: our NEF's only outbound HTTP is
   NRF registration; free5GC's NEF really writes influence data and PFDs to UDR. Traffic-influence
   data stored in an in-process map influences nothing.

2. **NSSF ignores data it already holds.** We accept, store, authorize and notify on per-AMF
   `SupportedNssaiAvailabilityData`, and then the selection decision consults a static seed catalog
   instead. Also newly named: `nsiInformationList` is never returned (both references produce it;
   it is the field that tells the AMF which NRF serves the slice instance), the TAI is ignored, and
   `candidateAmfList`/`targetAmfSet` are never produced, so NSSF can never trigger AMF relocation.
   ADR-0183's disclosure gestured at this; it was less specific than the facts warrant.

3. **Five of the eight NFs have no reference at all** -- 5G-EIR, SMSF, GMLC, LMF and NSACF exist in
   neither free5GC nor open5GS. Recorded as a lead, not a win: "nothing to compare against" is not
   evidence of correctness, and their own disclosed gaps are untouched by it. BSF is the first NF
   in this project to come out of a reference sweep with **no gap found** -- and free5GC is the one
   that diverges there, serving a `GetIndPCFBinding` operation the R19 YAML does not define.

### What this explicitly is not

Not a benchmark. No performance number was produced, and the commercialization mandate's
"must exceed free5GC" (ADR-0049) still has zero measurements behind it. Capability and performance
are separate claims and this ADR makes only the first one.

