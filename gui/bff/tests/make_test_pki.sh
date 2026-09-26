#!/usr/bin/env bash
# Throwaway PKI for the oam-gui-bff unit tests, generated at test time into <dir> so the tests
# never depend on (or write into) the repo's certs/ directory. Two independent roots, mirroring
# ADR-0422's trust split:
#   lab/      the service CA (NFs + the BFF's own service-facing identity)
#   operator/ the operator CA (who may open the GUI)
# Leaves: upstream + idp (lab, server+client), bff (lab, server+client), two shop terminals
# (operator CA, clientAuth), rogue-nf (lab CA, clientAuth -- an NF identity that must NOT open the
# GUI), and idp-sign: the fake IdP's ES256 ID-token signing key (+ its public point as DER).
set -euo pipefail
dir="$1"
mkdir -p "$dir"
cd "$dir"
[ -f done-v2 ] && exit 0

mkca() {
    openssl ecparam -name prime256v1 -genkey -noout -out "$1.key"
    openssl req -x509 -new -key "$1.key" -days 2 -sha256 -subj "/O=oam-gui-bff test/CN=$1 CA" \
        -addext "basicConstraints=critical,CA:true" -addext "keyUsage=critical,keyCertSign" \
        -out "$1.crt" 2>/dev/null
}
leaf() { # name ca eku
    mkdir -p "$1"
    openssl ecparam -name prime256v1 -genkey -noout -out "$1/key.pem"
    openssl req -new -key "$1/key.pem" -subj "/O=oam-gui-bff test/CN=$1" -out "$1/csr.pem"
    openssl x509 -req -in "$1/csr.pem" -CA "$2.crt" -CAkey "$2.key" -CAcreateserial -days 2 \
        -sha256 -out "$1/cert.pem" -extfile <(printf '%s\n' \
        "basicConstraints=CA:false" "keyUsage=critical,digitalSignature" \
        "extendedKeyUsage=$3" "subjectAltName=DNS:localhost,IP:127.0.0.1") 2>/dev/null
    rm -f "$1/csr.pem"
}
mkca lab
mkca operator
leaf upstream lab serverAuth,clientAuth
leaf bff lab serverAuth,clientAuth
leaf idp lab serverAuth,clientAuth
leaf terminal-shop-a operator clientAuth
leaf terminal-shop-b operator clientAuth
leaf rogue-nf lab clientAuth
mkdir -p idp-sign
openssl ecparam -name prime256v1 -genkey -noout -out idp-sign/key.pem
openssl ec -in idp-sign/key.pem -pubout -out idp-sign/pub.pem 2>/dev/null
openssl ec -in idp-sign/key.pem -pubout -outform DER -out idp-sign/pub.der 2>/dev/null
touch done-v2
