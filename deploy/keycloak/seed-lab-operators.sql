-- Lab-only operator_iam rows for the two Keycloak users render-realm.sh provisions (ADR-0441).
-- Deliberately NOT under deploy/db/operator_iam/: that directory is (a) globbed wholesale by
-- init-domain-dbs.sh into every deployment's operator_iam database and (b) globbed by
-- oam_gui_bff_security_tests' IAM_DDL_DIR into its own private test database. Neither should pick
-- up lab login credentials by default. Apply explicitly, after deploy/db/operator_iam/*.sql:
--   psql <operator_iam DB URL> -f deploy/keycloak/seed-lab-operators.sql
--
-- idp_subject values are the Keycloak user `id`s pinned in
-- deploy/keycloak/realm-5gc-r19-operators.json.tmpl (so this file does not depend on the render
-- step's random secrets/passwords, only on the realm template's fixed ids). idp_issuer must match
-- config/oam-gui-bff.json's oidc.issuer for the deployment this is applied to.
SET search_path = iam, public;

INSERT INTO operator_user
    (id, idp_issuer, idp_subject, username, display_name, email, home_org_unit_id, status, mfa_required)
VALUES
    ('lab-security-admin', 'https://127.0.0.1:8443/realms/5gc-r19-operators',
     '5c1e6b7a-0002-4a11-9c2e-0a1b2c3d4e5f', 'security.admin.lab', 'Lab SecurityAdmin',
     'security.admin.lab@lab.invalid', 'hq', 'ACTIVE', true),
    ('lab-shop-agent', 'https://127.0.0.1:8443/realms/5gc-r19-operators',
     '5c1e6b7a-0001-4a11-9c2e-0a1b2c3d4e5f', 'shop.agent.lab', 'Lab ShopAgent',
     'shop.agent.lab@lab.invalid', 'hq', 'ACTIVE', true)
ON CONFLICT (id) DO NOTHING;

-- Bootstrap grant: nobody grants the first security admin (CHECK granted_by IS DISTINCT FROM
-- user_id already forbids self-grant; granted_by NULL is the documented bootstrap exception,
-- ADR-0423). role_assignment.id has no default -- supplied explicitly.
INSERT INTO role_assignment (id, user_id, role_id, org_unit_id, granted_by)
VALUES ('lab-grant-security-admin', 'lab-security-admin', 'security_admin', 'hq', NULL)
ON CONFLICT (id) DO NOTHING;

-- The shop agent's grant has a real grantor (the lab security admin), not NULL, since it is not
-- the bootstrap case.
INSERT INTO role_assignment (id, user_id, role_id, org_unit_id, granted_by)
VALUES ('lab-grant-shop-agent', 'lab-shop-agent', 'shop_agent', 'hq', 'lab-security-admin')
ON CONFLICT (id) DO NOTHING;
