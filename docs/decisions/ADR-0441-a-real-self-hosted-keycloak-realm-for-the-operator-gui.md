## ADR-0441: A real, self-hosted Keycloak realm for the operator GUI -- closing ADR-0424's main deferral

**Date:** 2026-09-27. **Status:** accepted; live, tested end-to-end with a real Keycloak container
and the real `oam-gui-bff` binary. Closes ADR-0424's "a live Keycloak realm was not run on the
shared 16 GB machine" deferral. Back-channel logout and per-action step-up remain deferred (below).

**Decision.**

1. **Keycloak 26.0 (Apache-2.0, quay.io/keycloak/keycloak), real container, both deployment
   targets.** `deploy/docker/docker-compose.yml` gets a `keycloak` service; `deploy/helm/keycloak/`
   gets a chart matching this project's existing per-NF Helm convention (`deploy/helm/udr/` as the
   template: Chart.yaml/values.yaml/templates, same disclosed "no PKI provisioning, no HA" gap
   category the existing charts already carry). TLS is the SAME lab CA every NF uses
   (`scripts/gen-lab-pki.sh keycloak`) -- `oam-gui-bff`'s existing `idp_ca_path` (`certs/ca/ca.crt`)
   already pointed at that CA and needed no change.

2. **Database placement: a separate `keycloak` database on the SAME `postgres-chf` instance that
   already hosts `operator_iam` (compose), not a dedicated PostgreSQL container.** Checked against
   this project's two live conventions before deciding, per the task's explicit instruction to
   verify rather than assume:
   - *DB-per-domain rule* (`feedback_db_per_domain_rule` memory): "consolidate to one instance,
     split to a dedicated instance only by load" -- Keycloak's IAM data is a new, small, distinct
     domain, not a load-bearing one; `operator_iam` (the domain it is most tightly coupled to)
     already lives on `postgres-chf` as its own DATABASE, not a schema -- same pattern, so this
     follows precedent rather than inventing a new one.
   - *Resource constraint already on record*: ADR-0424 itself states a live IdP "was not run on the
     shared 16 GB machine" -- adding a whole second PostgreSQL container for a small IAM store
     fights that constraint for no isolation benefit an already-running, already-trusted Postgres
     doesn't provide.
   `init-domain-dbs.sh` gets one `create_db keycloak` line -- CREATE only, no DDL: Keycloak migrates
   and owns that database's schema itself (Liquibase, at boot), so this project's DDL never touches
   it, unlike `operator_iam`/`charging`/`orchestration`.
   **Rejected:** a dedicated `postgres-keycloak` container/instance -- matches UDR's/ADRF's reasons
   for a separate instance (real load isolation, different scaling profile) not present here; would
   also be one more moving part on the resource-constrained lab machine the deferral itself named.
   The **Helm chart** cannot follow this same placement: there is no shared PostgreSQL *release* in
   this project's Helm charts to attach to (only per-NF in-memory/no-DB charts exist so far), so it
   brings its own dedicated Postgres Deployment+PVC, disclosed in `Chart.yaml` as a divergence from
   the compose deployment's placement, not an inconsistency papered over.

3. **Realm provisioned by rendering a checked-in template, not a hand-run admin-console click-path
   or a committed realm export with real secrets in it.**
   `deploy/keycloak/realm-5gc-r19-operators.json.tmpl` is the realm (client, auth flow, users) with
   `__PLACEHOLDER__` tokens where secrets go; `deploy/keycloak/render-realm.sh` generates (once,
   idempotently) a client secret, two lab users' passwords, and two lab users' TOTP secrets -- all
   under `certs/` (covered by the existing `/certs/` gitignore rule, same as every other lab
   PKI/secret this project generates) -- substitutes them, and writes `deploy/keycloak/import/`
   (also gitignored), which Keycloak's `--import-realm` reads once at first boot against an empty
   `keycloak` database. `deploy/keycloak/totp.py` computes a seeded lab user's live 6-digit code
   from its generated secret, for interactive or scripted login.
   **Rejected:** a `kcadm.sh`/Admin-REST bootstrap script run against a live server -- works (it is
   literally how this ADR's own configuration was *discovered*, see point 5) but is imperative and
   non-reviewable as a diff; a plain committed `realm.json` with real secrets in git -- violates the
   standing "never commit secrets" rule and would ship the same credential to every clone.

4. **The BFF's OIDC client (`oam-gui`, confidential, `client_secret_post`, PKCE S256 enforced) is
   unchanged from ADR-0424/ADR-0422** -- this ADR wires it to a real IdP, it does not rename or
   reshape it. `config/oam-gui-bff.json`'s `oidc` block already had every field this needed
   (`issuer`/`authorization_endpoint`/`token_endpoint`/`jwks_uri`/`client_id`/`client_secret_file`/
   `idp_ca_path`) pointed at `https://127.0.0.1:8443/realms/5gc-r19-operators` -- only two VALUES
   changed: `acr_values` (`""` -> `"mfa"`) and `mfa_acr` (`[]` -> `["mfa"]`), per point 5. No new
   config KEY was added, so no NF-config schema key changed; the derived schema still picked up a
   real diff (`mfa_acr`'s item type, previously unknown from an empty array) --
   `gui/schema-gen/derive_nf_config_schemas.py` regenerated and `--check` is clean.

5. **MFA verification, empirically -- not assumed, because two real Keycloak behaviours are easy
   to get backwards and this project's own guardrails forbid shipping a guess:**
   - **Keycloak does not populate the OIDC `amr` claim** (a long-standing, real gap, confirmed by
     driving the built-in `browser` flow's default "OTP if configured" subflow end-to-end against a
     real 26.0 container and inspecting the decoded ID token: real password + real TOTP OTP
     produced `"acr": "1"` and no `amr` claim at all). `OidcAuthenticator::complete` (`auth.cpp`)
     already checks `amr` OR `acr`, so this is disclosed, not a code defect -- `mfa_amr` stays
     configured for a future IdP that does populate it, but Keycloak needs the `acr` path.
   - **Getting a real, checkable `acr` claim needs Keycloak's step-up (Level-of-Authentication)
     mechanism**, not the built-in "OTP if the user has it configured" subflow: the realm's
     `browser` flow is copied (`5gc-r19 browser`) and its OTP subflow's condition is swapped from
     `conditional-user-configured` to `conditional-level-of-authentication` (config
     `loa-condition-level: "1"`), and the `oam-gui` client gets `attributes.acr.loa.map =
     {"mfa":1}`. `oam-gui-bff` requests `acr_values=mfa` on every `/auth/login` (the `acr_values`
     config field, already present, unused until now). Verified: the resulting real, RS256-signed
     ID token carries `"acr": "mfa"` -- exactly what `mfa_acr: ["mfa"]` checks.
   - **Rejected: `default.acr.values` on the client**, which would let the realm request step-up
     for ANY caller without the RP needing to send `acr_values` itself (the more robust design).
     Setting it (with `acr.loa.map` already present) was rejected BY KEYCLOAK ITSELF at every
     ordering tried: `"Default ACR values need to contain values specified in the ACR-To-Loa
     mapping or number levels from set realm browser flow"`, reproducibly, including with the
     referenced map present in the same and in a prior request. Not chased further given the
     working alternative (point above) and a real, disclosed finding instead: even WITHOUT
     `acr_values` sent at all, this realm's copied OTP subflow still demanded OTP (verified) --
     Keycloak's condition executor appears to treat an unsatisfied configured level as
     required-until-satisfied regardless of an explicit request, which is a safety net, not a
     substitute for `oam-gui-bff` always sending `acr_values=mfa` (the only path proven to make the
     resulting `acr` claim read `"mfa"` specifically, which is what `mfa_acr` actually checks).
   - **Keycloak's stored TOTP secret is NOT base32-decoded before use as the HMAC-SHA1 key -- the
     raw UTF-8 bytes of the secret string are the key**, same as RFC 6238's own test vectors (whose
     reference key `"12345678901234567890"` is likewise used as raw ASCII, never base32-decoded).
     Verified by seeding a credential both ways against a real container: a base32-decoded key was
     rejected ("Invalid authenticator code"); the raw bytes of the identical secret string were
     accepted and the login succeeded. `deploy/keycloak/totp.py` implements it this way and cites
     this finding; easy to get backwards from a generic TOTP library's assumptions, so written down
     rather than left to be rediscovered.

6. **No roles or groups mirrored into Keycloak.** `oam-gui-bff`'s `OidcAuthenticator::complete`
   reads exactly `sub`, `sid`, `acr`, `amr`, `nonce`, `aud`/`azp` from the ID token and nothing else
   -- authorization is entirely `operator_iam`'s (ADR-0423), resolved from `(idp_issuer,
   idp_subject)` after the token is verified. Grepped before writing the realm, per the task's
   explicit instruction not to invent a role/claim the BFF doesn't already expect.
   **Rejected:** mirroring the ten `operator_iam` roles into Keycloak groups/roles and mapping them
   into a token claim -- would create a second, ignored source of truth (the BFF would never read
   it) for no benefit, and risks the two drifting silently.

7. **Two lab operator users, seeded outside `deploy/db/operator_iam/`.**
   `deploy/keycloak/seed-lab-operators.sql` inserts `operator_user`/`role_assignment` rows for the
   two users `render-realm.sh` provisions (`shop.agent.lab` -> role `shop_agent`; `security.
   admin.lab` -> role `security_admin`, the bootstrap grantor, `granted_by NULL` per ADR-0423's
   documented bootstrap exception -- `shop.agent.lab`'s grant has a real grantor, not NULL, since it
   is not the bootstrap case). Deliberately NOT under `deploy/db/operator_iam/`: that directory is
   globbed wholesale both by `init-domain-dbs.sh` (every deployment) and by
   `oam_gui_bff_security_tests`' `IAM_DDL_DIR` (its own private test database) -- lab login
   credentials should not ship by default into either. Applied explicitly, after
   `deploy/db/operator_iam/*.sql`, only where a deployment wants the lab users.

**What was actually run, not just written** (the task's bar: "a Keycloak container that starts" is
not done; a real round-trip succeeding is):
- A real Keycloak 26.0 container in **production mode** (`start`, not `start-dev`) against a real
  PostgreSQL 16, importing the checked-in realm template (rendered) -- confirmed by the server's own
  log: `Realm '5gc-r19-operators' imported` / `Import finished successfully`.
- The real `oam-gui-bff` binary, built from this worktree via the project's normal CMake/vcpkg path
  (no test harness, no mock), run against that container and a real (private, throwaway --
  `deploy/db/operator_iam/*.sql` applied fresh) `operator_iam` PostgreSQL.
- **Positive path:** `curl` presenting a real operator-CA terminal client certificate (mTLS) hit the
  real `/auth/login` -> real redirect to the real Keycloak authorization endpoint with PKCE +
  `acr_values=mfa` -> real Keycloak login form (password) -> real TOTP OTP (computed by
  `deploy/keycloak/totp.py` from the seeded secret) -> real redirect back with a real authorization
  code -> the real `/auth/callback` exchanged it (client_secret_post + PKCE verifier), fetched the
  real JWKS, verified the real RS256-signed ID token (issuer/audience/azp/nonce/exp, `kid` matched a
  live JWKS key), resolved `shop.agent.lab` in `operator_iam` by `(issuer, sub)`, and issued a real
  `__Host-oam_session` cookie. `iam.audit_event` recorded it: `action=auth.login outcome=ALLOWED
  username=shop.agent.lab terminal_cn=terminal-lab-1`.
- **Negative path, against the same real IdP** (not just the fake-IdP suite): the same user's
  `operator_iam` status flipped to `LOCKED` mid-session; a fresh login (real password, real correct
  OTP, real Keycloak success) was still refused by `oam-gui-bff` itself with `403 Sign-in was
  refused`, audited `outcome=DENIED reason="user status is LOCKED"`. Status reverted afterward.
- **The fake-IdP path (ADR-0424's own test suite) reran unchanged and green**: `oam_gui_bff_
  security_tests`, 10/10, including `OidcLoginWithMfaCreatesATerminalBoundSessionAndIsAudited` and
  `LoginWithoutMfaEvidenceIsRefusedAndAudited` -- this ADR adds a real deployment target, it does
  not touch or replace what that suite exercises. `oam_gui_bff_unit_tests`, 12/12, also green (the
  NF-config schema regeneration in point 4 is covered by
  `NfConfig.ValidatorAcceptsTheRealFileAndRejectsDrift`).

**Disclosed, deferred (explicitly, not silently):**
- **`oam-gui-bff` itself has no Dockerfile or compose/Helm entry.** This predates this ADR (Phase 7
  built and tested it as a CMake binary; nothing in ADR-0420..0425 containerized it) and is outside
  this task's scope, which was the IdP, not the BFF's own packaging -- named here rather than
  papered over by, e.g., quietly writing one under this ADR's number. The end-to-end proof above
  therefore runs `oam-gui-bff` as a host binary against a containerized Keycloak; a containerized
  BFF would need `token_endpoint`/`jwks_uri` reachable at Keycloak's in-cluster/in-network name
  (e.g. `keycloak:8443`) while `issuer` stays the externally-visible `https://127.0.0.1:8443/...`
  (or the real ingress hostname) -- both endpoints are independently configurable already
  (`config/oam-gui-bff.json`'s `oidc` block), so this needs a values/config change when it happens,
  not new code.
- **Back-channel logout**: `end_session_endpoint`/`post_logout_redirect_uri` are configured and
  `logout_url()` (ADR-0424) builds a real URL, but nothing registers a back-channel logout URI on
  the Keycloak client or handles a callback from one. Still deferred.
- **Per-action step-up**: the MECHANISM now exists and is proven (point 5) -- a second named ACR
  level (e.g. `acr.loa.map: {"mfa":1,"stepup":2}`) plus a second LoA condition in the flow would
  give a sensitive-action-specific re-authentication step. Not built: `oam-gui-bff` has no per-
  action trigger for it yet (ADR-0424's deferral, narrowed but not closed).
- **No Keycloak HA/clustering** (single replica, both deployment targets) -- consistent with every
  other lab-grade chart's disclosed gap, not a new one.
- **The Helm chart was not run through `helm lint`/`helm template`/`helm install`** -- no `helm`
  binary in this environment. Checked by hand against `deploy/helm/udr/`'s working templates (same
  Go-template-in-YAML shape, confirmed neither parses as plain YAML -- expected, not a defect) and
  by the same author who wrote and verified the compose path's identical logic; genuinely unverified
  against a real cluster, stated plainly rather than implied tested.
- **Neither deployment target provisions its own PKI** -- the compose `keycloak` service expects
  `scripts/gen-lab-pki.sh keycloak` to have run (now in `pki-init`'s NF list); the Helm chart expects
  a pre-created TLS secret. Same disclosed shape as every other chart in `deploy/helm/`.

**ADR-0424 status:** its "Keycloak realm export + compose/Helm entry" deferral is closed by this
ADR. "back-channel logout" and "step-up (acr) per sensitive action" remain open, narrowed as above.

