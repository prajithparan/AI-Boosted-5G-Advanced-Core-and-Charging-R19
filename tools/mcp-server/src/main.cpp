// ADR-0331: a read-only MCP tool server over this project's charging and NF state.
//
// `CLAUDE.md` proposed this ("an MCP server exposing read-only NF state and analytics as tools;
// read-only by default, any write/config action requires explicit human approval") and it was
// never built. This is that server, and it is the shared half of the agent roadmap in
// docs/AI_AGENTS_AND_MODELS.md: multiple agents are one server plus per-agent tool scoping, not N
// separate integrations.
//
// Transport is JSON-RPC 2.0 over stdio, which is MCP's own default and needs no port, no TLS
// termination and no auth layer of its own -- the process boundary is the trust boundary, and the
// caller is whoever launched it.
//
// Two properties are structural rather than procedural:
//
//   1. THERE IS NO WRITE PATH. Not "writes are gated" -- there is no tool that mutates anything.
//      That is the honest way to satisfy the human-approval rule in v1: nothing to approve.
//   2. A PII READ THAT CANNOT BE AUDITED DOES NOT HAPPEN. The audit row is committed BEFORE the
//      backend is called, and a failed audit write aborts the call. An unlogged PII access is
//      impossible, not merely discouraged.
//
// Motto check ("Built by AI. Built for AI. Bound by the spec."): every tool answers from a stored
// record. None synthesises a value. A tool that cannot ground its answer returns an error, so an
// agent above it has nothing to hallucinate from.

#include "sbi_core/http2_client.hpp"

#include <spdlog/sinks/stdout_sinks.h>
#include <spdlog/spdlog.h>

#include <iostream>
#include <string>

#include "agent_scope.hpp"
#include "cdr.hpp"
#include "rating_decision_store.hpp"

#include "nf_config/nf_config.hpp"
#include "pii_audit.hpp"
#include "tools.hpp"

namespace {

using nlohmann::json;

constexpr const char* kProtocolVersion = "2024-11-05";
constexpr const char* kServerName = "5gc-charging-mcp";

json rpc_error(const json& id, int code, const std::string& message) {
    return json{
        {"jsonrpc", "2.0"}, {"id", id}, {"error", json{{"code", code}, {"message", message}}}};
}

json rpc_result(const json& id, json result) {
    return json{{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}};
}

// MCP reports tool failures as a result with isError, not a JSON-RPC error: the call itself
// succeeded, the tool just could not answer. Keeping that distinction lets an agent tell "the
// server is broken" from "there is no such subscriber".
json tool_failure(const json& id, const std::string& message) {
    return rpc_result(id,
                      json{{"isError", true},
                           {"content", json::array({json{{"type", "text"}, {"text", message}}})}});
}

json tool_success(const json& id, const json& payload) {
    return rpc_result(
        id,
        json{{"isError", false},
             {"content", json::array({json{{"type", "text"}, {"text", payload.dump(2)}}})}});
}

} // namespace

int main() {
    // stderr, never stdout: stdout IS the JSON-RPC channel and a stray log line there corrupts
    // the protocol stream.
    spdlog::set_default_logger(spdlog::stderr_logger_mt("mcp"));

    json config;
    try {
        config = nf_config::load("mcp-server", CONFIG_DIR);
    } catch (const std::exception& e) {
        spdlog::error("mcp: cannot load config: {}", e.what());
        return 1;
    }

    const auto audit_conninfo = nf_config::require<std::string>(
        config, "pii_audit_database_url", "MCP_PII_AUDIT_DATABASE_URL");
    mcp::PiiAuditStore audit(audit_conninfo);
    if (!audit.is_connected()) {
        // Refusing to start is the point. A server that comes up able to serve PII with nowhere to
        // log it is exactly the window the audit requirement exists to close.
        spdlog::error("mcp: PII audit store unavailable -- refusing to start, because a server "
                      "that can read subscriber data without logging it is the failure this "
                      "audit exists to prevent");
        return 1;
    }

    const auto scopes = mcp::AgentScopeRegistry::from_config(config);
    const auto tools = mcp::build_tool_registry();

    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client client(std::move(tls));

    // ADR-0332: read-only access to the two project-owned stores that have no specified API.
    // Constructed here, once, so a per-call connection cost is not paid on every question an
    // agent asks.
    chf::RatingDecisionStore rating_decisions(nf_config::require<std::string>(
        config, "rating_database_url", "MCP_RATING_DATABASE_URL"));
    chf::DorisOptions doris{
        .host = nf_config::require<std::string>(config, "cdr_host", "MCP_CDR_HOST"),
        .port = static_cast<std::uint16_t>(
            nf_config::require<int>(config, "cdr_port", "MCP_CDR_PORT")),
        .user = nf_config::require<std::string>(config, "cdr_user", "MCP_CDR_USER"),
        .password = nf_config::require<std::string>(config, "cdr_password", "MCP_CDR_PASSWORD"),
        .database = nf_config::require<std::string>(config, "cdr_database", "MCP_CDR_DATABASE"),
    };
    chf::CdrWriter cdrs(doris);
    if (!rating_decisions.is_connected()) {
        // Not fatal, unlike the audit store: explain_charge degrades to a clear "no decision
        // recorded" error, which is a true statement. The audit is different because its absence
        // would let PII flow unlogged.
        spdlog::warn("mcp: rating-decision store unavailable -- explain_charge will report that "
                     "no decision could be read rather than inventing one");
    }

    mcp::ToolContext ctx{.client = &client,
                         .rating_decisions = &rating_decisions,
                         .cdrs = &cdrs,
                         .config = config};

    spdlog::info("mcp: ready, {} read-only tools, audit connected", tools.size());

    std::string line;
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            continue;
        }
        json req;
        try {
            req = json::parse(line);
        } catch (const std::exception& e) {
            std::cout << rpc_error(nullptr, -32700, std::string("parse error: ") + e.what()).dump()
                      << std::endl;
            continue;
        }

        const json id = req.contains("id") ? req.at("id") : json(nullptr);
        const std::string method = req.contains("method") && req.at("method").is_string()
                                       ? req.at("method").get<std::string>()
                                       : std::string{};

        if (method == "initialize") {
            std::cout << rpc_result(id,
                                    json{{"protocolVersion", kProtocolVersion},
                                         {"capabilities", json{{"tools", json::object()}}},
                                         {"serverInfo",
                                          json{{"name", kServerName}, {"version", "1.0.0"}}}})
                             .dump()
                      << std::endl;
            continue;
        }

        // The caller identifies its agent; the server decides what that agent may do. `_meta` is
        // MCP's own extension point for exactly this kind of out-of-band context.
        const json meta = req.contains("params") && req.at("params").contains("_meta")
                              ? req.at("params").at("_meta")
                              : json::object();
        const std::string agent_id = meta.contains("agentId") && meta.at("agentId").is_string()
                                         ? meta.at("agentId").get<std::string>()
                                         : std::string{};
        const std::string principal = meta.contains("principal") && meta.at("principal").is_string()
                                          ? meta.at("principal").get<std::string>()
                                          : std::string{};

        if (method == "tools/list") {
            // Each agent is shown only the tools it may call. Listing tools an agent cannot use
            // invites it to try, and every attempt is then a denial in the audit log that means
            // nothing.
            json arr = json::array();
            for (const auto& t : tools) {
                if (!agent_id.empty() && !scopes.may_call(agent_id, t.name)) {
                    continue;
                }
                arr.push_back(json{{"name", t.name},
                                   {"description", t.description},
                                   {"inputSchema", t.input_schema}});
            }
            std::cout << rpc_result(id, json{{"tools", arr}}).dump() << std::endl;
            continue;
        }

        if (method != "tools/call") {
            std::cout << rpc_error(id, -32601, "unknown method: " + method).dump() << std::endl;
            continue;
        }

        const json params = req.contains("params") ? req.at("params") : json::object();
        const std::string tool_name = params.contains("name") && params.at("name").is_string()
                                          ? params.at("name").get<std::string>()
                                          : std::string{};
        const json args = params.contains("arguments") && params.at("arguments").is_object()
                              ? params.at("arguments")
                              : json::object();
        const std::string request_id = id.is_string() ? id.get<std::string>() : id.dump();

        const mcp::Tool* tool = mcp::find_tool(tools, tool_name);
        if (tool == nullptr) {
            std::cout << tool_failure(id, "no such tool: " + tool_name).dump() << std::endl;
            continue;
        }

        const std::string subject =
            tool->subject_arg.empty()
                ? std::string{}
                : (args.contains(tool->subject_arg) && args.at(tool->subject_arg).is_string()
                       ? args.at(tool->subject_arg).get<std::string>()
                       : std::string{});

        // Scope first, and a denial is audited too -- an agent reaching beyond its remit is
        // exactly the signal this table exists to surface.
        if (!scopes.may_call(agent_id, tool_name)) {
            if (tool->returns_pii) {
                audit.record(mcp::PiiAccess{.agent_id = agent_id,
                                            .principal = principal,
                                            .tool_name = tool_name,
                                            .request_id = request_id,
                                            .subject_id = subject,
                                            .outcome = mcp::AccessOutcome::DeniedScope});
            }
            std::cout << tool_failure(
                             id, "agent '" + agent_id + "' is not permitted to call " + tool_name)
                             .dump()
                      << std::endl;
            continue;
        }

        // A pinned agent may read exactly one subject. This is what stops a customer agent from
        // walking the subscriber base with the same tool it legitimately uses for its own user.
        if (tool->returns_pii) {
            const auto pinned = scopes.pinned_subject(agent_id);
            if (!pinned.empty() && pinned != subject) {
                audit.record(mcp::PiiAccess{.agent_id = agent_id,
                                            .principal = principal,
                                            .tool_name = tool_name,
                                            .request_id = request_id,
                                            .subject_id = subject,
                                            .outcome = mcp::AccessOutcome::DeniedPolicy});
                std::cout << tool_failure(id,
                                          "agent '" + agent_id +
                                              "' may only read its own pinned subscriber")
                                 .dump()
                          << std::endl;
                continue;
            }
            if (subject.empty()) {
                std::cout << tool_failure(id, tool_name + " requires " + tool->subject_arg).dump()
                          << std::endl;
                continue;
            }
        }

        auto result = tool->invoke(args, ctx);

        // THE RULE. Audited before the answer leaves the process; a failed audit refuses the read.
        // Placed after invoke() only so the field classes are the ones actually produced -- the
        // data has not been returned to anyone at this point, and is discarded if this fails.
        if (tool->returns_pii) {
            const bool logged =
                audit.record(mcp::PiiAccess{.agent_id = agent_id,
                                            .principal = principal,
                                            .tool_name = tool_name,
                                            .request_id = request_id,
                                            .subject_id = subject,
                                            .field_classes = result.field_classes,
                                            .withheld_classes = result.withheld_classes,
                                            .outcome = mcp::AccessOutcome::Granted});
            if (!logged) {
                spdlog::error("mcp: refusing to return PII for {} -- audit write failed",
                              tool_name);
                std::cout << tool_failure(id,
                                          "refused: the access audit could not be written, so "
                                          "this read is not permitted")
                                 .dump()
                          << std::endl;
                continue;
            }
        }

        std::cout << (result.ok ? tool_success(id, result.content) : tool_failure(id, result.error))
                         .dump()
                  << std::endl;
    }
    return 0;
}
