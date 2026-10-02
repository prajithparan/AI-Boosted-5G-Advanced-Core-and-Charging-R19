# Operator GUI (Phase 7)

React + JSON Forms web console (`web/`) served by a C++ backend-for-frontend (`bff/`,
`oam-gui-bff`). Design: `docs/DECISIONS.md` ADR-0420 (stack), 0421 (derived schemas), 0422
(transport), 0423 (operator IAM + audit), 0424 (OIDC login), 0425 (NF configuration), 0441 (real
Keycloak realm, closing 0424's IdP deferral), 0442 (`oam-gui-bff` containerized, closing 0441's
Compose-side deferral).

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
2. Create the `operator_iam` DB on postgres-chf if the volume predates it. Do NOT re-run
   `init-domain-dbs.sh` on an existing volume (it re-applies the charging DDL, which assumes a fresh
   database, and stops). Apply only this domain, in order:
   ```sh
   docker exec docker-postgres-chf-1 psql -U postgres -c 'CREATE DATABASE operator_iam'
   for f in deploy/db/operator_iam/*.sql; do
     docker exec -i docker-postgres-chf-1 psql -U postgres -v ON_ERROR_STOP=1 -d operator_iam < "$f"
   done
   ```
   then insert your org units, users (IdP issuer + subject) and role assignments -- the test seed
   in `bff/tests/test_bff_security.cpp` shows the shape.
3. An OIDC IdP: **provided since ADR-0441/ADR-0442** (corrected 2026-10-02, docs-audit -- this step
   previously said "not provided yet," which is stale). A real, self-hosted Keycloak realm now runs
   as the `keycloak` Compose service (`quay.io/keycloak/keycloak:26.0`, its own `keycloak` database
   on `postgres-chf`). Before `docker compose up keycloak`, generate its realm import:
   `deploy/keycloak/render-realm.sh` writes into `deploy/keycloak/import/` (gitignored, generates
   its own secrets, same pattern as `certs/`); Keycloak reads it once via `--import-realm` on first
   boot against an empty `keycloak` database. `oam-gui-bff` also needs its own leaf certificate and
   trust in Keycloak's TLS server certificate (`gui/scripts/gen-operator-pki.sh terminal-lab-1`,
   same CA as step 1). The confidential-client/redirect-URI/MFA shape this step originally
   described is what `render-realm.sh` provisions -- not independently re-verified field-by-field
   against this paragraph's exact wording in this pass.
4. `gui/web && npm run build`, then run `build/gui/bff/oam-gui-bff` and open
   `https://127.0.0.1:8710/`. `npm run watch` rebuilds `dist/`; restart the BFF to reload it.
   Another config file: `env 'OAM-GUI-BFF_CONFIG_FILE=/abs/path.json' build/gui/bff/oam-gui-bff`
   (the variable name keeps the hyphens -- `nf_config::load` upper-cases the service name only).

Note: in the lab an approved NF configuration change rewrites the git-tracked `config/<nf>.json`
(ADR-0425); the NF picks it up on its next restart.
