// oam-gui-bff route table (ADR-0422..0425). Every /api handler follows the same order:
//   resolve session -> CSRF (writes) -> authorize (permission + scope) -> AUDIT the decision ->
//   act -> audit the result.
// The decision is audited BEFORE anything is forwarded: if that audit write fails the action is
// refused (503), so no change can happen without a prior audit row. Bodies that may carry SIM
// keys are parsed (authorization needs the SUPI) but never logged; secrets are stripped by field
// policy before anything is stored or audited, and handler exceptions are logged generically
// because pqxx / json error texts can quote their input.

#include "bff.hpp"
#include "redact.hpp"
#include "util.hpp"

#include "sbi_core/json_body.hpp"

#include <spdlog/spdlog.h>

#include <chrono>
#include <functional>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace oam_gui_bff {

namespace {

using nlohmann::json;
using sbi_core::http2::ClientRequest;
using sbi_core::http2::Request;
using sbi_core::http2::Response;

// ---- response helpers ------------------------------------------------------------------------

void harden(Response& r) {
    r.headers.emplace("cache-control", "no-store");
    r.headers.emplace("x-content-type-options", "nosniff");
    r.headers.emplace("referrer-policy", "no-referrer");
}

Response problem(int status, const std::string& title, const std::string& detail) {
    return sbi_core::http2::problem_response(status, title, detail);
}

Response json_resp(int status, const json& body) {
    return Response::json(status, body.dump());
}

Response html(int status, const std::string& body) {
    Response r;
    r.status = status;
    r.headers.emplace("content-type", "text/html; charset=utf-8");
    r.headers.emplace("content-security-policy", "default-src 'none'; frame-ancestors 'none'");
    r.body = body;
    return r;
}

std::string session_cookie(const std::string& value, bool clear = false) {
    return std::string(kSessionCookie) + "=" + (clear ? "" : value) +
           "; Path=/; Secure; HttpOnly; SameSite=Strict" + (clear ? "; Max-Age=0" : "");
}
std::string login_cookie(const std::string& value, bool clear = false) {
    // Lax: it must survive the top-level redirect back from the IdP.
    return std::string(kLoginCookie) + "=" + (clear ? "" : value) +
           "; Path=/; Secure; HttpOnly; SameSite=Lax; Max-Age=" + (clear ? "0" : "300");
}

void access_log(const Request& req, const std::string& user, const char* route, int status,
                std::chrono::steady_clock::time_point start) {
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start)
                        .count();
    // Never a body, a header value, a query string or a cookie.
    spdlog::info("oam-gui-bff: terminal='{}' user='{}' {} {} -> {} ({} ms)", req.peer_cert_cn,
                 user, req.method, route, status, ms);
}

bool csrf_ok(const Request& req) {
    return header(req, "x-requested-by") == "oam-gui" &&
           header(req, "content-type").rfind("application/json", 0) == 0;
}

std::optional<json> parse_body(const Request& req) {
    try {
        auto j = json::parse(req.body);
        if (!j.is_object()) return std::nullopt;
        return std::optional<json>(std::move(j));
    } catch (const std::exception&) {
        return std::nullopt; // e.what() may quote the input: never surfaced
    }
}

std::string digits(const std::string& s) {
    std::string d;
    for (const char c : s) {
        if (c >= '0' && c <= '9') d += c;
    }
    return d;
}

// ---- the request context every /api handler receives ----------------------------------------

struct Ctx {
    const Request& req;
    Principal who;
    Deps& d;

    void audit(const std::string& action, const std::string& outcome, int status,
               const std::string& reason = {}, const std::string& resource = {},
               const std::string& customer = {}, json before = nullptr, json after = nullptr,
               const std::string& approval = {}) {
        AuditEvent e;
        e.who = &who;
        e.action = action;
        e.outcome = outcome;
        e.http_status = status;
        e.reason = reason;
        e.resource_ref = resource;
        e.customer_ref = customer;
        if (!before.is_null()) e.before = std::move(before);
        if (!after.is_null()) e.after = std::move(after);
        e.approval_request_id = approval;
        d.iam.audit(e); // throws -> the wrapper answers 503 and nothing further happens
    }
    Response deny(const std::string& action, int status, const std::string& reason,
                  const std::string& resource = {}, const std::string& customer = {}) {
        audit(action, "DENIED", status, reason, resource, customer);
        if (status == 404) return problem(404, "Not Found", "no such resource");
        if (status == 400) return problem(400, "Bad Request", reason);
        if (status == 409) return problem(409, "Conflict", reason);
        return problem(status, status == 401 ? "Unauthorized" : "Forbidden",
                       status == 401 ? "sign in required" : "not permitted");
    }
    bool can(const std::string& permission, const std::string& unit) {
        return d.iam.allowed(who.user_id, permission, unit);
    }

    // Forwards to a BSS service with the BFF's own identity. The body is sent as given.
    std::optional<sbi_core::http2::ClientResponse> upstream(const std::string& method,
                                                            const std::string& url,
                                                            const std::string& body = {}) {
        ClientRequest out;
        out.method = method;
        out.url = url;
        out.body = body;
        out.headers.emplace("accept", "application/json");
        if (!body.empty()) out.headers.emplace("content-type", "application/json");
        auto r = d.services.send(out);
        if (!r.has_value()) {
            spdlog::warn("oam-gui-bff: upstream unreachable: {}", r.error());
            return std::nullopt;
        }
        return *r;
    }
};

using ApiHandler = std::function<Response(Ctx&)>;

// Wraps an /api handler: session, CSRF, exception boundary, hardening, access log.
auto api(Deps& d, const char* route, const char* action, ApiHandler h) {
    return [&d, route, action, h = std::move(h)](const Request& req) -> Response {
        const auto start = std::chrono::steady_clock::now();
        std::string user;
        Response r;
        try {
            std::string why = "no session cookie";
            std::optional<Principal> who;
            if (const auto c = cookie(req, kSessionCookie); c && !c->empty()) {
                why.clear();
                who = d.iam.resolve_session(*c, req.peer_cert_cn, req.peer_address, why);
            }
            if (!who) {
                AuditEvent e;
                e.terminal_cn = req.peer_cert_cn;
                e.client_ip = req.peer_address;
                e.action = action;
                e.outcome = "DENIED";
                e.http_status = 401;
                e.reason = why;
                d.iam.audit(e);
                r = problem(401, "Unauthorized", "sign in required");
            } else {
                user = who->username;
                Ctx ctx{req, *who, d};
                if (req.method != "GET" && !csrf_ok(req)) {
                    r = ctx.deny(action, 403, "CSRF: missing x-requested-by or non-JSON body");
                } else {
                    r = h(ctx);
                }
            }
        } catch (const std::exception&) {
            // Generic on purpose: the exception text may quote a body, a key or a DB value.
            spdlog::error("oam-gui-bff: {} {} failed internally (details withheld)", req.method,
                          route);
            r = problem(503, "Service Unavailable",
                        "the request could not be completed and was not audited as done");
        }
        harden(r);
        access_log(req, user, route, r.status, start);
        return r;
    };
}

// ---- maker-checker execution -------------------------------------------------------------------

struct Collection {
    const char* path;       // /api/tmf620/productOffering
    const char* upstream;   // /productOffering
    const char* read_perm;  // product_offering:read
    const char* create_perm;
};
const Collection kCatalog[] = {
    {"/api/tmf620/productOffering", "/productOffering", "product_offering:read",
     "product_offering:create"},
    {"/api/tmf620/productOfferingPrice", "/productOfferingPrice", "product_offering_price:read",
     "product_offering_price:create"},
};

// Executes an APPROVED request. Returns (ok, result for the record).
std::pair<bool, json> execute(Ctx& c, const ApprovalRequest& a) {
    for (const auto& col : kCatalog) {
        if (a.permission_id == col.create_perm) {
            const auto r = c.upstream("POST",
                                      c.d.config.product_catalog_base_url + kTmf620Root +
                                          col.upstream,
                                      a.payload.dump());
            if (!r) return {false, json{{"error", "catalog service unreachable"}}};
            json body;
            try {
                body = json::parse(r->body);
            } catch (const std::exception&) {
                body = nullptr;
            }
            return {r->status == 201, json{{"httpStatus", r->status}, {"response", body}}};
        }
    }
    if (a.permission_id == "nf_config:change") {
        const std::string nf = a.payload.at("nf").get<std::string>();
        const json& content = a.payload.at("content");
        const auto schema = c.d.configs.schema(nf);
        if (!c.d.configs.editable(nf) || !schema) return {false, json{{"error", "not editable"}}};
        const auto errs = NfConfigManager::validate(*schema, content);
        if (!errs.empty()) return {false, json{{"validationErrors", errs}}};
        const int v = c.d.iam.add_config_version(nf, content, a.requested_by, a.id,
                                                 a.payload.value("comment", ""));
        c.d.configs.apply(nf, content);
        c.d.iam.mark_config_applied(nf, v);
        return {true, json{{"nf", nf}, {"version", v}, {"restartRequired", true}}};
    }
    return {false, json{{"error", "no executor for " + a.permission_id}}};
}

json approval_json(Deps& d, const ApprovalRequest& a) {
    json payload = a.payload;
    if (a.permission_id == "nf_config:change" && payload.contains("nf")) {
        if (const auto s = d.configs.schema(payload["nf"].get<std::string>())) {
            payload["content"] = NfConfigManager::mask(*s, payload["content"]);
        }
    }
    json out{{"id", a.id},           {"permission", a.permission_id}, {"orgUnit", a.org_unit_id},
             {"target", a.target_ref}, {"requestedBy", a.requested_by_name},
             {"requestedAt", a.requested_at}, {"expiresAt", a.expires_at}, {"reason", a.reason},
             {"status", a.status},   {"payload", payload}};
    if (!a.decision_reason.empty()) out["decisionReason"] = a.decision_reason;
    if (a.execution_result) out["executionResult"] = *a.execution_result;
    return out;
}

} // namespace

void register_routes(sbi_core::http2::Server& server, Deps& d, StaticFiles static_files) {
    // ============================== authentication ==============================================

    server.add_route("GET", "/auth/login", [&d](const Request& req) {
        const auto start = std::chrono::steady_clock::now();
        Response r;
        try {
            const std::string binding = random_token(32);
            r.status = 302;
            r.headers.emplace("location", d.auth.begin(binding, req.peer_cert_cn));
            r.headers.emplace("set-cookie", login_cookie(binding));
        } catch (const std::exception&) {
            spdlog::error("oam-gui-bff: login start failed (details withheld)");
            r = problem(503, "Service Unavailable", "sign-in is unavailable");
        }
        harden(r);
        access_log(req, "", "/auth/login", r.status, start);
        return r;
    });

    server.add_route("GET", "/auth/callback", [&d](const Request& req) {
        const auto start = std::chrono::steady_clock::now();
        Response r;
        std::string user;
        try {
            AuditEvent e;
            e.terminal_cn = req.peer_cert_cn;
            e.client_ip = req.peer_address;
            e.action = "auth.login";
            LoginOutcome out;
            const auto code = query(req, "code");
            const auto state = query(req, "state");
            const auto binding = cookie(req, kLoginCookie);
            if (query(req, "error")) {
                out.reason = "IdP returned error: " + query(req, "error").value_or("");
            } else if (!code || !state || !binding) {
                out.reason = "callback without code/state or without the login cookie";
            } else {
                out = d.auth.complete(*code, *state, *binding);
            }
            if (!out.ok) {
                Principal p;
                if (out.user) {
                    p.user_id = out.user->id;
                    p.username = out.user->username;
                }
                p.terminal_cn = req.peer_cert_cn;
                p.client_ip = req.peer_address;
                if (out.user) e.who = &p;
                e.outcome = "DENIED";
                e.http_status = 403;
                e.reason = out.reason;
                if (!out.user && !out.subject.empty()) e.resource_ref = "idp-subject/" + out.subject;
                d.iam.audit(e);
                r = html(403, "<!doctype html><title>Sign-in refused</title><p>Sign-in was "
                              "refused. Contact your security administrator if this persists."
                              "</p><p><a href=\"/auth/login\">Try again</a></p>");
            } else {
                const std::string token = d.iam.create_session(
                    *out.user, "oidc", out.idp_session_id, out.acr, out.amr, req.peer_cert_cn,
                    req.peer_address, header(req, "user-agent"));
                user = out.user->username;
                Principal p{out.user->id, out.user->username, out.user->display_name,
                            sha256_hex(token), out.user->home_unit, req.peer_cert_cn,
                            req.peer_address};
                e.who = &p;
                e.outcome = "ALLOWED";
                e.http_status = 200;
                e.after = json{{"acr", out.acr}, {"amr", out.amr}};
                d.iam.audit(e);
                // 200 + meta refresh, not 302: a SameSite=Strict cookie set on a redirect chain
                // that began at the IdP would not be sent on the redirect's own navigation.
                r = html(200, "<!doctype html><meta http-equiv=\"refresh\" content=\"0;url=/\">"
                              "<title>Signed in</title><p><a href=\"/\">Continue</a></p>");
                r.headers.emplace("set-cookie", session_cookie(token));
            }
            r.headers.emplace("set-cookie", login_cookie("", true));
        } catch (const std::exception&) {
            spdlog::error("oam-gui-bff: login completion failed (details withheld)");
            r = problem(503, "Service Unavailable", "sign-in could not be completed");
        }
        harden(r);
        access_log(req, user, "/auth/callback", r.status, start);
        return r;
    });

    server.add_route("POST", "/auth/logout", api(d, "/auth/logout", "auth.logout", [](Ctx& c) {
                         c.d.iam.end_session(c.who.session_id, "LOGOUT");
                         c.audit("auth.logout", "ALLOWED", 200);
                         auto r = json_resp(200, json{{"logoutUrl", c.d.auth.logout_url()}});
                         r.headers.emplace("set-cookie", session_cookie("", true));
                         return r;
                     }));

    server.add_route("GET", "/api/me", api(d, "/api/me", "auth.whoami", [](Ctx& c) {
                         c.audit("auth.whoami", "ALLOWED", 200);
                         return json_resp(200, json{{"userId", c.who.user_id},
                                                    {"username", c.who.username},
                                                    {"displayName", c.who.display_name},
                                                    {"orgUnit", c.who.active_unit},
                                                    {"terminal", c.who.terminal_cn},
                                                    {"permissions",
                                                     c.d.iam.permissions(c.who.user_id)}});
                     }));

    // ============================== TMF620 catalog ==============================================

    for (const auto& col : kCatalog) {
        server.add_route("GET", col.path, api(d, col.path, col.read_perm, [col](Ctx& c) {
                             if (!c.can(col.read_perm, c.who.active_unit)) {
                                 return c.deny(col.read_perm, 403, "missing permission");
                             }
                             c.audit(col.read_perm, "ALLOWED", 200, {}, col.upstream);
                             const auto r = c.upstream("GET", c.d.config.product_catalog_base_url +
                                                                  kTmf620Root + col.upstream);
                             if (!r) return problem(502, "Bad Gateway", "catalog unreachable");
                             return Response::json(static_cast<int>(r->status), r->body);
                         }));
        server.add_route(
            "GET", std::string(col.path) + "/{id}",
            api(d, col.path, col.read_perm, [col](Ctx& c) {
                const auto& id = c.req.path_params.at("id");
                if (!is_safe_id(id)) return c.deny(col.read_perm, 400, "invalid resource id");
                if (!c.can(col.read_perm, c.who.active_unit)) {
                    return c.deny(col.read_perm, 403, "missing permission");
                }
                c.audit(col.read_perm, "ALLOWED", 200, {}, std::string(col.upstream) + "/" + id);
                const auto r = c.upstream("GET", c.d.config.product_catalog_base_url +
                                                     kTmf620Root + col.upstream + "/" + id);
                if (!r) return problem(502, "Bad Gateway", "catalog unreachable");
                return Response::json(static_cast<int>(r->status), r->body);
            }));
        // Create = a maker-checker REQUEST. Nothing reaches the catalog until someone else
        // approves it.
        server.add_route(
            "POST", col.path, api(d, col.path, col.create_perm, [col](Ctx& c) {
                if (!c.can(col.create_perm, c.who.active_unit)) {
                    return c.deny(col.create_perm, 403, "missing permission");
                }
                const auto body = parse_body(c.req);
                if (!body) return c.deny(col.create_perm, 400, "body is not a JSON object");
                // Percent-encoded by the GUI: HTTP header values are Latin-1 only, reasons are not.
                const std::string reason = url_decode(header(c.req, "x-oam-reason"));
                if (reason.empty()) {
                    return c.deny(col.create_perm, 400, "a change reason (x-oam-reason) is required");
                }
                if (!c.d.iam.approver_permission(col.create_perm)) {
                    return c.deny(col.create_perm, 403, "no approval policy for this action");
                }
                const auto id = c.d.iam.create_approval(col.create_perm, c.who.active_unit,
                                                        body->value("name", ""), *body, reason,
                                                        c.who.user_id);
                c.audit(col.create_perm, "PENDING_APPROVAL", 202, reason, col.upstream, {},
                        nullptr, *body, id);
                return json_resp(202, json{{"approvalRequestId", id}, {"status", "PENDING"}});
            }));
    }

    // ============================== maker-checker ===============================================

    server.add_route("GET", "/api/approvals", api(d, "/api/approvals", "approval.list", [](Ctx& c) {
                         const std::string status = query(c.req, "status").value_or("");
                         json out = json::array();
                         for (const auto& a : c.d.iam.approvals(status, 200)) {
                             const auto approver = c.d.iam.approver_permission(a.permission_id);
                             const bool mine = a.requested_by == c.who.user_id;
                             const bool can_decide =
                                 approver && c.can(*approver, a.org_unit_id) && !mine;
                             if (!mine && !can_decide) continue;
                             auto j = approval_json(c.d, a);
                             j["canDecide"] = can_decide && a.status == "PENDING";
                             out.push_back(std::move(j));
                         }
                         c.audit("approval.list", "ALLOWED", 200);
                         return json_resp(200, out);
                     }));

    server.add_route(
        "POST", "/api/approvals/{id}/decision",
        api(d, "/api/approvals/{id}/decision", "approval.decide", [](Ctx& c) {
            const auto& id = c.req.path_params.at("id");
            const auto body = parse_body(c.req);
            if (!is_safe_id(id) || !body) return c.deny("approval.decide", 400, "bad request");
            const std::string decision = body->value("decision", "");
            const std::string reason = body->value("reason", "");
            if ((decision != "approve" && decision != "reject") || reason.empty()) {
                return c.deny("approval.decide", 400, "decision (approve|reject) and reason required");
            }
            const auto a = c.d.iam.approval(id);
            if (!a) return c.deny("approval.decide", 404, "no such request", "approval/" + id);
            const auto approver = c.d.iam.approver_permission(a->permission_id);
            if (!approver || !c.can(*approver, a->org_unit_id)) {
                return c.deny(approver.value_or("approval.decide"), 403,
                              "missing approver permission", "approval/" + id);
            }
            if (a->requested_by == c.who.user_id) {
                return c.deny(*approver, 403, "four-eyes: the maker cannot decide their own request",
                              "approval/" + id);
            }
            if (!c.d.iam.decide(id, c.who.user_id, decision == "approve", reason)) {
                return c.deny(*approver, 409, "request is not pending (or expired)",
                              "approval/" + id);
            }
            c.audit(*approver, decision == "approve" ? "APPROVED" : "REJECTED", 200, reason,
                    "approval/" + id, {}, nullptr, nullptr, id);
            if (decision == "reject") return json_resp(200, json{{"status", "REJECTED"}});
            // Execute under the approver's session; the result lands on the request and in the
            // audit trail.
            const auto [ok, result] = execute(c, *a);
            c.d.iam.mark_executed(id, ok, result);
            c.audit(a->permission_id, ok ? "ALLOWED" : "ERROR", ok ? 200 : 502,
                    "executed after approval", a->target_ref, {}, nullptr, result, id);
            return json_resp(ok ? 200 : 502,
                             json{{"status", ok ? "EXECUTED" : "FAILED"}, {"result", result}});
        }));

    // ============================== customer onboarding =========================================

    server.add_route(
        "POST", "/api/provisioning/customerOrder",
        api(d, "/api/provisioning/customerOrder", "customer_order:create", [](Ctx& c) {
            constexpr const char* kPerm = "customer_order:create";
            if (!c.can(kPerm, c.who.active_unit)) return c.deny(kPerm, 403, "missing permission");
            auto body = parse_body(c.req);
            if (!body) return c.deny(kPerm, 400, "body is not a JSON object");
            const std::string supi = body->value("supi", "");
            if (!is_safe_id(supi)) return c.deny(kPerm, 400, "supi missing or malformed");
            const std::string client_key = body->value("idempotencyKey", "");
            if (!client_key.empty() && !is_safe_id(client_key)) {
                return c.deny(kPerm, 400, "idempotencyKey malformed", {}, supi);
            }
            // Keys are namespaced per org unit, so a key typed in shop B can never resume (and so
            // read) shop A's order. The provisioning order id is "ord-<key>".
            (*body)["idempotencyKey"] =
                c.who.active_unit + "." + (client_key.empty() ? digits(supi) : client_key);
            const auto claim = c.d.iam.claim_customer(supi, c.who.active_unit, c.who.user_id);
            if (claim == IamStore::Claim::OtherUnit) {
                return c.deny(kPerm, 409, "this subscriber is not available to your unit", {},
                              supi);
            }
            const auto rules = c.d.iam.field_rules("customer_order");
            auto secrets = secret_values(*body, rules);
            // Audited BEFORE forwarding: an unaudited onboarding cannot happen.
            c.audit(kPerm, "ALLOWED", 0, "decision", "customerOrder", supi, nullptr,
                    strip_secrets(*body, rules));
            auto r = c.upstream("POST", c.d.config.provisioning_base_url + kProvisioningRoot +
                                            "/customerOrder",
                                body->dump());
            body.reset(); // drop the only parsed copy of the keys now
            if (!r) {
                if (claim == IamStore::Claim::Claimed) c.d.iam.release_customer(supi, c.who.active_unit);
                c.audit(kPerm, "ERROR", 502, "provisioning unreachable", "customerOrder", supi);
                return problem(502, "Bad Gateway", "provisioning unreachable");
            }
            json resp;
            try {
                resp = json::parse(r->body);
            } catch (const std::exception&) {
                resp = json::object();
            }
            const std::string order_id = resp.is_object() ? resp.value("orderId", "") : "";
            if (!order_id.empty()) {
                c.d.iam.record_order(order_id, supi, c.who.active_unit, c.who.user_id);
            } else if (r->status == 400 && claim == IamStore::Claim::Claimed) {
                c.d.iam.release_customer(supi, c.who.active_unit); // rejected: nothing written
            }
            c.audit(kPerm, r->status < 300 ? "ALLOWED" : "ERROR", static_cast<int>(r->status),
                    "result", order_id.empty() ? "customerOrder" : "customerOrder/" + order_id,
                    supi, nullptr, mask_pii(resp, {}, {}, secrets));
            const auto shown = mask_pii(resp, c.d.iam.field_rules("customer_order_response"),
                                        {supi, digits(supi)}, secrets);
            secrets.clear();
            return json_resp(static_cast<int>(r->status), shown);
        }));

    server.add_route(
        "GET", "/api/provisioning/customerOrder/{id}",
        api(d, "/api/provisioning/customerOrder/{id}", "customer_order:read", [](Ctx& c) {
            constexpr const char* kPerm = "customer_order:read";
            const auto& id = c.req.path_params.at("id");
            if (!is_safe_id(id)) return c.deny(kPerm, 400, "invalid order id");
            const auto owner = c.d.iam.order_owner(id);
            // Unknown and out-of-scope look identical (404): no existence oracle across shops.
            if (!owner || !c.can(kPerm, owner->first)) {
                return c.deny(kPerm, 404, owner ? "order outside the caller's scope"
                                                : "order not created through the GUI",
                              "customerOrder/" + id, owner ? owner->second : std::string());
            }
            const std::string unmask_reason = url_decode(header(c.req, "x-oam-unmask-reason"));
            if (!unmask_reason.empty() && !c.can("pii:unmask", owner->first)) {
                return c.deny("pii:unmask", 403, "missing permission", "customerOrder/" + id,
                              owner->second);
            }
            c.audit(kPerm, "ALLOWED", 0, "decision", "customerOrder/" + id, owner->second);
            if (!unmask_reason.empty()) {
                c.audit("pii:unmask", "ALLOWED", 0, unmask_reason, "customerOrder/" + id,
                        owner->second);
            }
            const auto r = c.upstream("GET", c.d.config.provisioning_base_url + kProvisioningRoot +
                                                 "/customerOrder/" + id);
            if (!r) return problem(502, "Bad Gateway", "provisioning unreachable");
            json resp;
            try {
                resp = json::parse(r->body);
            } catch (const std::exception&) {
                resp = json::object();
            }
            if (unmask_reason.empty()) {
                resp = mask_pii(resp, c.d.iam.field_rules("customer_order_response"),
                                {owner->second, digits(owner->second)});
            }
            return json_resp(static_cast<int>(r->status), resp);
        }));

    // ============================== NF configuration ============================================

    server.add_route("GET", "/api/config", api(d, "/api/config", "nf_config:read", [](Ctx& c) {
                         if (!c.d.iam.allowed_anywhere(c.who.user_id, "nf_config:read")) {
                             return c.deny("nf_config:read", 403, "missing permission");
                         }
                         c.audit("nf_config:read", "ALLOWED", 200, {}, "config");
                         json out = json::array();
                         for (const auto& nf : c.d.configs.editable_nfs()) out.push_back(nf);
                         return json_resp(200, json{{"editable", out}});
                     }));

    server.add_route(
        "GET", "/api/config/{nf}", api(d, "/api/config/{nf}", "nf_config:read", [](Ctx& c) {
            const auto& nf = c.req.path_params.at("nf");
            if (!c.d.iam.allowed_anywhere(c.who.user_id, "nf_config:read")) {
                return c.deny("nf_config:read", 403, "missing permission", "config/" + nf);
            }
            const auto schema = c.d.configs.schema(nf);
            const auto current = c.d.configs.current(nf);
            if (!schema || !current) return c.deny("nf_config:read", 404, "unknown component");
            auto versions = c.d.iam.config_versions(nf);
            if (versions.empty()) {
                // Baseline: the file on disk becomes version 1 (created_by NULL = imported).
                const int v = c.d.iam.add_config_version(nf, *current, "", "", "imported from file");
                c.d.iam.mark_config_applied(nf, v);
                versions = c.d.iam.config_versions(nf);
            }
            c.audit("nf_config:read", "ALLOWED", 200, {}, "config/" + nf);
            json vs = json::array();
            for (const auto& v : versions) {
                vs.push_back({{"version", v.version}, {"createdBy", v.created_by},
                              {"createdAt", v.created_at}, {"appliedAt", v.applied_at},
                              {"comment", v.comment}, {"sha256", v.content_sha256},
                              {"restartRequired", v.restart_required}});
            }
            return json_resp(200, json{{"nf", nf},
                                       {"editable", c.d.configs.editable(nf)},
                                       {"apply", schema->value("x-apply", "restart")},
                                       {"content", NfConfigManager::mask(*schema, *current)},
                                       {"versions", vs}});
        }));

    // Change or rollback: both become a maker-checker request carrying the full new content.
    const auto propose = [](Ctx& c, const std::string& nf, json proposed, const std::string& reason,
                            const std::string& comment) -> Response {
        constexpr const char* kPerm = "nf_config:change";
        if (!c.d.iam.allowed_anywhere(c.who.user_id, kPerm)) {
            return c.deny(kPerm, 403, "missing permission", "config/" + nf);
        }
        if (!c.d.configs.editable(nf)) {
            return c.deny(kPerm, 403, "component not enabled for GUI configuration", "config/" + nf);
        }
        const auto schema = c.d.configs.schema(nf);
        const auto current = c.d.configs.current(nf);
        if (!schema || !current) return c.deny(kPerm, 404, "unknown component", "config/" + nf);
        if (reason.empty()) return c.deny(kPerm, 400, "a change reason is required", "config/" + nf);
        proposed = NfConfigManager::unmask_unchanged(*schema, std::move(proposed), *current);
        const auto errs = NfConfigManager::validate(*schema, proposed);
        if (!errs.empty()) {
            c.audit(kPerm, "DENIED", 400, "schema validation failed", "config/" + nf);
            return json_resp(400, json{{"title", "Bad Request"}, {"status", 400},
                                       {"validationErrors", errs}});
        }
        if (proposed == *current) return c.deny(kPerm, 409, "no change", "config/" + nf);
        const auto id = c.d.iam.create_approval(kPerm, c.who.active_unit, nf,
                                                json{{"nf", nf}, {"content", proposed},
                                                     {"comment", comment}},
                                                reason, c.who.user_id);
        c.audit(kPerm, "PENDING_APPROVAL", 202, reason, "config/" + nf, {},
                NfConfigManager::mask(*schema, *current), NfConfigManager::mask(*schema, proposed),
                id);
        return json_resp(202, json{{"approvalRequestId", id}, {"status", "PENDING"}});
    };

    server.add_route("POST", "/api/config/{nf}",
                     api(d, "/api/config/{nf}", "nf_config:change", [propose](Ctx& c) {
                         const auto body = parse_body(c.req);
                         if (!body || !body->contains("content") || !(*body)["content"].is_object()) {
                             return c.deny("nf_config:change", 400, "content object required");
                         }
                         return propose(c, c.req.path_params.at("nf"), (*body)["content"],
                                        body->value("reason", ""), body->value("comment", ""));
                     }));

    server.add_route("POST", "/api/config/{nf}/rollback",
                     api(d, "/api/config/{nf}/rollback", "nf_config:change", [propose](Ctx& c) {
                         const auto& nf = c.req.path_params.at("nf");
                         const auto body = parse_body(c.req);
                         if (!body || !(*body)["version"].is_number_integer()) {
                             return c.deny("nf_config:change", 400, "version required");
                         }
                         const int v = (*body)["version"].get<int>();
                         const auto content = c.d.iam.config_version(nf, v);
                         if (!content) return c.deny("nf_config:change", 404, "no such version");
                         return propose(c, nf, *content, body->value("reason", ""),
                                        "rollback to version " + std::to_string(v));
                     }));

    // ============================== static web app ==============================================

    auto files = std::make_shared<const StaticFiles>(std::move(static_files));
    const auto serve = [files](const std::string& key, const Request& req) {
        const auto start = std::chrono::steady_clock::now();
        Response r;
        const auto it = files->find(key);
        if (it == files->end()) {
            r = problem(404, "Not Found", "no such file");
        } else {
            r.status = 200;
            r.headers.emplace("content-type", it->second.content_type);
            r.body = it->second.body;
            if (key == "index.html") {
                r.headers.emplace("content-security-policy",
                                  "default-src 'self'; script-src 'self'; style-src 'self'; "
                                  "img-src 'self' data:; connect-src 'self'; object-src 'none'; "
                                  "base-uri 'none'; form-action 'self'; frame-ancestors 'none'");
                r.headers.emplace("x-frame-options", "DENY");
            }
        }
        harden(r);
        access_log(req, "", "static", r.status, start);
        return r;
    };
    server.add_route("GET", "/", [serve](const Request& req) { return serve("index.html", req); });
    server.add_route("GET", "/index.html",
                     [serve](const Request& req) { return serve("index.html", req); });
    server.add_route("GET", "/assets/{file}", [serve](const Request& req) {
        const auto& f = req.path_params.at("file");
        return serve(is_safe_id(f) ? "assets/" + f : std::string(), req);
    });
}

} // namespace oam_gui_bff
