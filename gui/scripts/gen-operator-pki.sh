#!/usr/bin/env bash
# Lab PKI for the operator GUI (ADR-0422). NOT production PKI -- same caveats as
# scripts/gen-lab-pki.sh. Creates, under certs/ (gitignored):
#   certs/oam-gui-bff/{cert,key}.pem       the BFF's identity, signed by the EXISTING lab CA
#                                           (serverAuth for browsers, clientAuth towards the NFs)
#   certs/oam-operator-ca/ca.{crt,key}     a SEPARATE operator CA: which terminals may open the GUI
#   certs/oam-operator-ca/<terminal>.p12   one client certificate per shop terminal, for the browser
#   certs/oam-gui-bff/oidc-client-secret   placeholder for the IdP client secret (replace it)
# Usage: gui/scripts/gen-operator-pki.sh [terminal-name ...]   (default: terminal-lab-1)
set -euo pipefail
REPO="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")/../.." && pwd)"
C="$REPO/certs"
[ -f "$C/ca/ca.key" ] || { echo "run scripts/gen-lab-pki.sh first (lab CA missing)"; exit 1; }
"$REPO/scripts/gen-lab-pki.sh" oam-gui-bff >/dev/null
mkdir -p "$C/oam-operator-ca"
cd "$C/oam-operator-ca"
if [ ! -f ca.key ]; then
    openssl ecparam -name prime256v1 -genkey -noout -out ca.key
    openssl req -x509 -new -key ca.key -days 365 -sha256 -subj "/O=5gc-r19 Lab/CN=5gc-r19 Operator CA" \
        -addext "basicConstraints=critical,CA:true" -addext "keyUsage=critical,keyCertSign,cRLSign" -out ca.crt
fi
for t in "${@:-terminal-lab-1}"; do
    [ -f "$t.p12" ] && continue
    openssl ecparam -name prime256v1 -genkey -noout -out "$t.key"
    openssl req -new -key "$t.key" -subj "/O=5gc-r19 Lab/CN=$t" -out "$t.csr"
    openssl x509 -req -in "$t.csr" -CA ca.crt -CAkey ca.key -CAcreateserial -days 365 -sha256 \
        -extfile <(printf 'basicConstraints=CA:false\nkeyUsage=critical,digitalSignature\nextendedKeyUsage=clientAuth\n') \
        -out "$t.crt"
    # Import into the browser; the export password is asked interactively, never stored here.
    openssl pkcs12 -export -inkey "$t.key" -in "$t.crt" -name "$t" -out "$t.p12"
    rm -f "$t.csr"
    echo "terminal certificate: $C/oam-operator-ca/$t.p12"
done
[ -f "$C/oam-gui-bff/oidc-client-secret" ] || { umask 077; echo "replace-with-the-idp-client-secret" > "$C/oam-gui-bff/oidc-client-secret"; }
echo "browser trust anchor for the BFF's server certificate: $C/ca/ca.crt (lab only)"
