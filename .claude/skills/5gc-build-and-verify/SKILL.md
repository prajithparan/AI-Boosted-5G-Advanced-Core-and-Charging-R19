---
name: 5gc-build-and-verify
description: Build, test, format and benchmark this 5GC repo without losing hours to its specific traps - corrupted ninja logs forcing 2-hour rebuilds, harness-reaped background builds, the fast syntax-only loop, clang-format-18, and the UDR row that must be cleared before ctest. Use whenever building, running ctest, or benchmarking here.
---

# Building and verifying the 5GC repo

A full build is **144-173 targets and takes ~2 hours**, almost all of it the generated
`sbi_generated` sources. Everything below exists to avoid paying that more than once.

## Always launch builds detached

Plain `run_in_background` bash tasks get reaped by the harness mid-build. **Every kill corrupts
`build/.ninja_log`**, and a corrupted log makes ninja re-run all ~144 targets from scratch
(`ninja: warning: premature end of file; recovering` at the top of the log is the tell). Two kills
in a row cost four hours and produce nothing.

```bash
SCR=<scratchpad>
setsid nohup cmake --build build -j8 > $SCR/build.log 2>&1 < /dev/null & disown
```

`setsid` detaches the process group so harness task-kills cannot reach ninja. Then watch it with a
Monitor that polls `ps -eo comm | grep -qE "^(cmake|ninja)$"` and reports
`grep -cE '^FAILED:' $SCR/build.log` when the process is gone.

Do **not** `pkill -f "cmake --build build"` to stop a build — that re-corrupts the log.

## Iterate with syntax-only checks, not builds

A single TU check is ~3 minutes against a ~45-minute incremental build. Generate the command from
the compile database once per file:

```python
import json, shlex
db = json.load(open('build/compile_commands.json'))
e = next(x for x in db if x['file'].endswith('nfs/smf/src/main.cpp'))
parts = shlex.split(e['command'])          # drop '-o <obj>' and '-c', insert '-fsyntax-only'
```

Write it to a script that `cd`s to `e['directory']` first. Use this to get new code compiling
*before* spending a build cycle. It catches the common failure here: asn1c generated types are
forward-declared, so a new NGAP field needs its own `#include <TypeName.h>` (e.g.
`AssociatedQosFlowItem.h`, `QosFlowListWithCause.h`).

To validate a large edit without touching a file a running build will compile, copy it beside the
original (`nfs/smf/src/main_wip.cpp`) and syntax-check the copy — NF `CMakeLists.txt` files list
sources explicitly, so a stray file is inert. Relative `#include "..."` only resolve if the copy
sits in the same directory.

## Formatting

The binary is **`clang-format-18`**; plain `clang-format` is not on PATH and fails with a
misleading "No such file or directory" per file.

```bash
clang-format-18 --dry-run -Werror <file>    # check
clang-format-18 -i <file>                   # fix
```

## Before a full ctest

`UdrIntegration.AmfContextLifecycle` fails on any second local run — it PUTs a resource with no
DELETE in the spec, so it passes once per database reset. Not a regression:

```bash
docker exec docker-postgres-udr-1 psql -U udr -d udr -tAc \
  "DELETE FROM udr_amf_context WHERE ue_id='imsi-999700000000001'"
```

Also kill any manually started NF processes first — they squat the ports tests spawn onto.

**Never quote a ctest total you did not measure.** This repo has twice carried a stale number
forward into ADRs. Run `ctest` and quote its output, or say plainly that only targeted
`--gtest_filter` subsets were run.

## Test flakes that are not regressions

Running many NF tests in one `--gtest_filter` process occasionally fails one on port contention;
re-running the same filter passes. Confirm by running the single test alone before calling it a
regression.

## Shell traps that have bitten here

- `pgrep -c "[l]d\b"` matches **thermald**. Match process names exactly:
  `ps -eo comm | grep -cE "^(cmake|ninja)$"`.
- `pkill -f` and `pgrep -f` match the Bash wrapper's own command line. Prefer explicit PIDs.
- A `cd` in one Bash call persists into later calls — use absolute paths or `cd` back.

## Benchmarking

`scripts/run-baseline-benchmark.sh` runs the baseline; `tools/sbi-loadgen` is the harness
(closed-loop by default, `--rate` for open-loop / coordinated-omission-corrected).

OAuth2 tokens live 3600 s. A benchmark that fetches one token up front and then drifts past it
will happily measure the **401 rejection path at full speed** and report plausible numbers. The
script now re-authenticates per case and asserts every run recorded only 200s — keep that guard.
Cross-check any run against the server-side TS 28.552 counters
(`nrf_nfs_disc_req_total` vs `nrf_nfs_disc_succ_total`).
