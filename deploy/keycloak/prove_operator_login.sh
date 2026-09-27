#!/usr/bin/env bash
# Drives a real login round-trip through a CONTAINERIZED oam-gui-bff + containerized Keycloak
# (ADR-0442) -- the same proof shape ADR-0441 ran by hand with curl, scripted here so it is
# reproducible rather than left to a scratchpad. Presents the real operator-CA terminal client
# certificate (mTLS), submits the real seeded lab password, computes and submits the real TOTP
# code (deploy/keycloak/totp.py), and confirms a real session cookie is issued.
#
# Prerequisites (see ADR-0442 for the exact commands used the first time):
#   - scripts/gen-lab-pki.sh (with oam-gui-bff, keycloak, and every other NF name) run once,
#     either on the host or via the already-built image's own openssl (pki-init's own apt-get
#     install can be skipped this way if network to apt is unavailable).
#   - gui/scripts/gen-operator-pki.sh terminal-lab-1 (host) -- the operator-CA client cert/key
#     this script presents.
#   - deploy/keycloak/render-realm.sh (host) -- the real client secret + seeded lab users' real
#     password/TOTP secret this script reads.
#   - deploy/db/operator_iam/*.sql and deploy/keycloak/seed-lab-operators.sql applied to the
#     operator_iam database the running oam-gui-bff container points at.
#   - `docker compose -f deploy/docker/docker-compose.yml up -d --no-deps keycloak oam-gui-bff`
#     (or an equivalent already-running pair) reachable at 127.0.0.1:8443/127.0.0.1:8710.
#
# Usage: deploy/keycloak/prove_operator_login.sh <repo_root> <ca_bundle_path>
#   repo_root:      the checkout root (for certs/oam-operator-ca/terminal-lab-1.{crt,key} and
#                    certs/keycloak-lab-users/shop.agent.lab.{password,otp-secret}).
#   ca_bundle_path:  the CA that signed BOTH the containerized oam-gui-bff's and Keycloak's TLS
#                    server certificates -- the certs_data VOLUME's CA, not necessarily
#                    <repo_root>/certs/ca/ca.crt (a HOST run of scripts/gen-lab-pki.sh produces a
#                    SEPARATE CA from a container-run pki-init -- see ADR-0442's own disclosure).
#                    Extract it from a running container/volume, e.g.:
#                      docker run --rm -v docker_certs_data:/build/certs --entrypoint cat \
#                        <any-image-with-that-volume-mounted> /build/certs/ca/ca.crt > ca.crt
set -euo pipefail
REPO="$1"
CA="$2"
JAR="$(mktemp)"
CERT="$REPO/certs/oam-operator-ca/terminal-lab-1.crt"
KEY="$REPO/certs/oam-operator-ca/terminal-lab-1.key"
CURL=(curl -s -c "$JAR" -b "$JAR" --cacert "$CA" --cert "$CERT" --key "$KEY")

echo "1. GET /auth/login"
HDRS="$(mktemp)"
"${CURL[@]}" -D "$HDRS" -o /dev/null https://127.0.0.1:8710/auth/login
STATUS=$(head -1 "$HDRS")
LOC=$(grep -i '^location:' "$HDRS" | sed 's/^[Ll]ocation: //;s/\r//')
echo "   $STATUS -> $LOC"
[[ "$LOC" == https://127.0.0.1:8443/realms/5gc-r19-operators/* ]] || { echo "FAIL: unexpected auth url"; exit 1; }
[[ "$LOC" == *acr_values=mfa* ]] || { echo "FAIL: no acr_values=mfa"; exit 1; }

echo "2. GET authorization_endpoint"
PAGE1="$(mktemp)"
"${CURL[@]}" -o "$PAGE1" "$LOC"
LOGIN_ACTION=$(grep -o '<form[^>]*action="[^"]*"' "$PAGE1" | head -1 | sed 's/.*action="//;s/"$//' | sed 's/&amp;/\&/g')
[ -n "$LOGIN_ACTION" ] || { echo "FAIL: no login form action found"; cat "$PAGE1"; exit 1; }
echo "   login form action: ${LOGIN_ACTION:0:80}..."

echo "3. POST username/password"
PASSWORD="$(cat "$REPO/certs/keycloak-lab-users/shop.agent.lab.password")"
PAGE2="$(mktemp)"
"${CURL[@]}" -o "$PAGE2" --data-urlencode "username=shop.agent.lab" --data-urlencode "password=$PASSWORD" "$LOGIN_ACTION"
OTP_ACTION=$(grep -o '<form[^>]*action="[^"]*"' "$PAGE2" | head -1 | sed 's/.*action="//;s/"$//' | sed 's/&amp;/\&/g')
[ -n "$OTP_ACTION" ] || { echo "FAIL: no OTP form action found (bad password?)"; cat "$PAGE2"; exit 1; }
echo "   otp form action: ${OTP_ACTION:0:80}..."

echo "4. POST otp"
CODE="$(python3 "$REPO/deploy/keycloak/totp.py" shop.agent.lab)"
HDRS2="$(mktemp)"
"${CURL[@]}" -D "$HDRS2" -o /dev/null --data-urlencode "otp=$CODE" "$OTP_ACTION"
STATUS2="$(head -1 "$HDRS2")"
CALLBACK="$(grep -i '^location:' "$HDRS2" | sed 's/^[Ll]ocation: //;s/\r//')"
echo "   $STATUS2 -> ${CALLBACK:0:100}..."
[[ "$CALLBACK" == https://127.0.0.1:8710/auth/callback* ]] || { echo "FAIL: unexpected callback url: $CALLBACK"; exit 1; }

echo "5. GET /auth/callback"
HDRS3="$(mktemp)"
"${CURL[@]}" -D "$HDRS3" -o /dev/null "$CALLBACK"
STATUS3="$(head -1 "$HDRS3")"
SESSION_COOKIE="$(grep -i '^set-cookie:.*__Host-oam_session' "$HDRS3" || true)"
echo "   $STATUS3"
echo "   $SESSION_COOKIE"
[[ "$STATUS3" == *200* ]] || { echo "FAIL: callback status not 200"; exit 1; }
[ -n "$SESSION_COOKIE" ] || { echo "FAIL: no session cookie issued"; exit 1; }

echo
echo "PROOF OK: containerized oam-gui-bff <-> containerized Keycloak, real MFA login, real session cookie issued."
