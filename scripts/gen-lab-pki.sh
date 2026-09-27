#!/usr/bin/env bash
# Generates a throwaway lab CA and per-NF leaf certificates for local TLS 1.3 + mTLS between NFs.
#
# NOT for production use as-is: this is a single self-signed root with no intermediate, no CRL/OCSP,
# no HSM-backed keys, and 1-year validity purely because "regenerate before it expires" is simpler
# than rotation machinery for a lab. Real deployment PKI (per TS 33.501) is out of scope here --
# this exists so libs/sbi-core's mTLS enforcement has real certs to test against. See
# docs/DECISIONS.md for the ADR this supports.
#
# Each NF gets ONE cert with BOTH serverAuth and clientAuth EKU, because every NF in this project
# is simultaneously an SBI server (exposing its own API) and an SBI client (calling other NFs) --
# see CLAUDE.md's non-negotiable rules. SAN covers 127.0.0.1/localhost since every NF in this lab
# runs on loopback; add real hostnames/IPs here if that ever changes.
#
# Usage: scripts/gen-lab-pki.sh [nf-name ...]
#   Defaults to: stub-nrf hello-nf
#   Output: certs/ca/ca.{key,crt}, certs/<nf-name>/{key,crt}.pem

set -euo pipefail

SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" &>/dev/null && pwd)"
REPO_ROOT="$(cd -- "${SCRIPT_DIR}/.." &>/dev/null && pwd)"
CERTS_DIR="${REPO_ROOT}/certs"

NF_NAMES=("$@")
if [ ${#NF_NAMES[@]} -eq 0 ]; then
    NF_NAMES=(nrf hello-nf)
fi

mkdir -p "${CERTS_DIR}/ca"

echo "== Lab root CA =="
if [ ! -f "${CERTS_DIR}/ca/ca.key" ]; then
    openssl ecparam -name prime256v1 -genkey -noout -out "${CERTS_DIR}/ca/ca.key"
    openssl req -x509 -new -key "${CERTS_DIR}/ca/ca.key" \
        -days 365 -sha256 \
        -subj "/O=5gc-r19 Lab/CN=5gc-r19 Lab Root CA" \
        -addext "basicConstraints=critical,CA:true" \
        -addext "keyUsage=critical,keyCertSign,cRLSign" \
        -out "${CERTS_DIR}/ca/ca.crt"
    echo "  generated ${CERTS_DIR}/ca/ca.{key,crt}"
else
    echo "  ${CERTS_DIR}/ca/ca.key already exists, reusing (delete certs/ to force regeneration)"
fi

for nf in "${NF_NAMES[@]}"; do
    nf_dir="${CERTS_DIR}/${nf}"
    mkdir -p "${nf_dir}"

    if [ -f "${nf_dir}/key.pem" ]; then
        echo "== ${nf}: cert already exists, skipping =="
        # Group-readable (openssl otherwise writes private keys 0600, owner-only, regardless of
        # whether this branch or the fresh-generation branch below runs): every NF's own runtime
        # container in this project runs as root, so 0600-owned-by-root was never a problem here --
        # until Keycloak (ADR-0441/ADR-0442), whose official image drops privileges to UID 1000
        # (still GID 0, confirmed: `docker run --entrypoint id quay.io/keycloak/keycloak:26.0` ->
        # `uid=1000(keycloak) gid=0(root)`) and fails closed at startup unable to read a 0600
        # root-owned key.pem ("Failed to start server in (production) mode:
        # /build/certs/keycloak/key.pem", no further detail logged). This branch's own chmod is not
        # optional: a `certs_data` volume provisioned by an EARLIER run of this script (i.e. every
        # volume that already exists on a machine that ran ADR-0441's own compose Keycloak service
        # before this fix) hits this "already exists" branch forever after, never the fresh one
        # below -- meaning ADR-0441's own compose Keycloak could not actually have started against
        # such a volume as committed; this is disclosed in ADR-0442, not assumed fixed by the
        # fresh-generation branch alone.
        chmod 640 "${nf_dir}/key.pem" 2>/dev/null || true
        continue
    fi

    echo "== ${nf} =="
    openssl ecparam -name prime256v1 -genkey -noout -out "${nf_dir}/key.pem"
    # 0640 (owner rw, GROUP r, other none), not the openssl default 0600: every leaf key's group
    # owner is `root` (gid 0) in every container that runs this script, which is the SAME gid
    # Keycloak's own non-root UID 1000 process runs under -- group-read is therefore sufficient,
    # world-readable is not needed on this otherwise-shared, multi-user host. Still a lab-only
    # CA/keypair (this file's own header comment) -- the CA key itself (`ca.key`, which signs, and
    # is never read by a served container) is deliberately left at its default 0600.
    chmod 640 "${nf_dir}/key.pem"
    openssl req -new -key "${nf_dir}/key.pem" \
        -subj "/O=5gc-r19 Lab/CN=${nf}" \
        -out "${nf_dir}/csr.pem"

    openssl x509 -req -in "${nf_dir}/csr.pem" \
        -CA "${CERTS_DIR}/ca/ca.crt" -CAkey "${CERTS_DIR}/ca/ca.key" -CAcreateserial \
        -days 365 -sha256 \
        -extfile <(cat <<EOF
basicConstraints=CA:false
keyUsage=critical,digitalSignature,keyEncipherment
extendedKeyUsage=serverAuth,clientAuth
subjectAltName=DNS:${nf},DNS:localhost,IP:127.0.0.1
EOF
) \
        -out "${nf_dir}/cert.pem"

    rm -f "${nf_dir}/csr.pem"
    echo "  generated ${nf_dir}/{key,cert}.pem (serverAuth+clientAuth, SAN=127.0.0.1/localhost/${nf})"
done

echo "== NRF OAuth2 JWT signing key (ES256) =="
# Deliberately a SEPARATE keypair from any NF's mTLS transport cert above -- reusing a transport
# key for JOSE signing mixes usages that real PKI practice (and TS 33.501) keeps apart. This is
# NRF's own signing key; every NF's copy of nrf-jwt/public.pem is what they use to verify tokens
# NRF issues (see sbi_core::jwt::Verifier). See docs/DECISIONS.md ADR-0012.
JWT_KEY_DIR="${CERTS_DIR}/nrf-jwt"
mkdir -p "${JWT_KEY_DIR}"
if [ ! -f "${JWT_KEY_DIR}/private.pem" ]; then
    openssl ecparam -name prime256v1 -genkey -noout -out "${JWT_KEY_DIR}/private.pem"
    openssl ec -in "${JWT_KEY_DIR}/private.pem" -pubout -out "${JWT_KEY_DIR}/public.pem" 2>/dev/null
    echo "  generated ${JWT_KEY_DIR}/{private,public}.pem"
else
    echo "  ${JWT_KEY_DIR}/private.pem already exists, reusing"
fi

echo "== Done. CA: ${CERTS_DIR}/ca/ca.crt =="
