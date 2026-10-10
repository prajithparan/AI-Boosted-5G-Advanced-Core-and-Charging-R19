## ADR-0312: all 60 AF-facing YAML files wired -- four collisions, and a build hazard of my own making

**Date:** 2026-09-06. **Status:** accepted. **Follows:** ADR-0302 (which wired the first of 58).

ADR-0294 found NEF's AF-facing surface entirely missing: 58 of the 60 `TS29122_*`/`TS29522_*` files
sat on disk unwired. ADR-0302 wired one. This wires the remaining 57 in a single batch.

### Why one batch

Each codegen addition forces a full regeneration and a full rebuild of every NF, because all of
them include the generated common-data header. At ~20 minutes per rebuild on this machine, 57
sequential additions was not a plan. Batching also makes every type collision surface in ONE pass
instead of 57.

That trade was right for collision discovery and wrong for memory: it front-loaded the whole cost
into one enormous rebuild and produced repeated OOM kills. A staged approach (ten files at a time)
would have been slower in total but steadier. Recorded because the next person facing this choice
should know both halves.

### Four collisions in ~3,000 types

| Type | Referenced by | Resolved to |
|---|---|---|
| `IptvConfigData` | `nfs/udr` | `_Application_Data` |
| `ServiceParameterData` | `nfs/udr` | `_Application_Data` |
| `UeIdReq` | `nfs/nef` | `_Nnef_UEId` |
| `LocUpdateData` | `nfs/gmlc`, `tests/conformance/test_gmlc_dtos.cpp` | `_Ngmlc_Location` |

Each was chosen by which service the route actually serves, NOT by pattern: `UeIdReq_UEId`
(AF-facing, TS 29.522) and `UeIdReq_Nnef_UEId` (NF-facing, TS 29.591) are genuinely different
schemas for a related concept, and picking wrong compiles cleanly then mis-parses real requests.

Four out of ~3,000 is far better than ADR-0301 predicted, and the reason is structural: TS 29.122
and TS 29.522 mostly describe AF-facing concepts with no NF-facing twin. Real overlap happens only
where the same data appears in both a provisioning API and a repository.

**Method worth reusing:** rather than discovering collisions one 20-minute rebuild at a time, a
static sweep -- every `sbi_gen::` reference in the tree against every generated definition -- found
the complete set in seconds. My first sweep scanned only `nfs/` and `bss/` and missed a test file,
which cost another full rebuild; the corrected sweep covers `tests/` and `libs/` too.

### Only ~3 new opaque fallbacks

ADR-0301's two generator fixes (constraint-only `allOf` members, and merging sibling `properties`)
absorbed almost everything 57 new specs threw at the generator. That is evidence those two defects
were the systematic ones rather than the head of a long tail.

### A build hazard I introduced and should not repeat

Throughout this work I drove builds as `cmake --build ... | grep -E "error:" | head -8`. `head`
exits after its match limit, closing the pipe; ninja takes SIGPIPE and can die **mid-build**; and
the pipeline's exit status comes from the trailing `echo`, so it reports SUCCESS. Several of my own
"build complete, zero errors" readings were really "the build stopped early and printed nothing" --
which I then misattributed to OOM kills.

Related: a `grep -c "error:"` returning 0 exits 1, so a task reported "failed" while the build had
in fact succeeded.

The rule this leaves: **a build's result is ninja's own exit code and its complete output**, not
whatever the last stage of a shell pipeline returned. Redirect to a file, then inspect.

### State after this change

- 60 of 60 AF-facing YAMLs wired (was 3); 94 generated units (was 6).
- Build exits 0 with zero errors; **580/580 tests pass**.
- Generated common-data header is now 29,093 lines, and every one of the 22 NFs includes it. Each
  compile touching it peaks above 3 GB, which is why this machine cannot sustain `-j2`.

### The prerequisite for the remaining work

Routes exist for `TS29522_TrafficInfluence` only (ADR-0302, six operations brokering to UDR). The
other 56 files have generated DTOs and no handlers -- that is the substantive remainder of NEF, and
it is genuinely per-file work.

Before that work, **splitting generated output per source file** should come first. It is no longer an
optimisation: it would cut per-compile memory, cut rebuild scope when one YAML changes, and make
the collision class above structurally impossible, since `UeIdReq` from two specs would live in
separate translation units rather than needing suffixes to coexist. Every one of the 56 remaining
slices otherwise pays the current tax.

