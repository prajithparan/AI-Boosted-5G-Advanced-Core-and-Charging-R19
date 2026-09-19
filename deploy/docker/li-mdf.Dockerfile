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

RUN cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -D5GC_BUILD_TESTS=OFF \
    && cmake --build build --target li-mdf

FROM ubuntu:24.04 AS runtime

RUN apt-get update && apt-get install -y --no-install-recommends \
    openssl ca-certificates libxml2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY --from=builder /build/build/nfs/li-mdf/li-mdf /build/li-mdf
COPY --from=builder /build/build/libs/li-core/libli_core.so /build/build/libs/li-core/
COPY --from=builder /build/specs/etsi /build/specs/etsi
COPY --from=builder /build/config /build/config

EXPOSE 7805/tcp 7806/tcp 9491/tcp

ENTRYPOINT ["/build/li-mdf"]
