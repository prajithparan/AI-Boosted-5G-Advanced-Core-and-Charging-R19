## ADR-0250: YAML/TS cross-checking made standard -- and two errors it immediately caught

**Status:** Closed (work pushed; last citing commit f0ac549 on origin/main, 2026-10-06).

### Context

User instruction, mandatory going forward: **cross-check the respective YAML files against the TS
documents before drawing any conclusion.** This corrects a real methodological weakness in
ADR-0247/ADR-0248: the CHF conclusion was drawn from TS 28.552's prose alone, and the
`N2SmInfoType` count from YAML alone. Neither was checked against the other, and a conclusion that
rests on one source when two exist is not verified -- it is half-verified and stated with full
confidence, which is worse.

Applying the rule immediately caught **two errors in already-committed documents**. Recording both
rather than quietly editing them.

### Error 1 (corrected): clause 5.7.2 is NOT implementable

`docs/PERFORMANCE_MAPPING.md` and ADR-0247 stated that only TS 28.552 clause 5.7.1 depends on a
VNFM and that "only 5.7.2 (connection data volumes) is implementable without a VNFM".

**That is wrong.** Reading each of the four 5.7.2 measurements' own `c)` source fields
individually: `DataVolum.InBytes`, `.OutBytes`, `.InPackets` and `.OutPackets` are each obtained
from `ByteIncomingVnfExtCp` / `ByteOutgoingVnfExtCp` / `PacketIncomingVnfExtCp` /
`PacketOutgoingVnfExtCp` -- VNF measurements delivered by the same VNFM as 5.7.1's. The **entire**
clause 5.7 depends on the ETSI NFV-MANO layer this project does not have. The original error came
from reading 5.7.1's `c)` field, then inferring 5.7.2's from its title.

### Error 2 (refined): CHF *is* a real 3GPP managed object

ADR-0247 emphasised that TS 28.552 has no CHF clause -- true, and now stated more strongly: a
literal text search of TS 28.552 v19.8.0 returns **zero** occurrences of "CHF" anywhere in the
document. But the framing risked reading as "CHF is absent from 3GPP management entirely", and the
YAML says otherwise: **`ChfFunction` is a real managed object class in
`specs/5G_APIs-REL-19/TS28541_5GcNrm.yaml`**.

The accurate, cross-checked statement is narrower and more useful: **CHF is modellable and
manageable under the 5GC NRM; what does not exist is any performance measurement defined against
it.** The practical consequence for ADR-0049's mandate is unchanged -- CHF measurement needs this
project's own counters following 28.552's request/success/per-cause-failure *pattern* with no
conformance claim -- but the reason is now precise instead of overstated.

### What the cross-check confirmed (conclusions that survived)

- **NRF's `NFS.*` mapping holds.** Each measurement corresponds to a real operation in the
  vendored YAML: `NFS.RegReq`/`RegSucc` -> `RegisterNFInstance`, `NFS.UpdateReq`/`UpdateSucc` ->
  `UpdateNFInstance`, `NFS.DiscReq`/`DiscSucc` -> `SearchNFInstances`
  (`TS29510_Nnrf_NFManagement.yaml`, `TS29510_Nnrf_NFDiscovery.yaml`). The 0-of-13-conformant
  finding stands.
- **`N2SmInfoType` = 26 values** stands: the YAML enum is the stage-3 definition, and SMF now
  implements 4 of them.

### A real opportunity the cross-check surfaced

**`TS28532_PerfMnS.yaml` is already vendored** -- the Performance Management MnS API has a real R19
YAML in this repo, as do `TS28532_FileDataReportingMnS.yaml` and
`TS28532_StreamingDataMnS.yaml`. ADR-0247 listed TS 28.532 as a spec we would "need later" for a
reporting interface; in fact the machine-readable interface definition was already present and
unexamined. Under this project's own standing full-YAML-coverage rule these are in scope for
implementation, which makes a real, spec-defined PM reporting interface reachable rather than
hypothetical.

### Decision

Cross-checking YAML against TS text (and vice versa) before stating a conclusion is now standard
practice for this project, not an optional extra step. Where the two disagree or only one covers a
question, say which source the conclusion rests on.

