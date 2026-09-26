# Operator GUI (Phase 7)

React + JSON Forms web console (`web/`) served by a C++ backend-for-frontend (`bff/`,
`oam-gui-bff`). Design: `docs/DECISIONS.md` ADR-0420 (stack), 0421 (derived schemas), 0422
(transport), 0423 (operator IAM + audit), 0424 (OIDC login), 0425 (NF configuration).

## Layout

| Path | What |
|---|---|
| `schema-gen/` | schema derivation + drift checks (TMF620 from `libs/bss-sid`, provisioning cross-check, one schema per `config/*.json`) |
| `web/src/schemas/` | the derived schemas (generated, committed, drift-checked by ctest) |
| `web/` | the React app; `npm run build` -> `web/dist/` |
| `bff/` | `oam-gui-bff`: operator mTLS + OIDC sessions, per-call authorization + audit, allow-listed forwarding |
| `../deploy/db/operator_iam/` | the `operator_iam` domain DB (roles, scopes, maker-checker, sessions, audit) |

## Build and test

```sh
cd gui/web && npm ci && npm run build            # web app (TypeScript check + Vite)
python3 gui/web/scripts/check_licenses.py gui/web/package-lock.json   # OSI-only gate
python3 gui/web/scripts/render_smoke.py gui/web/dist   # headless render under the real CSP
cmake --build build --target oam-gui-bff oam_gui_bff_unit_tests oam_gui_bff_security_tests
ctest --test-dir build -R 'gui_|Tmf620|Redact|Util|NfConfig'           # no database needed
flock /tmp/5gc-r19-itest.lock ctest --test-dir build -L needs-postgres  # BffSecurity.* (private DB)
```

The security suite creates and drops a private database on the PostgreSQL named by
`TEST_POSTGRES_URL` (default `postgresql://postgres@127.0.0.1:5434/postgres`) and binds loopback
ports 28741-28744 only.

## Run locally (lab)

1. `scripts/gen-lab-pki.sh` (if not done), then `gui/scripts/gen-operator-pki.sh terminal-lab-1`;
   import `certs/oam-operator-ca/terminal-lab-1.p12` into the browser and trust `certs/ca/ca.crt`.
2. Create the `operator_iam` DB on postgres-chf if the volume predates it:
   `docker cp deploy/db <pg-chf>:/domain-ddl && docker exec <pg-chf> bash /domain-ddl/init-domain-dbs.sh`
   (idempotent per database), then insert your org units, users (IdP issuer + subject) and role
   assignments -- see the test seed in `bff/tests/test_bff_security.cpp` for the shape.
3. An OIDC IdP (Keycloak recommended) with a confidential client `oam-gui`, redirect URI
   `https://127.0.0.1:8710/auth/callback`, MFA required; put its endpoints in
   `config/oam-gui-bff.json` and its client secret in `certs/oam-gui-bff/oidc-client-secret`.
   **Not provided yet** (ADR-0424 deferral): without an IdP the BFF starts but nobody can sign in.
4. `gui/web && npm run build`, then run `build/gui/bff/oam-gui-bff` and open
   `https://127.0.0.1:8710/`. `npm run watch` rebuilds `dist/`; restart the BFF to reload it.
