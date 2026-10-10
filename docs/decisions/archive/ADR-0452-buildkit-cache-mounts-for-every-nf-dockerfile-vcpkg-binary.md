## ADR-0452: BuildKit cache mounts for every NF Dockerfile (vcpkg binary cache + ccache)

**Status:** Closed (work pushed; last citing commit 11fd4de on origin/main, 2026-10-06).

User-directed, from a direct question asked mid-ADR-0451's repeated failures: "give me recommendation
to improve CI faster." Investigated the actual CI setup before recommending anything (not generic
advice) -- most standard levers (ccache, vcpkg binary caching, single-runner-by-design to prevent the
box's own 15GB from thrashing, auto-cancel of superseded runs) are already real and already
documented in `.github/workflows/ci.yml`'s own comments (ADR-0363, ADR-0124/0132, ADR-0138). The one
confirmed, unaddressed gap: **every one of the 28 `deploy/docker/*.Dockerfile` files does a fully
cold `vcpkg install` with zero caching of any kind**, confirmed by grepping all of them for
`ccache`/`cache_from`/`--mount=type=cache` and finding none. Since `vcpkg.json` is one shared
manifest (already disclosed in `nrf.Dockerfile`'s own comment -- "`vcpkg install` pulls in every
dependency for ANY target's configure step, not just the one being built here"), this meant every
single NF image build cold-compiled the *entire* 102-package manifest, including ONNX Runtime
(large, slow, and not even called by most NFs), from scratch, every time -- directly responsible for
several of ADR-0451's six failed UDSF verification attempts.

**Fix.** Added BuildKit cache mounts to all 28 Dockerfiles (`postgres-chf.Dockerfile` excluded, not
a C++ NF build): a `# syntax=docker/dockerfile:1` directive (required to unlock the `--mount=`
extended `RUN` syntax), a new `RUN apt-get install ccache` step, and the existing `RUN cmake -S . -B
build ...` step rewritten to mount two caches --

```
RUN --mount=type=cache,id=vcpkg-bincache,target=/root/.cache/vcpkg-bincache,sharing=locked \
    --mount=type=cache,id=ccache,target=/root/.cache/ccache,sharing=locked \
    VCPKG_BINARY_SOURCES="clear;files,/root/.cache/vcpkg-bincache,readwrite" \
    CCACHE_DIR=/root/.cache/ccache \
    cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -D5GC_BUILD_TESTS=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build build --target <nf>
```

Both mounts use a **stable `id=`, identical across all 28 files** -- deliberately not scoped per
image -- so the FIRST NF image built on a host populates the cache for every other NF's build
afterward: `vcpkg-bincache` is the same `VCPKG_BINARY_SOURCES` binary-caching mechanism
`.github/workflows/ci.yml` already uses for its own native (non-Docker) build, just mounted into the
Docker build via BuildKit instead of GitHub's `actions/cache`; it skips recompiling any package whose
exact ABI hash was already built by *any* other NF's image build on that host, not just a prior build
of the same image. `ccache` does the equivalent for this project's own source. `sharing=locked`:
vcpkg's binary-cache directory is documented as unsafe for concurrent writers, and `docker compose up
--build` can build several NF images in parallel.

Applied mechanically via a one-off Python script (not 28 manual edits -- the `RUN apt-get install`
and `RUN cmake` blocks are byte-identical across every file except the final `--target <nf>` name,
confirmed by grep before writing the script), dry-run compared against all 28 files before applying,
then every file individually validated with `docker buildx build --check -f <file> .` -- all 28
passed clean, no warnings.

**Verification, completed, with a real bug found and fixed along the way.**

First attempts to measure the real before/after timing hit two real, unrelated blockers before
reaching a useful result: (1) `docker compose build udsf` run to populate the cache for the first
time was killed twice by Claude Code's own background-process memory-pressure reaper while
compiling ONNX Runtime (the same reaper that intervened during ADR-0451's own verification), and
(2) once memory pressure cleared and the build was retried a third time, it **failed outright** --
not from memory, but from a real CMake configure error:

```
CMake Error at /opt/vcpkg/scripts/buildsystems/vcpkg.cmake:615 (_add_executable):
  Impossible to link target 'chf' because the link item 'ONNX::onnx',
  specified without any feature or 'DEFAULT' feature, has already occurred
  with the feature 'WHOLE_ARCHIVE', which is not allowed.
```
(and identically for `nwdaf`). Both targets' `$<LINK_LIBRARY:WHOLE_ARCHIVE,ONNX::onnx>` usage is
deliberate and documented (`nfs/chf/CMakeLists.txt`/`nfs/nwdaf/CMakeLists.txt`'s own comments, a
real fix for ONNX Runtime dropping operator schemas otherwise) -- this was a new failure mode, not
a known issue, and not obviously caused by the caching change itself. Investigated rather than
assumed:

- Ruled out `-D5GC_BUILD_TESTS=OFF` (every Dockerfile's own flag): reproduced the exact same
  `cmake -S . -B build` configure, with real `chf`+`nwdaf`+onnx/onnxruntime all genuinely present,
  directly on the host -- succeeded cleanly.
- Ruled out the binary-cache restore itself being corrupted: inspected the actual
  `vcpkg-bincache` BuildKit cache mount contents directly (via a throwaway diagnostic
  `docker buildx build` mounting the same cache id) -- all 102 expected package archives present,
  correctly sized (the `onnxruntime` entry alone is a genuine ~556MB, not truncated), zero
  zero-byte/tiny files. Also reproduced a **genuine fresh restore-from-binary-cache** (not reusing
  an already-installed dir) on the host into an empty `VCPKG_INSTALLED_DIR` -- also succeeded
  cleanly.
- Ruled out the new `ccache` compiler-launcher flags: added the identical
  `-DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache` to the same host
  reproduction -- still succeeded cleanly.
- Found the real difference: `cmake --version` inside the actual failing container reported
  **3.28.3** (Ubuntu 24.04's apt-packaged version). The host dev environment and
  `.github/workflows/ci.yml` both explicitly use **4.4.0**, installed via
  `~/.local/build-tools-venv` and required on `PATH` (`ci.yml`'s own "for t in ninja cmake
  clang-18 ... " PATH-setup step) -- every real build of this project, anywhere, until tonight,
  has run its top-level configure under 4.4.0, never 3.28.3. CMake's `$<LINK_LIBRARY:FEATURE,...>`
  generator-expression validation (a relatively young feature, introduced in CMake 3.24) evidently
  differs enough between these two versions that 3.28.3 raises this conflict and 4.4.0 does not --
  a real, pre-existing version sensitivity in this exact ONNX-linking pattern that nothing had ever
  exercised before, because no Docker build had ever gotten far enough with a populated cache to
  reach a full successful top-level configure before tonight.

**Fix:** all 28 Dockerfiles now install CMake 4.4.0 directly, via the identical official Kitware
release tarball (`https://github.com/Kitware/CMake/releases/download/v4.4.0/
cmake-4.4.0-linux-x86_64.tar.gz`) that vcpkg itself already downloads internally whenever the
system cmake doesn't meet its own minimum (confirmed via the real build log -- vcpkg was about to
fetch this exact file for its own internal port-build use when the earlier diagnostic was
cancelled). `PATH`-prepended ahead of apt's `/usr/bin/cmake`; the apt package is left installed,
untouched, since nothing else depends on which one wins once this is in effect. Applied
mechanically via a second one-off script, anchored on the `RUN git clone ... vcpkg.git` line
already confirmed present verbatim in all 28 files; all 28 re-validated with
`docker buildx build --check` after this change too -- clean.

**Real measured result, both runs on a quiet host (load ~1-2, confirmed via `uptime` beforehand):**

| Build | What it measures | Real wall-clock time | Result |
|---|---|---|---|
| `docker compose build udsf` | Cold: cache starts populated from earlier (killed) attempts, but this is the first build to reach a *successful* configure+link under the CMake fix | **31m12s** | `docker-udsf:latest` built clean, 161/161 ninja targets, real `udsf` binary linked |
| `docker compose build nrf` | Warm: run immediately after, same host, same warm `vcpkg-bincache`/`ccache` mounts | **1m59s** | `docker-nrf:latest` rebuilt (new image hash), real `nrf` binary linked |

**~15.7x faster** for the second NF. The nrf build's own log confirms why: zero `vcpkg/buildtrees`
compile activity for any package (no onnxruntime/boost/protobuf/... rebuilding), and every
`[n/153] Building CXX object ...` line in the log is this project's *own* source
(`libs/sbi-generated`'s generated DTOs) completing in under 2 seconds each, consistent with a
`ccache` hit layered on top of an already-fully-restored vcpkg install -- both new caching layers
measurably doing their job, not just syntactically present.

This closes ADR-0452 as fully verified -- the original BuildKit-cache-mount idea, the real
CMake-version bug it incidentally surfaced and fixed, and the real before/after numbers proving the
fix delivers what it was meant to.

