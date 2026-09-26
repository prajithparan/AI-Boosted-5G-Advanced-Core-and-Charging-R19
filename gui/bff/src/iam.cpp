#include "iam.hpp"

#include "util.hpp"

#include <pqxx/pqxx>

#include <utility>

namespace oam_gui_bff {

using nlohmann::json;

namespace {

std::optional<std::string> opt(const std::string& s) {
    return s.empty() ? std::nullopt : std::optional<std::string>(s);
}
std::optional<std::string> opt_json(const std::optional<json>& j) {
    return j.has_value() ? std::optional<std::string>(j->dump()) : std::nullopt;
}
std::string str(const pqxx::field_ref& f) { return f.is_null() ? std::string() : f.as<std::string>(); }

constexpr const char* kTs = "YYYY-MM-DD\"T\"HH24:MI:SS\"Z\"";
std::string ts(const std::string& col) {
    return "to_char(" + col + " AT TIME ZONE 'UTC', '" + kTs + "')";
}

} // namespace

IamStore::IamStore(const std::string& conninfo, std::size_t pool_size, std::string chain_key)
    : pool_(conninfo, pool_size), chain_key_(std::move(chain_key)) {}

void IamStore::ensure_audit_partitions() {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("SELECT iam.ensure_audit_partition(now()), "
           "iam.ensure_audit_partition(now() + INTERVAL '1 month')");
    t.commit();
}

void IamStore::audit(const AuditEvent& e) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    const Principal* p = e.who;
    t.exec("INSERT INTO iam.audit_event(chain_key, user_id, username, session_id, org_unit_id, "
           "terminal_cn, client_ip, action, resource_ref, customer_ref, outcome, http_status, "
           "reason, before_state, after_state, approval_request_id) VALUES "
           "($1,$2,$3,$4,$5,$6,$7,$8,$9,$10,$11,$12,$13,$14::jsonb,$15::jsonb,$16)",
           pqxx::params{chain_key_,
                        p ? opt(p->user_id) : std::nullopt,
                        p ? opt(p->username) : std::nullopt,
                        p ? opt(p->session_id) : std::nullopt,
                        p ? opt(p->active_unit) : std::nullopt,
                        opt(p ? p->terminal_cn : e.terminal_cn),
                        opt(p ? p->client_ip : e.client_ip),
                        e.action,
                        opt(e.resource_ref),
                        opt(e.customer_ref),
                        e.outcome,
                        e.http_status == 0 ? std::nullopt : std::optional<int>(e.http_status),
                        opt(e.reason),
                        opt_json(e.before),
                        opt_json(e.after),
                        opt(e.approval_request_id)});
    t.commit();
}

// ---- login state ---------------------------------------------------------------------------

void IamStore::put_login_state(const std::string& state, const std::string& nonce,
                               const std::string& code_verifier,
                               const std::string& browser_binding, const std::string& terminal_cn) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("DELETE FROM iam.login_state WHERE expires_at < now()");
    t.exec("INSERT INTO iam.login_state(state_hash, nonce, code_verifier, browser_binding_hash, "
           "terminal_cn, expires_at) SELECT $1,$2,$3,$4,$5, now() + make_interval(secs => "
           "(SELECT login_state_ttl_seconds FROM iam.auth_policy))",
           pqxx::params{sha256_hex(state), nonce, code_verifier, sha256_hex(browser_binding),
                        opt(terminal_cn)});
    t.commit();
}

std::optional<LoginState> IamStore::take_login_state(const std::string& state,
                                                     const std::string& browser_binding) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    // Single use: deleted whether or not it then validates.
    const auto r = t.exec("DELETE FROM iam.login_state WHERE state_hash = $1 "
                          "RETURNING nonce, code_verifier, browser_binding_hash, terminal_cn, "
                          "expires_at > now() AS live",
                          pqxx::params{sha256_hex(state)});
    t.commit();
    if (r.empty() || !r[0]["live"].as<bool>() ||
        r[0]["browser_binding_hash"].as<std::string>() != sha256_hex(browser_binding)) {
        return std::nullopt;
    }
    return LoginState{r[0]["nonce"].as<std::string>(), r[0]["code_verifier"].as<std::string>(),
                      str(r[0]["terminal_cn"])};
}

// ---- users & sessions ----------------------------------------------------------------------

std::optional<UserRecord> IamStore::find_user(const std::string& issuer,
                                              const std::string& subject) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    const auto r = t.exec(
        "SELECT u.id, u.username, coalesce(u.display_name, u.username) AS display_name, "
        "u.home_org_unit_id, u.status, u.mfa_required, "
        "(u.last_login_at IS NOT NULL AND u.last_login_at < now() - make_interval(days => "
        " p.dormant_after_days)) AS dormant "
        "FROM iam.operator_user u CROSS JOIN iam.auth_policy p "
        "WHERE u.idp_issuer = $1 AND u.idp_subject = $2",
        pqxx::params{issuer, subject});
    if (r.empty()) return std::nullopt;
    const auto row = r[0];
    return UserRecord{row["id"].as<std::string>(), row["username"].as<std::string>(),
                      row["display_name"].as<std::string>(),
                      row["home_org_unit_id"].as<std::string>(), row["status"].as<std::string>(),
                      row["mfa_required"].as<bool>(), row["dormant"].as<bool>()};
}

void IamStore::mark_dormant(const std::string& user_id) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("UPDATE iam.operator_user SET status = 'DORMANT', locked_reason = "
           "'no login within auth_policy.dormant_after_days' WHERE id = $1 AND status = 'ACTIVE'",
           pqxx::params{user_id});
    t.exec("UPDATE iam.operator_session SET ended_at = now(), end_reason = 'USER_DISABLED' "
           "WHERE user_id = $1 AND ended_at IS NULL",
           pqxx::params{user_id});
    t.commit();
}

std::string IamStore::create_session(const UserRecord& user, const std::string& auth_method,
                                     const std::string& idp_session_id, const std::string& acr,
                                     const std::vector<std::string>& amr,
                                     const std::string& terminal_cn, const std::string& client_ip,
                                     const std::string& user_agent) {
    const std::string token = random_token(32);
    std::string amr_csv;
    for (const auto& a : amr) {
        if (a.find(',') != std::string::npos) continue; // not a token; not stored
        amr_csv += (amr_csv.empty() ? "" : ",") + a;
    }
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("INSERT INTO iam.operator_session(id, user_id, active_org_unit_id, auth_method, "
           "idp_session_id, acr, amr, terminal_cn, client_ip, user_agent, expires_at) "
           "SELECT $1,$2,$3,$4,$5,$6, string_to_array($7, ','), $8,$9,$10, now() + "
           "make_interval(mins => (SELECT session_absolute_minutes FROM iam.auth_policy))",
           pqxx::params{sha256_hex(token), user.id, user.home_unit, auth_method,
                        opt(idp_session_id), opt(acr), amr_csv, opt(terminal_cn), opt(client_ip),
                        opt(user_agent.substr(0, 256))});
    t.exec("UPDATE iam.operator_user SET last_login_at = now() WHERE id = $1",
           pqxx::params{user.id});
    t.commit();
    return token;
}

std::optional<Principal> IamStore::resolve_session(const std::string& cookie_value,
                                                   const std::string& terminal_cn,
                                                   const std::string& client_ip,
                                                   std::string& why) {
    why.clear();
    const std::string sid = sha256_hex(cookie_value);
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    const auto r = t.exec(
        "SELECT s.user_id, u.username, coalesce(u.display_name, u.username) AS display_name, "
        "s.active_org_unit_id, coalesce(s.terminal_cn, '') AS terminal_cn, s.ended_at IS NOT NULL "
        "AS ended, s.expires_at <= now() AS expired, "
        "s.last_seen_at < now() - make_interval(mins => p.session_idle_minutes) AS idle, "
        "u.status FROM iam.operator_session s JOIN iam.operator_user u ON u.id = s.user_id "
        "CROSS JOIN iam.auth_policy p WHERE s.id = $1",
        pqxx::params{sid});
    if (r.empty()) {
        why = "no such session";
        return std::nullopt;
    }
    const auto row = r[0];
    std::string end;
    if (row["ended"].as<bool>()) {
        why = "session ended";
    } else if (row["expired"].as<bool>()) {
        why = "session expired";
        end = "EXPIRED";
    } else if (row["idle"].as<bool>()) {
        why = "session idle timeout";
        end = "IDLE";
    } else if (row["status"].as<std::string>() != "ACTIVE") {
        why = "user is " + row["status"].as<std::string>();
        end = "USER_DISABLED";
    } else if (row["terminal_cn"].as<std::string>() != terminal_cn) {
        // A session is bound to the terminal certificate it was opened on: a stolen cookie
        // replayed from another machine does not work.
        why = "session presented from a different terminal";
        end = "REVOKED";
    }
    if (!why.empty()) {
        if (!end.empty()) {
            t.exec("UPDATE iam.operator_session SET ended_at = now(), end_reason = $2 "
                   "WHERE id = $1 AND ended_at IS NULL",
                   pqxx::params{sid, end});
        }
        t.commit();
        return std::nullopt;
    }
    t.exec("UPDATE iam.operator_session SET last_seen_at = now() WHERE id = $1",
           pqxx::params{sid});
    t.commit();
    return Principal{row["user_id"].as<std::string>(),  row["username"].as<std::string>(),
                     row["display_name"].as<std::string>(), sid,
                     row["active_org_unit_id"].as<std::string>(), terminal_cn, client_ip};
}

void IamStore::end_session(const std::string& session_id, const std::string& reason) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("UPDATE iam.operator_session SET ended_at = now(), end_reason = $2 "
           "WHERE id = $1 AND ended_at IS NULL",
           pqxx::params{session_id, reason});
    t.commit();
}

// ---- authorization -------------------------------------------------------------------------

namespace {
constexpr const char* kLiveGrants =
    "FROM iam.role_assignment ra "
    "JOIN iam.role_permission rp ON rp.role_id = ra.role_id "
    "JOIN iam.operator_user u ON u.id = ra.user_id AND u.status = 'ACTIVE' "
    "WHERE ra.user_id = $1 AND ra.revoked_at IS NULL AND ra.valid_from <= now() "
    "AND (ra.valid_to IS NULL OR ra.valid_to > now()) ";
}

bool IamStore::allowed(const std::string& user_id, const std::string& permission,
                       const std::string& org_unit) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    return t
        .exec(std::string("SELECT EXISTS (SELECT 1 ") + kLiveGrants +
                  "AND rp.permission_id = $2 AND (rp.scope = 'GLOBAL' OR EXISTS (SELECT 1 FROM "
                  "iam.org_unit_closure c WHERE c.ancestor_id = ra.org_unit_id AND "
                  "c.descendant_id = $3)))",
              pqxx::params{user_id, permission, org_unit})
        .one_field()
        .as<bool>();
}

bool IamStore::allowed_anywhere(const std::string& user_id, const std::string& permission) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    return t
        .exec(std::string("SELECT EXISTS (SELECT 1 ") + kLiveGrants + "AND rp.permission_id = $2)",
              pqxx::params{user_id, permission})
        .one_field()
        .as<bool>();
}

std::vector<std::string> IamStore::permissions(const std::string& user_id) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    std::vector<std::string> out;
    for (const auto& row :
         t.exec(std::string("SELECT DISTINCT rp.permission_id ") + kLiveGrants + "ORDER BY 1",
                pqxx::params{user_id})) {
        out.push_back(row[0].as<std::string>());
    }
    return out;
}

std::vector<FieldRule> IamStore::field_rules(const std::string& resource) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    std::vector<FieldRule> out;
    for (const auto& row : t.exec("SELECT field_path, classification, "
                                  "coalesce(unmask_permission, '') FROM iam.field_policy "
                                  "WHERE resource = $1 ORDER BY field_path",
                                  pqxx::params{resource})) {
        out.push_back({row[0].as<std::string>(), row[1].as<std::string>(),
                       row[2].as<std::string>()});
    }
    return out;
}

std::optional<std::string> IamStore::approver_permission(const std::string& permission) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    const auto r = t.exec("SELECT approver_permission FROM iam.approval_policy "
                          "WHERE permission_id = $1",
                          pqxx::params{permission});
    if (r.empty()) return std::nullopt;
    return r[0][0].as<std::string>();
}

// ---- scope registry ------------------------------------------------------------------------

IamStore::Claim IamStore::claim_customer(const std::string& supi, const std::string& unit,
                                         const std::string& user_id) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    // Insert-or-read in one statement: concurrent claims from two shops serialise on the PK.
    const auto ins = t.exec("INSERT INTO iam.customer_ownership(supi, org_unit_id, claimed_by) "
                            "VALUES ($1,$2,$3) ON CONFLICT (supi) DO NOTHING RETURNING supi",
                            pqxx::params{supi, unit, user_id});
    Claim c = Claim::Claimed;
    if (ins.empty()) {
        const auto owner = t.exec("SELECT org_unit_id FROM iam.customer_ownership WHERE supi = $1",
                                  pqxx::params{supi})
                               .one_field()
                               .as<std::string>();
        c = owner == unit ? Claim::AlreadyOurs : Claim::OtherUnit;
    }
    t.commit();
    return c;
}

void IamStore::release_customer(const std::string& supi, const std::string& unit) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("DELETE FROM iam.customer_ownership WHERE supi = $1 AND org_unit_id = $2 AND NOT "
           "EXISTS (SELECT 1 FROM iam.order_ownership o WHERE o.supi = $1)",
           pqxx::params{supi, unit});
    t.commit();
}

void IamStore::record_order(const std::string& order_id, const std::string& supi,
                            const std::string& unit, const std::string& user_id) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("INSERT INTO iam.order_ownership(order_id, supi, org_unit_id, created_by) "
           "VALUES ($1,$2,$3,$4) ON CONFLICT (order_id) DO NOTHING",
           pqxx::params{order_id, supi, unit, user_id});
    t.commit();
}

std::optional<std::pair<std::string, std::string>>
IamStore::order_owner(const std::string& order_id) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    const auto r = t.exec("SELECT org_unit_id, supi FROM iam.order_ownership WHERE order_id = $1",
                          pqxx::params{order_id});
    if (r.empty()) return std::nullopt;
    return std::make_pair(r[0][0].as<std::string>(), r[0][1].as<std::string>());
}

// ---- maker-checker -------------------------------------------------------------------------

std::string IamStore::create_approval(const std::string& permission, const std::string& unit,
                                      const std::string& target_ref, const json& payload,
                                      const std::string& reason,
                                      const std::string& requested_by) {
    const std::string id = "apr-" + random_token(12);
    const std::string body = payload.dump();
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("INSERT INTO iam.approval_request(id, permission_id, org_unit_id, target_ref, payload, "
           "payload_sha256, requested_by, reason, expires_at) SELECT $1,$2,$3,$4,$5::jsonb,$6,$7,$8, "
           "now() + make_interval(hours => (SELECT approval_ttl_hours FROM iam.auth_policy))",
           pqxx::params{id, permission, unit, opt(target_ref), body, sha256_hex(body),
                        requested_by, reason});
    t.commit();
    return id;
}

namespace {
constexpr const char* kApprovalCols =
    "a.id, a.permission_id, a.org_unit_id, coalesce(a.target_ref, '') AS target_ref, "
    "a.requested_by, u.username AS requested_by_name, a.reason, a.status, "
    "coalesce(a.decided_by, '') AS decided_by, coalesce(a.decision_reason, '') AS "
    "decision_reason, a.payload::text AS payload, a.execution_result::text AS execution_result, ";

ApprovalRequest approval_from(const pqxx::row_ref& r) {
    ApprovalRequest a;
    a.id = r["id"].as<std::string>();
    a.permission_id = r["permission_id"].as<std::string>();
    a.org_unit_id = r["org_unit_id"].as<std::string>();
    a.target_ref = r["target_ref"].as<std::string>();
    a.requested_by = r["requested_by"].as<std::string>();
    a.requested_by_name = r["requested_by_name"].as<std::string>();
    a.requested_at = r["requested_at"].as<std::string>();
    a.expires_at = r["expires_at"].as<std::string>();
    a.reason = r["reason"].as<std::string>();
    a.status = r["status"].as<std::string>();
    a.decided_by = r["decided_by"].as<std::string>();
    a.decision_reason = r["decision_reason"].as<std::string>();
    a.payload = json::parse(r["payload"].as<std::string>());
    if (!r["execution_result"].is_null()) {
        a.execution_result.emplace(json::parse(r["execution_result"].as<std::string>()));
    }
    return a;
}
} // namespace

std::optional<ApprovalRequest> IamStore::approval(const std::string& id) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    const auto r = t.exec(std::string("SELECT ") + kApprovalCols + ts("a.requested_at") +
                              " AS requested_at, " + ts("a.expires_at") +
                              " AS expires_at FROM iam.approval_request a JOIN "
                              "iam.operator_user u ON u.id = a.requested_by WHERE a.id = $1",
                          pqxx::params{id});
    if (r.empty()) return std::nullopt;
    return approval_from(r[0]);
}

bool IamStore::decide(const std::string& id, const std::string& decider, bool approve,
                      const std::string& reason) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    const auto r = t.exec("UPDATE iam.approval_request SET status = $3, decided_by = $2, "
                          "decided_at = now(), decision_reason = $4 WHERE id = $1 AND "
                          "status = 'PENDING' AND expires_at > now() AND requested_by <> $2 "
                          "RETURNING id",
                          pqxx::params{id, decider, approve ? "APPROVED" : "REJECTED", reason});
    t.commit();
    return !r.empty();
}

void IamStore::mark_executed(const std::string& id, bool ok, const json& result) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("UPDATE iam.approval_request SET status = $2, executed_at = now(), "
           "execution_result = $3::jsonb WHERE id = $1 AND status = 'APPROVED'",
           pqxx::params{id, ok ? "EXECUTED" : "FAILED", result.dump()});
    t.commit();
}

std::vector<ApprovalRequest> IamStore::approvals(const std::string& status, int limit) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    std::vector<ApprovalRequest> out;
    for (const auto& row :
         t.exec(std::string("SELECT ") + kApprovalCols + ts("a.requested_at") +
                    " AS requested_at, " + ts("a.expires_at") +
                    " AS expires_at FROM iam.approval_request a JOIN iam.operator_user u ON "
                    "u.id = a.requested_by WHERE ($1 = '' OR a.status = $1) "
                    "ORDER BY a.requested_at DESC LIMIT $2",
                pqxx::params{status, limit})) {
        out.push_back(approval_from(row));
    }
    return out;
}

// ---- NF configuration versions -------------------------------------------------------------

int IamStore::add_config_version(const std::string& nf, const json& content,
                                 const std::string& created_by, const std::string& approval_id,
                                 const std::string& comment) {
    const std::string body = content.dump();
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    // Serialise version numbering per NF.
    t.exec("SELECT pg_advisory_xact_lock(hashtext('iam.nf_config_version:' || $1))",
           pqxx::params{nf});
    const int v = t.exec("INSERT INTO iam.nf_config_version(nf_name, version, content, "
                         "content_sha256, created_by, approval_request_id, comment) SELECT $1, "
                         "coalesce(max(version), 0) + 1, $2::jsonb, $3, $4, $5, $6 FROM "
                         "iam.nf_config_version WHERE nf_name = $1 RETURNING version",
                         pqxx::params{nf, body, sha256_hex(body), opt(created_by),
                                      opt(approval_id), opt(comment)})
                      .one_field()
                      .as<int>();
    t.commit();
    return v;
}

void IamStore::mark_config_applied(const std::string& nf, int version) {
    auto l = pool_.acquire();
    pqxx::work t(l.conn());
    t.exec("UPDATE iam.nf_config_version SET applied_at = now() WHERE nf_name = $1 AND "
           "version = $2",
           pqxx::params{nf, version});
    t.commit();
}

std::vector<ConfigVersion> IamStore::config_versions(const std::string& nf) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    std::vector<ConfigVersion> out;
    for (const auto& r : t.exec(
             "SELECT v.version, coalesce(u.username, '(imported from file)') AS created_by, " +
                 ts("v.created_at") + " AS created_at, coalesce(" + ts("v.applied_at") +
                 ", '') AS applied_at, coalesce(v.comment, '') AS comment, v.content_sha256, "
                 "v.restart_required FROM iam.nf_config_version v LEFT JOIN iam.operator_user u "
                 "ON u.id = v.created_by WHERE v.nf_name = $1 ORDER BY v.version DESC",
             pqxx::params{nf})) {
        out.push_back({r["version"].as<int>(), r["created_by"].as<std::string>(),
                       r["created_at"].as<std::string>(), r["applied_at"].as<std::string>(),
                       r["comment"].as<std::string>(), r["content_sha256"].as<std::string>(),
                       r["restart_required"].as<bool>()});
    }
    return out;
}

std::optional<json> IamStore::config_version(const std::string& nf, int version) {
    auto l = pool_.acquire();
    pqxx::nontransaction t(l.conn());
    const auto r = t.exec("SELECT content::text FROM iam.nf_config_version WHERE nf_name = $1 "
                          "AND version = $2",
                          pqxx::params{nf, version});
    if (r.empty()) return std::nullopt;
    return std::optional<json>(json::parse(r[0][0].as<std::string>()));
}

} // namespace oam_gui_bff
