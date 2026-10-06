## ADR-0313: split the generated implementation, not the generated header

**Date:** 2026-09-06. **Status:** accepted. **Follows:** ADR-0312, which made this necessary.

Wiring all 60 AF-facing YAML files (ADR-0312) pushed the generated common-data group to ~3,000
types in one header and one source file. The result was measured, not estimated:

| | Peak RSS | Wall time |
|---|---|---|
| Unsplit `TS26510_CommonData_grp.cpp` (33,263 lines) | **10.0 GB** | **4m45s** |
| 400 types/part (7 parts) | 5.1 GB | 1m40s |
| **150 types/part (17 parts)** | **3.2 GB** | **41s** |

One translation unit needing 10 GB is why a 15 GB machine could not sustain `-j2` and produced ten
OOM kills in a single session. Each kill cost a restart; several of the "build succeeded" readings
in between were my own truncating pipelines (ADR-0312), which made the situation harder to diagnose
than it needed to be.

### The header must NOT be split, and that is the whole subtlety

ADR-0010 established that 3GPP's YAML files have genuine cross-file circular dependencies. The
generator therefore groups files into strongly connected components (Tarjan) and emits ONE
topologically-sorted header per SCC. A naive one-header-per-source-file version was already built
and already failed with "not declared in this scope", because `#pragma once` silently no-ops a
re-entrant include before the needed type is defined.

So "split the codegen output" has an obvious reading that breaks the build. What splits safely is
the **implementation**: once the whole header is included, every `to_json`/`from_json` body is
independent of every other. The 3,000 function pairs are dealt across 17 `.cpp` files that each
include the same complete header. Correctness is unchanged by construction -- identical
declarations, identical order, identical visibility -- and 580/580 tests confirm it.

### Two properties that make it safe to rebuild

- **Stable partitioning.** Types are dealt in the same deterministic order the header declares
  them, so a given type's implementation stays in the same part across regenerations. A schema
  change does not reshuffle every part and invalidate every object file.
- **Clean-slate regeneration already existed.** `libs/sbi-generated/CMakeLists.txt` does
  `file(REMOVE_RECURSE)` before generating -- itself the fix for a past duplicate-symbol bug -- so
  the previous monolithic `.cpp` cannot linger beside the new parts and define every symbol twice.
  This change would have been a nasty duplicate-definition failure without that.

### Why 150 and not "smaller is better"

The target was never "smaller"; it was that parallel compilation fits. At 400 types/part the worst
file still needed 5.1 GB, so two concurrent parts would want 10.2 GB and remain at the OOM
killer's mercy -- the exact failure this exists to remove. 150 keeps each part near 3 GB, so `-j2`
is genuinely safe. More object files is a cheap price; OOM kills are not.

### Result

First successful full `-j2` build of the session: exit 0, zero errors, no OOM kill, 580/580 tests.
This also removes the tax every one of the 56 remaining NEF route slices would otherwise have paid.

