# syntax=docker/dockerfile:1
# Multi-stage build for gui/bff (oam-gui-bff), the Phase 7 operator GUI's backend-for-frontend
# (ADR-0420..0425, ADR-0441). Closes the gap ADR-0441 explicitly disclosed and deferred: this NF
# had no Dockerfile, compose entry, or Helm chart at all -- Phase 7 built and tested it as a CMake
# binary only.
#
# THREE stages, not the usual two, because this is the one component in this repo whose runtime
# artifact is not solely a C++ binary: gui/web (React + JSON Forms, this project's one deliberate
# exception to "C/C++/Python only for new code") must be built into static assets
# (index.html/assets/*) BEFORE the C++ stage even runs, because `oam-gui-bff` REFUSES TO START
# without them -- gui/bff/src/bff.cpp's `load_static_files()` throws if `<static_dir>/index.html`
# is missing, by design ("build the web app first"). CI has never run `npm ci`/`npm run build`
# (only schema/license Python checks against the checked-in source) -- this Dockerfile is the
# first place that build actually runs unattended; verified here with Node 22 (`npm ci && npm run
# build` completed cleanly, `tsc --noEmit` included -- Vite 8 needs Node >=20.19, well inside
# node:22's range).
#
# The C++ stage mirrors every other Dockerfile in this directory (nrf.Dockerfile's header comment
# has the full rationale) -- including the FULL builder toolchain (libsctp-dev/libbpf-dev/
# libcap-dev/clang-18/bison+flex/asn1c), which this NF's own CMakeLists.txt does NOT use (no SCTP,
# no eBPF, no ASN.1). That was checked, not assumed, before writing this file: the single top-level
# CMakeLists.txt unconditionally add_subdirectory()s every NF, including nfs/upf
# (`pkg_check_modules(libbpf REQUIRED ...)`, `find_program(CLANG_EXECUTABLE ... REQUIRED)`) and
# libs/ngap-generated (`find_program(ASN1C_EXECUTABLE ... REQUIRED)`, both evaluated at CONFIGURE
# time, not just when their own target is built. Configuring for `--target oam-gui-bff` still walks
# every one of those CMakeLists.txt files, so the reduced toolchain the task brief floated as
# plausible does not actually work here -- confirmed by reading those two CMakeLists.txt files, not
# guessed either way.
#
# Like udr.Dockerfile/udsf.Dockerfile: does NOT generate its own lab PKI at start. `oam-gui-bff`'s
# leaf certificate must chain to the SAME shared lab CA every other NF's does (see
# docker-compose.yml's `pki-init` service, whose NF list now includes `oam-gui-bff`) -- assumes
# /build/certs is already populated by that service when the container starts.

FROM node:22-alpine AS web-builder

WORKDIR /web
# package.json/package-lock.json copied first so `npm ci` layer-caches across source-only edits.
COPY gui/web/package.json gui/web/package-lock.json ./
RUN npm ci
COPY gui/web/ .
RUN npm run build

FROM ubuntu:24.04 AS builder

# bison/flex: vcpkg builds libpq from source (libpqxx, ADR-0054) -- and since vcpkg.json is one
# shared manifest, `vcpkg install` pulls in every dependency for ANY target's configure step, not
# just the one being built here (nrf.Dockerfile's header comment has the regression history).
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config \
    python3 python3-pip python3-venv ca-certificates bison flex patch \
    # libsctp-dev/libbpf-dev/libcap-dev/clang-18: libs/ngap-core (SCTP) and nfs/upf (eBPF/XDP
    # datapath) require these at CMake CONFIGURE time, unconditionally, for every target -- see
    # this file's header comment for how that was confirmed rather than assumed for oam-gui-bff.
    libsctp-dev libbpf-dev libcap-dev clang-18 \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/codegen-venv && /opt/codegen-venv/bin/pip install jinja2 pyyaml
ENV PATH="/opt/codegen-venv/bin:${PATH}"

# Full (non-shallow) clone + explicit checkout of the exact commit vcpkg.json's builtin-baseline
# pins: a shallow clone only fetches the default branch's current tip, which usually does NOT
# contain that specific historical commit object, and vcpkg's version resolution needs it present.
RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout f1d4bbc72f183441403ba5107cb19d75a5abc2a2 \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /build
COPY . .

# asn1c: libs/ngap-generated needs this real toolchain at configure time (ADR-0030/ADR-0031),
# unconditionally, for the same reason the apt packages above are unconditional.
RUN ./scripts/setup-asn1c.sh

# ADR-0452: ccache, installed as its own step rather than folded into the apt-get block
# above (which has scattered inline comments per-Dockerfile) -- a separate RUN here is the
# same one line in every one of the 28 Dockerfiles in this directory, safe to apply
# mechanically without parsing each file's own package list.
RUN apt-get update && apt-get install -y --no-install-recommends ccache && rm -rf /var/lib/apt/lists/*

# ADR-0452: BuildKit cache mounts, shared across EVERY NF's Dockerfile via the same id=
# (not scoped per-image) -- real, confirmed problem: vcpkg.json is one shared manifest (see
# this project's own Dockerfiles' bison/flex comments), so every NF's image build cold-
# compiled the ENTIRE dependency set including onnxruntime from scratch, with the manifest's
# shared majority (boost, openssl, curl, protobuf, ...) never reused across the 28 separate
# Dockerfiles here. vcpkg-bincache holds vcpkg's own binary cache (skips recompiling a
# package whose exact ABI hash was already built by ANY other NF's image build on this
# host, same VCPKG_BINARY_SOURCES mechanism .github/workflows/ci.yml already uses for its
# own native build); ccache holds this project's OWN compiled object files, keyed by content
# hash, so an unchanged .cpp recompiling for a different NF target still hits cache.
# sharing=locked: vcpkg's binary-cache directory is documented not safe for concurrent
# writers, and `docker compose up --build` can build several NF images in parallel.
RUN --mount=type=cache,id=vcpkg-bincache,target=/root/.cache/vcpkg-bincache,sharing=locked \
    --mount=type=cache,id=ccache,target=/root/.cache/ccache,sharing=locked \
    VCPKG_BINARY_SOURCES="clear;files,/root/.cache/vcpkg-bincache,readwrite" \
    CCACHE_DIR=/root/.cache/ccache \
    cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -D5GC_BUILD_TESTS=OFF \
    -DCMAKE_C_COMPILER_LAUNCHER=ccache -DCMAKE_CXX_COMPILER_LAUNCHER=ccache \
    && cmake --build build --target oam-gui-bff

FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    openssl ca-certificates \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY --from=builder /build/build/gui/bff/oam-gui-bff /build/oam-gui-bff

# CONFIG_DIR-equivalent (this NF resolves relative config paths against REPO_ROOT, baked in at
# compile time as /build/config -- see gui/bff/CMakeLists.txt's REPO_ROOT compile definition and
# gui/bff/src/main.cpp's resolve()). The whole config/ tree, not just oam-gui-bff.json: the NF
# config viewer (ADR-0425) reads every other component's config/<nf>.json to display it in the
# GUI, so a partial copy would make every other tile in that screen 404. All checked-in, non-secret
# lab defaults -- same class of file amf.Dockerfile already bakes in for its own config/amf.json.
COPY config /build/config
# ADR-0425's derived per-component JSON Schemas, read at request time by NfConfigManager
# (gui/bff/src/config_mgmt.cpp) to validate/mask an operator's edit -- without these, every
# component's config screen in the GUI fails closed with "no schema".
COPY gui/web/src/schemas/nf-config /build/gui/web/src/schemas/nf-config
# ADR-0442's compose-only config overlay (deliberately NOT under config/ -- see that file's own
# header comment). Baked in at the SAME path docker-compose.yml's OAM-GUI-BFF_CONFIG_FILE env var
# names, so it is present whether or not that env var ends up set for a given run.
COPY deploy/docker/oam-gui-bff.compose.json /build/deploy/docker/oam-gui-bff.compose.json
# The built web app (see the web-builder stage above) -- load_static_files() requires
# <static_dir>/index.html to exist or this binary refuses to start.
COPY --from=web-builder /web/dist /build/gui/web/dist

# 8710: the operator-facing HTTPS listener (TLS 1.3 + operator-CA mTLS + OIDC session cookie).
# 9510: Prometheus /metrics.
EXPOSE 8710/tcp 9510/tcp

ENTRYPOINT ["/build/oam-gui-bff"]
