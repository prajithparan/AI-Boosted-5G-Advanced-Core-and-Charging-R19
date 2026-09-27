#!/usr/bin/env python3
"""Prints the current 6-digit TOTP code for a lab user seeded by render-realm.sh.

Keycloak's OTPCredentialModel/HmacOTP does NOT base32-decode the stored `secretData.value` --
it uses the raw UTF-8 bytes of that string directly as the HMAC-SHA1 key (RFC 6238's own test
vectors do the same: the reference key "12345678901234567890" is used as raw ASCII bytes, never
base32-decoded). Verified empirically against a real Keycloak 26.0 container while building this
integration (ADR-0441): a base32-decoded key was rejected ("Invalid authenticator code"); the raw
bytes of the same secret string were accepted. Not documented by us elsewhere; disclosed here
because it is easy to get backwards if re-derived from a generic TOTP library's assumptions.

Usage: deploy/keycloak/totp.py <username>
  reads certs/keycloak-lab-users/<username>.otp-secret (written by render-realm.sh).
"""
import hashlib
import hmac
import pathlib
import struct
import sys
import time


def totp(secret: str, digits: int = 6, period: int = 30) -> str:
    key = secret.encode()
    counter = int(time.time() // period)
    msg = struct.pack(">Q", counter)
    h = hmac.new(key, msg, hashlib.sha1).digest()
    offset = h[-1] & 0x0F
    code = (struct.unpack(">I", h[offset:offset + 4])[0] & 0x7FFFFFFF) % (10 ** digits)
    return str(code).zfill(digits)


def main() -> None:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <username>", file=sys.stderr)
        raise SystemExit(2)
    username = sys.argv[1]
    repo_root = pathlib.Path(__file__).resolve().parents[2]
    secret_path = repo_root / "certs" / "keycloak-lab-users" / f"{username}.otp-secret"
    if not secret_path.exists():
        print(f"no such lab user secret: {secret_path} (run deploy/keycloak/render-realm.sh)",
              file=sys.stderr)
        raise SystemExit(1)
    secret = secret_path.read_text().strip()
    print(totp(secret))


if __name__ == "__main__":
    main()
