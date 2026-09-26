#pragma once

// IamStore: oam-gui-bff's access to the operator_iam database (ADR-0423). Every authorization
// decision is a query here -- never a flag the browser sent. The BFF connects as the
// `oam_gui_bff` role, which can INSERT audit rows but never UPDATE/DELETE them.

#include <nf_config/pg_pool.hpp>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <vector>

namespace oam_gui_bff {

// Who is calling, resolved from a live session + the connection it arrived on.
struct Principal {
    std::string user_id;
    std::string username;
    std::string display_name;
    std::string session_id;   // sha256 of the cookie; never the cookie itself
    std::string active_unit;  // org unit the session works in (the user's home unit at login)
    std::string terminal_cn;  // operator mTLS certificate CN
    std::string client_ip;
};

struct AuditEvent {
    const Principal* who = nullptr; // nullptr: unauthenticated
    std::string terminal_cn;        // used when `who` is null
    std::string client_ip;          // used when `who` is null
    std::string action;             // permission id or auth.* event
    std::string resource_ref;
    std::string customer_ref;
    std::string outcome;            // ALLOWED | DENIED | ERROR | PENDING_APPROVAL | APPROVED | REJECTED
    int http_status = 0;
    std::string reason;
    std::optional<nlohmann::json> before;
    std::optional<nlohmann::json> after;
    std::string approval_request_id;
};

struct UserRecord {
    std::string id, username, display_name, home_unit, status;
    bool mfa_required = true;
    bool dormant = false; // last login older than auth_policy.dormant_after_days
};

struct FieldRule {
    std::string path;           // dotted
    std::string classification; // SECRET_WRITE_ONLY | PII | CREDENTIAL
    std::string unmask_permission;
};

struct ApprovalRequest {
    std::string id, permission_id, org_unit_id, target_ref, requested_by, requested_by_name,
        requested_at, reason, status, decided_by, decision_reason, expires_at;
    nlohmann::json payload;
    std::optional<nlohmann::json> execution_result;
};

struct LoginState {
    std::string nonce, code_verifier, terminal_cn;
};

struct ConfigVersion {
    int version = 0;
    std::string created_by, created_at, applied_at, comment, content_sha256;
    bool restart_required = true;
};

class IamStore {
public:
    IamStore(const std::string& conninfo, std::size_t pool_size, std::string chain_key);

    // Creates this and next month's audit partition (idempotent; DEFAULT catches the rest).
    void ensure_audit_partitions();

    // ---- audit: throws on failure. Callers deny the action if the audit write fails. ----
    void audit(const AuditEvent& e);

    // ---- login state (OIDC state/nonce/PKCE), single use ----
    void put_login_state(const std::string& state, const std::string& nonce,
                         const std::string& code_verifier, const std::string& browser_binding,
                         const std::string& terminal_cn);
    std::optional<LoginState> take_login_state(const std::string& state,
                                               const std::string& browser_binding);

    // ---- users & sessions ----
    std::optional<UserRecord> find_user(const std::string& issuer, const std::string& subject);
    void mark_dormant(const std::string& user_id);
    // Returns the opaque cookie value; the DB only ever holds its sha256.
    std::string create_session(const UserRecord& user, const std::string& auth_method,
                               const std::string& idp_session_id, const std::string& acr,
                               const std::vector<std::string>& amr, const std::string& terminal_cn,
                               const std::string& client_ip, const std::string& user_agent);
    // Resolves a cookie to a live session: ended, idle-expired, absolute-expired, user no longer
    // ACTIVE, or presented from a different terminal certificate -> nullopt with `why` set (and
    // the session ended where that applies).
    std::optional<Principal> resolve_session(const std::string& cookie_value,
                                             const std::string& terminal_cn,
                                             const std::string& client_ip, std::string& why);
    void end_session(const std::string& session_id, const std::string& reason);

    // ---- authorization ----
    // Does the user hold `permission` with a scope covering `org_unit` (GLOBAL grants cover all)?
    bool allowed(const std::string& user_id, const std::string& permission,
                 const std::string& org_unit);
    // Does the user hold `permission` anywhere (for listing screens they may see at all)?
    bool allowed_anywhere(const std::string& user_id, const std::string& permission);
    std::vector<std::string> permissions(const std::string& user_id);
    std::vector<FieldRule> field_rules(const std::string& resource);
    // The permission that approves requests for `permission`, if it needs four-eyes.
    std::optional<std::string> approver_permission(const std::string& permission);

    // ---- scope registry (customers / orders a unit owns) ----
    enum class Claim { Claimed, AlreadyOurs, OtherUnit };
    Claim claim_customer(const std::string& supi, const std::string& unit,
                         const std::string& user_id);
    void release_customer(const std::string& supi, const std::string& unit);
    void record_order(const std::string& order_id, const std::string& supi,
                      const std::string& unit, const std::string& user_id);
    // (owning unit, supi) of an order, if the BFF created it.
    std::optional<std::pair<std::string, std::string>> order_owner(const std::string& order_id);

    // ---- maker-checker ----
    std::string create_approval(const std::string& permission, const std::string& unit,
                                const std::string& target_ref, const nlohmann::json& payload,
                                const std::string& reason, const std::string& requested_by);
    std::optional<ApprovalRequest> approval(const std::string& id);
    // Moves PENDING -> APPROVED/REJECTED. Returns false if not pending, expired, or the decider
    // is the requester (the DB CHECK refuses that too; this is the first line).
    bool decide(const std::string& id, const std::string& decider, bool approve,
                const std::string& reason);
    void mark_executed(const std::string& id, bool ok, const nlohmann::json& result);
    std::vector<ApprovalRequest> approvals(const std::string& status, int limit);

    // ---- NF configuration versions ----
    int add_config_version(const std::string& nf, const nlohmann::json& content,
                           const std::string& created_by, const std::string& approval_id,
                           const std::string& comment);
    void mark_config_applied(const std::string& nf, int version);
    std::vector<ConfigVersion> config_versions(const std::string& nf);
    std::optional<nlohmann::json> config_version(const std::string& nf, int version);

private:
    nf_config::PgPool pool_;
    std::string chain_key_;
};

} // namespace oam_gui_bff
