# syntax=docker/dockerfile:1
# Multi-stage build for nfs/upf. Derived from deploy/docker/smf.Dockerfile, which mirrors deploy/docker/amf.Dockerfile -- see that file's header
# comment for why this image does NOT generate its own lab PKI at start (must share the same root
# CA as nrf/amf; see deploy/docker/docker-compose.yml's pki-init service).

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

# ADR-0452: Ubuntu 24.04's apt-packaged cmake (3.28.3) has different $<LINK_LIBRARY:...>
# generator-expression validation than the 4.4.0 this project's host dev environment and
# .github/workflows/ci.yml already standardize on -- confirmed, by elimination, as the real
# cause of a real "Impossible to link target 'chf'... ONNX::onnx... WHOLE_ARCHIVE" CMake
# configure error this version mismatch produced (nfs/chf/CMakeLists.txt and nfs/nwdaf/
# CMakeLists.txt both use that feature for ONNX::onnx, for a real, documented reason -- a
# dropped-operator-schema bug, see their own comments -- not something to work around by
# changing the project's own CMakeLists.txt). Same official Kitware release tarball vcpkg
# itself already downloads internally when the system cmake doesn't meet ITS OWN minimum
# (confirmed via a real build log), just installed where the top-level configure's own cmake
# invocation picks it up too -- PATH-prepended so it takes precedence over apt's /usr/bin/
# cmake for the rest of this build, without uninstalling the apt package (nothing else here
# depends on which cmake wins once this is in effect).
RUN curl -fsSL https://github.com/Kitware/CMake/releases/download/v4.4.0/cmake-4.4.0-linux-x86_64.tar.gz \
    | tar -xz -C /opt
ENV PATH="/opt/cmake-4.4.0-linux-x86_64/bin:${PATH}"

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
    && cmake --build build --target upf

FROM ubuntu:24.04 AS runtime

# libbpf1/libelf1/libcap2: the three shared libraries `ldd nfs/upf/upf` shows beyond libc (eBPF/XDP
# datapath, capability handling). iproute2: the datapath shells out to `ip link/addr/netns`
# (nfs/upf/src/datapath.cpp run_ip_command) -- without it the datapath fails at start and the UPF
# falls back to "PFCP control-plane signalling still works, but no uplink packet will actually be
# decapsulated/forwarded". Running the datapath also needs capabilities this image cannot grant
# itself: see the `upf` service in docker-compose.yml (cap_add) and deploy/helm/upf.
RUN apt-get update && apt-get install -y --no-install-recommends \
    openssl ca-certificates libbpf1 libelf1 libcap2 iproute2 \
    && rm -rf /var/lib/apt/lists/*

WORKDIR /build
COPY --from=builder /build/build/nfs/upf/upf /build/upf
# GTPU_BPF_OBJ_PATH is an absolute compile-time path (nfs/upf/CMakeLists.txt: the object's location
# in the BUILD tree), so the object must sit at the same absolute path in this image.
COPY --from=builder /build/build/nfs/upf/gtpu_decap.bpf.o /build/build/nfs/upf/gtpu_decap.bpf.o

# 7796 Nupf SBI (TLS 1.3 + mTLS), 8805/udp PFCP (N4), 9471 Prometheus /metrics. config/upf.json's
# li_poi X1 listener (7810) is off by default and is not exposed here.
EXPOSE 7796/tcp 8805/udp 9471/tcp

ENTRYPOINT ["/build/upf"]
