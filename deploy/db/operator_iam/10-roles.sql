-- operator_iam default permission catalogue, roles, SoD pairs, four-eyes policy and field policy
-- (ADR-0423). DATA, not code: an operator edits these rows (through the maker-checker role-grant
-- path once its screen lands) without a BFF release. The BFF knows permission ids only as the
-- strings below; roles are never named in C++.
SET search_path = iam, public;

INSERT INTO org_unit(id, kind, parent_id, name) VALUES ('hq', 'HQ', NULL, 'Headquarters');

INSERT INTO permission(id, resource, action, description) VALUES
 ('customer_order:create',          'customer_order',   'create', 'Submit a customer onboarding order'),
 ('customer_order:read',            'customer_order',   'read',   'Read onboarding orders (PII masked)'),
 ('pii:unmask',                     'pii',              'unmask', 'See PII unmasked, with a stated reason'),
 ('product_offering:read',          'product_offering', 'read',   'Read TMF620 product offerings'),
 ('product_offering:create',        'product_offering', 'create', 'Propose a TMF620 product offering (four-eyes)'),
 ('product_offering_price:read',    'product_offering_price', 'read',   'Read TMF620 prices'),
 ('product_offering_price:create',  'product_offering_price', 'create', 'Propose a TMF620 price (four-eyes)'),
 ('catalog_approval:decide',        'catalog_approval', 'decide', 'Approve or reject catalog changes'),
 ('nf_config:read',                 'nf_config',        'read',   'Read NF configuration (credentials masked)'),
 ('nf_config:change',               'nf_config',        'change', 'Propose an NF configuration change or rollback (four-eyes)'),
 ('config_approval:decide',         'config_approval',  'decide', 'Approve or reject NF configuration changes'),
 ('role_grant:create',              'role_grant',       'create', 'Propose a role grant (four-eyes)'),
 ('role_grant_approval:decide',     'role_grant_approval', 'decide', 'Approve or reject role grants'),
 ('user:manage',                    'user',             'manage', 'Joiner / mover / leaver administration'),
 ('audit:read',                     'audit',            'read',   'Read and export the audit trail');

INSERT INTO role(id, name, description, privileged) VALUES
 ('shop_agent',         'Shop agent',               'Onboards customers of their own shop', false),
 ('shop_supervisor',    'Shop supervisor',          'Shop agent + PII unmask for their shop(s)', false),
 ('back_office_agent',  'Back-office agent',        'Reads orders across its anchor unit, PII unmask', false),
 ('product_manager',    'Product / tariff manager', 'Proposes catalog changes', true),
 ('catalog_approver',   'Catalog approver',         'Checker for catalog changes', true),
 ('network_config_engineer', 'Network configuration engineer', 'Proposes NF configuration changes', true),
 ('network_config_approver', 'Network configuration approver', 'Checker for NF configuration changes', true),
 ('security_admin',     'Security administrator',   'Proposes role grants, manages user lifecycle', true),
 ('security_approver',  'Security approver',        'Checker for role grants', true),
 ('auditor',            'Read-only auditor',        'Reads the audit trail and all records, changes nothing', true);

INSERT INTO role_permission(role_id, permission_id, scope) VALUES
 ('shop_agent', 'customer_order:create', 'OWN_SUBTREE'),
 ('shop_agent', 'customer_order:read', 'OWN_SUBTREE'),
 ('shop_agent', 'product_offering:read', 'GLOBAL'),
 ('shop_agent', 'product_offering_price:read', 'GLOBAL'),
 ('shop_supervisor', 'customer_order:create', 'OWN_SUBTREE'),
 ('shop_supervisor', 'customer_order:read', 'OWN_SUBTREE'),
 ('shop_supervisor', 'pii:unmask', 'OWN_SUBTREE'),
 ('shop_supervisor', 'product_offering:read', 'GLOBAL'),
 ('shop_supervisor', 'product_offering_price:read', 'GLOBAL'),
 ('back_office_agent', 'customer_order:read', 'OWN_SUBTREE'),
 ('back_office_agent', 'pii:unmask', 'OWN_SUBTREE'),
 ('back_office_agent', 'product_offering:read', 'GLOBAL'),
 ('back_office_agent', 'product_offering_price:read', 'GLOBAL'),
 ('product_manager', 'product_offering:read', 'GLOBAL'),
 ('product_manager', 'product_offering:create', 'GLOBAL'),
 ('product_manager', 'product_offering_price:read', 'GLOBAL'),
 ('product_manager', 'product_offering_price:create', 'GLOBAL'),
 ('catalog_approver', 'product_offering:read', 'GLOBAL'),
 ('catalog_approver', 'product_offering_price:read', 'GLOBAL'),
 ('catalog_approver', 'catalog_approval:decide', 'GLOBAL'),
 ('network_config_engineer', 'nf_config:read', 'GLOBAL'),
 ('network_config_engineer', 'nf_config:change', 'GLOBAL'),
 ('network_config_approver', 'nf_config:read', 'GLOBAL'),
 ('network_config_approver', 'config_approval:decide', 'GLOBAL'),
 ('security_admin', 'role_grant:create', 'GLOBAL'),
 ('security_admin', 'user:manage', 'GLOBAL'),
 ('security_approver', 'role_grant_approval:decide', 'GLOBAL'),
 ('auditor', 'audit:read', 'GLOBAL'),
 ('auditor', 'customer_order:read', 'GLOBAL'),
 ('auditor', 'product_offering:read', 'GLOBAL'),
 ('auditor', 'product_offering_price:read', 'GLOBAL'),
 ('auditor', 'nf_config:read', 'GLOBAL');

INSERT INTO sod_rule(id, permission_a, permission_b, description) VALUES
 ('sod-catalog', 'product_offering:create', 'catalog_approval:decide', 'Catalog maker cannot be a catalog checker'),
 ('sod-catalog-price', 'product_offering_price:create', 'catalog_approval:decide', 'Price maker cannot be a catalog checker'),
 ('sod-config', 'nf_config:change', 'config_approval:decide', 'Config maker cannot be a config checker'),
 ('sod-grant', 'role_grant:create', 'role_grant_approval:decide', 'Grant maker cannot be a grant checker');

INSERT INTO approval_policy(permission_id, threshold_amount, threshold_currency, approver_permission) VALUES
 ('product_offering:create', NULL, NULL, 'catalog_approval:decide'),
 ('product_offering_price:create', NULL, NULL, 'catalog_approval:decide'),
 ('nf_config:change', NULL, NULL, 'config_approval:decide'),
 ('role_grant:create', NULL, NULL, 'role_grant_approval:decide');

INSERT INTO field_policy(resource, field_path, classification, unmask_permission) VALUES
 ('customer_order', 'sim.k', 'SECRET_WRITE_ONLY', NULL),
 ('customer_order', 'sim.opc', 'SECRET_WRITE_ONLY', NULL),
 ('customer_order', 'supi', 'PII', 'pii:unmask'),
 ('customer_order', 'msisdn', 'PII', 'pii:unmask'),
 ('customer_order', 'individual.givenName', 'PII', 'pii:unmask'),
 ('customer_order', 'individual.familyName', 'PII', 'pii:unmask'),
 ('customer_order', 'individual.email', 'PII', 'pii:unmask'),
 ('customer_order', 'individual.phone', 'PII', 'pii:unmask'),
 -- Response-side: provisioning's account/subscriber/customer ids embed the SUPI's digits.
 ('customer_order_response', 'supi', 'PII', 'pii:unmask'),
 ('customer_order_response', 'msisdn', 'PII', 'pii:unmask'),
 ('customer_order_response', 'accountId', 'PII', 'pii:unmask'),
 ('customer_order_response', 'subscriberId', 'PII', 'pii:unmask'),
 ('customer_order_response', 'customerId', 'PII', 'pii:unmask');
