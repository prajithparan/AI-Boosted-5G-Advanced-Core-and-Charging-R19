# syntax=docker/dockerfile:1
# Multi-stage build for nfs/li-mdf, the MDF2 (ADR-0377). Mirrors deploy/docker/adrf.Dockerfile,
# with three additions no other NF image needs, because this is the first container to ship
# li_core (ADR-0372 disclosed exactly this requirement when the X1 schema path was introduced):
#
#   1. libli_core.so -- the repository's ONE shared library (ADR-0364 folds the asn1c codec into
#      it with hidden visibility to keep 47 NGAP-colliding type names out of an NF's symbol
#      space). The binary's build-tree RPATH looks for it under /build/build/libs/li-core, which
#      is where it is copied.
#   2. specs/etsi -- the ETSI X1 schema set. LI_ETSI_SCHEMA_DIR is a compile-time absolute path
#      (/build/specs/etsi); without the files there, schema validation fails closed and EVERY X1
#      request becomes a TopLevelError. WORKDIR is /build in both stages so the path resolves.
#   3. config/ -- CONFIG_DIR is likewise a build-tree absolute path.
#
# Certificates come from the shared volume pki-init populates, as for every other NF: this image
# does NOT generate its own, because every NF must share one root CA.

FROM ubuntu:24.04 AS builder

RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config \
    python3 python3-pip python3-venv ca-certificates bison flex patch \
    libsctp-dev libbpf-dev libcap-dev clang-18 \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/codegen-venv && /opt/codegen-venv/bin/pip install jinja2 pyyaml
ENV PATH="/opt/codegen-venv/bin:${PATH}"

RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout f1d4bbc72f183441403ba5107cb19d75a5abc2a2 \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /build
COPY . .

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
    && cmake --build build --target li-mdf

FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    openssl ca-certificates libxml2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY --from=builder /build/build/nfs/li-mdf/li-mdf /build/li-mdf
COPY --from=builder /build/build/libs/li-core/libli_core.so /build/build/libs/li-core/
COPY --from=builder /build/specs/etsi /build/specs/etsi
# ADR-0440: x1-validation.xsd imports the 3GPP X1 extension schema (TS 33.128 attachment,
# namespace r19:v4) from ../../3gpp/33128-attachments -- without it the schema set does not load
# and every X1 request fails closed.
COPY --from=builder /build/specs/3gpp/33128-attachments/urn_3GPP_ns_li_3GPPX1Extensions.xsd \
     /build/specs/3gpp/33128-attachments/
COPY --from=builder /build/config /build/config

EXPOSE 7805/tcp 7806/tcp 9491/tcp

ENTRYPOINT ["/build/li-mdf"]
