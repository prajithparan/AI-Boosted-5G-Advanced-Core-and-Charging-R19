## ADR-0423: Operator identity, access, maker-checker and audit -- the `operator_iam` domain

**Date:** 2026-09-26. **Status:** accepted; implemented for login, onboarding, catalog proposals,
approvals and NF config; deferrals listed. Owner requirement: Tier-1 security governance for shop
agents and back office.

**Where.** Its own domain DB `operator_iam` (schema `iam`) on the postgres-chf instance, DDL in
`deploy/db/operator_iam/` (00-schema, 10-roles), applied by `deploy/db/init-domain-dbs.sh`
(DB-per-domain rule). The BFF connects as the least-privileged `oam_gui_bff` role (created by the
DDL, credentials set by deployment): INSERT/SELECT on the audit trail, never UPDATE/DELETE.

**Model.**
- *Organisation:* `org_unit` (HQ, CHANNEL, DEALER, SHOP, BACK_OFFICE, NOC) with a closure table,
  so "is shop S inside a grant anchored at U" is one indexed lookup at any depth. Re-parenting is
  refused (mover = close + recreate) so the closure cannot go stale. `terminal` registers shop PCs
  by their operator-CA certificate CN.
- *Users:* `operator_user` holds the IdP issuer+subject, never a credential; status ACTIVE / LOCKED
  / DORMANT / LEFT (joiner/leaver), `operator_user_move` (mover history, mover != moved), dormancy
  from `auth_policy.dormant_after_days` enforced at login (the account is locked and the attempt
  audited), `mfa_required` per user.
- *Authorization, data-driven:* `permission` (resource, action) rows; `role` rows (seeded: shop
  agent, shop supervisor, back-office agent, product/tariff manager, catalog approver, network
  config engineer/approver, security admin/approver, read-only auditor -- editable data, never named
  in C++); `role_permission` with scope OWN_SUBTREE or GLOBAL; `role_assignment` anchored at an org
  unit with validity window, revocation, and a CHECK that the grantor is not the grantee.
  `sod_rule` pairs (catalog maker vs checker, config maker vs checker, grant maker vs checker).
- *Field-level:* `field_policy` -- SECRET_WRITE_ONLY (SIM K/OPc: accepted, forwarded once, never
  returned/stored/audited; also scrubbed from any upstream response that echoed them, either case),
  PII (masked in every response unless `pii:unmask` in scope AND a stated reason, the unmask itself
  audited), CREDENTIAL (config secrets, masked; "unchanged" when the mask is sent back).
- *Maker-checker:* `approval_policy` says which permissions need four eyes and which permission
  approves; `approval_request` stores the (secret-free) payload + sha256, and a DB CHECK
  `decided_by <> requested_by` plus a trigger refusing a grantee deciding their own role grant --
  the BFF refuses first, the DB refuses even if the BFF is bypassed (both tested). Execution happens
  under the checker's session; result stored on the request.
- *Scope registry:* the provisioning API carries no shop, so the BFF CLAIMS the SUPI for the
  caller's unit (`customer_ownership`, insert-or-read, race-free) before forwarding and records
  `order_ownership` from the response. Idempotency keys are rewritten to `<unit>.<key>`, so a key
  typed in shop B can never resume (and so read) shop A's order. Out-of-scope reads answer 404,
  identical to unknown (no cross-shop existence oracle).
- *Sessions:* `operator_session` stores only sha256(cookie); bound to the terminal certificate it
  was opened on -- the cookie replayed from another terminal is refused and the session ended
  (tested); idle and absolute timeouts from `auth_policy`. `login_state` holds OIDC state/nonce/PKCE,
  single use, bound to the browser by a Lax login cookie.
- *Audit:* `audit_event`, partitioned by month (+ DEFAULT partition, `ensure_audit_partition` run at
  BFF start-up), who / session / unit / terminal CN / IP / action / resource / customer (SUPI) /
  outcome (ALLOWED, DENIED, ERROR, PENDING_APPROVAL, APPROVED, REJECTED) / status / reason /
  before-after (secrets stripped). Hash-chained per `chain_key` (one chain per BFF instance, so
  instances never contend): a BEFORE INSERT trigger takes a per-chain advisory lock, assigns
  `chain_seq`, links `prev_hash`, computes `row_hash` = sha256 over a deterministic canonical text.
  UPDATE/DELETE/TRUNCATE are not granted AND refused by triggers; `audit_verify_chain()` finds an
  edited or truncated chain (tested by a superuser disabling triggers and editing a row).
  **Tamper-evident, not tamper-proof:** a superuser could rewrite a whole chain consistently; the
  remedy (periodic export of chain heads to WORM/external storage) is deferred, below.
  `audit_export` is the reviewer/export view.
- *Order of operations in every handler:* session -> CSRF -> permission+scope -> AUDIT the decision
  -> act -> audit the result. If the decision's audit write fails, the action is refused (503): no
  change without a prior audit row. Denied and unauthenticated attempts are audited too (tested).
  Handler exceptions are logged generically (pqxx/json texts can quote values).

**Identity propagation to BSS/NF calls.** Implemented now: the BFF writes the attributed audit
itself (real operator, unit, terminal, approval id) for every call it makes; the services' own
`audit_record.actor` still says the service name. Designed, deferred: a short-lived JWS
`x-oam-actor` header signed by the BFF's key, verified by the BSS services and written into their
`audit_record.actor` -- needs a verification step in every BSS service, a separate increment.

**Tier-1 scale.** Indexes for per-shop and per-user queries on grants, ownership, approvals,
sessions and audit; monthly audit partitions; per-instance chains avoid a global lock. Each BFF
replica MUST get its own `audit_chain_key` (env `OAM_GUI_BFF_AUDIT_CHAIN_KEY`, e.g. the pod name):
replicas sharing a key still produce a valid chain but contend on its lock.

**Rejected.** *Home-grown passwords/MFA* (ADR-0424). *Roles hardcoded in C++* (every role change
would be a release). *One global audit chain* (serialises every instance). *Relying on REVOKE alone*
(lab/CI connect as superuser; triggers + verification cover that). *A shop column added to the
provisioning API* (changes a service contract to serve the GUI; the registry keeps it in the GUI's
domain -- revisit when TMF622 channel/shop lands in orchestration).

**Deferred (disclosed):** role-grant / user-lifecycle API and screens (DDL + permissions + DB
checks exist; SoD is checked at grant time, which is exactly that API -- a seed can still create a
violating user, and the four-eyes rule is proven to hold even then); terminal-to-shop binding
enforcement (`terminal` table exists, sessions record the CN); `x-oam-actor` propagation; audit
partition rotation/retention job and WORM export of chain heads; balance-adjustment thresholds
(`approval_policy.threshold_*` exists, no balance screen yet); CI wiring -- the security tests read
`TEST_POSTGRES_URL` (CI's postgres-chf) and are labelled `needs-postgres`; whether ci.yml exposes
that variable on the ctest step is for the integrator to confirm.

