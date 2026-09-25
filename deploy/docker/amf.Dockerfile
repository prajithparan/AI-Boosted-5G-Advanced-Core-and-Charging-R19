# Multi-stage build for nfs/amf. Mirrors deploy/docker/nrf.Dockerfile -- see that file's header
# comment for why /build is used as the source root (nfs/amf/CMakeLists.txt bakes CERTS_DIR in as
# a compile-time absolute path the same way nfs/nrf/CMakeLists.txt does).
#
# UNLIKE nrf.Dockerfile's standalone entrypoint: this image does NOT generate its own lab PKI at
# start. AMF's cert must chain to the SAME root CA nrf's cert does (and vice versa, for mTLS to
# work both directions) -- two containers each independently running gen-lab-pki.sh would mint two
# unrelated CAs, and the mTLS handshake between them would fail. See
# deploy/docker/docker-compose.yml's pki-init service, which provisions certs/ once into a shared
# volume both nrf and amf mount. This entrypoint assumes /build/certs is already populated when the
# container starts.

FROM ubuntu:24.04 AS builder

# bison/flex: vcpkg builds libpq from source (libpqxx, ADR-0054 -- bss/product-catalog's real
# PostgreSQL persistence) -- and since vcpkg.json is one shared manifest, `vcpkg install` pulls in
# every dependency for ANY target's configure step, not just the one being built here. Real
# regression this project hit and fixed: every Dockerfile broke on this the moment libpqxx was
# added, not just product-catalog's own.
RUN apt-get update && apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build git curl zip unzip tar pkg-config \
    python3 python3-pip python3-venv ca-certificates bison flex patch \
    # libsctp-dev/libbpf-dev/libcap-dev/clang-18: libs/ngap-core (SCTP) and nfs/upf
    # (eBPF/XDP datapath) require these at CMake configure time -- same real,
    # pre-existing gap as the asn1c one above, matching the fix
    # .github/workflows/ci.yml already needed for the identical reason.
    libsctp-dev libbpf-dev libcap-dev clang-18 \
    && rm -rf /var/lib/apt/lists/*

RUN python3 -m venv /opt/codegen-venv && /opt/codegen-venv/bin/pip install jinja2 pyyaml
ENV PATH="/opt/codegen-venv/bin:${PATH}"

RUN git clone https://github.com/microsoft/vcpkg.git /opt/vcpkg \
    && git -C /opt/vcpkg checkout f1d4bbc72f183441403ba5107cb19d75a5abc2a2 \
    && /opt/vcpkg/bootstrap-vcpkg.sh -disableMetrics

WORKDIR /build
COPY . .

# asn1c: libs/ngap-generated needs this real toolchain at configure time (ADR-0030/
# ADR-0031) -- pre-existing gap, independent of the libpqxx/bison fix above: this repo's
# Dockerfiles never ran this step, so a from-scratch image build was already broken
# before product-catalog/libpqxx existed. Found by actually running a real docker build,
# not assumed.
RUN ./scripts/setup-asn1c.sh

RUN cmake -S . -B build -G Ninja \
    -DCMAKE_TOOLCHAIN_FILE=/opt/vcpkg/scripts/buildsystems/vcpkg.cmake \
    -DCMAKE_BUILD_TYPE=Release -D5GC_BUILD_TESTS=OFF \
    && cmake --build build --target amf

FROM ubuntu:24.04 AS runtime

# libxml2: the AMF now hosts an LI IRI-POI (src/li_poi.cpp, ADR-0377) whose LI_X1 server links
# li_core, which validates X1 documents with libxml2 at runtime.
RUN apt-get update && apt-get install -y --no-install-recommends \
    openssl ca-certificates libxml2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY --from=builder /build/build/nfs/amf/amf /build/amf
# The AMF's IRI-POI links li_core (ADR-0377): its build-tree RPATH looks for libli_core.so under
# /build/build/libs/li-core, and li_core carries LI_ETSI_SCHEMA_DIR=/build/specs/etsi as a
# compile-time absolute path, so the X1 schema set must be present there or X1 validation fails
# closed. Same two additions li-mdf.Dockerfile documents.
COPY --from=builder /build/build/libs/li-core/libli_core.so /build/build/libs/li-core/
COPY --from=builder /build/specs/etsi /build/specs/etsi
# ADR-0440: x1-validation.xsd imports the 3GPP X1 extension schema (TS 33.128 attachment,
# namespace r19:v4) from ../../3gpp/33128-attachments -- without it the schema set does not load
# and every X1 request fails closed.
COPY --from=builder /build/specs/3gpp/33128-attachments/urn_3GPP_ns_li_3GPPX1Extensions.xsd \
     /build/specs/3gpp/33128-attachments/
# CONFIG_DIR is baked in at compile time as /build/config (docs/DECISIONS.md ADR-0077) --
# config/amf.json is checked-in, non-secret lab default config, so it's copied into the runtime
# image directly rather than volume-mounted like certs_data (which must come from pki-init).
COPY config/amf.json /build/config/amf.json

# 7778 Namf SBI, 9465 Prometheus, 7807 LI_X1 provisioning (the IRI-POI; only bound when
# li_poi.enabled -- config/amf.json defaults it off).
EXPOSE 7778/tcp 9465/tcp 7807/tcp

ENTRYPOINT ["/build/amf"]
