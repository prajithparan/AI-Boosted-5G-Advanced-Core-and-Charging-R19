#pragma once

// ADR-0331: the read-only tool surface.
//
// Every tool here is a READ. There is no write path and no config-change path, by construction
// rather than by policy: `CLAUDE.md` requires any write or config action to have explicit human
// approval in the loop, and the honest way to guarantee that in v1 is to have nothing to approve.
//
// Tools are split into two kinds, and the distinction is the whole point:
//   * PII tools return subscriber-identifying data. They MUST audit before returning, and refuse
//     the read if the audit cannot be written.
//   * Non-PII tools (catalog, NF inventory) return operator-domain data with no subject, so they
//     record no subject-linked audit row.
// `returns_pii` is a property of the tool, declared once, so a new tool cannot quietly skip the
// audit by forgetting to call it -- the dispatcher decides based on this flag, not the tool body.

#include <nlohmann/json.hpp>

#include <functional>
#include <string>
#include <vector>

namespace sbi_core::http2 {
class Client;
}

namespace mcp {

class PiiAuditStore;
class AgentScopeRegistry;

struct ToolResult {
    bool ok = false;
    nlohmann::json content = nlohmann::json::object();
    std::string error;
    // Classes actually returned, for the audit row. Populated by the tool, never guessed by the
    // dispatcher, because only the tool knows what it ended up including.
    std::vector<std::string> field_classes;
    std::vector<std::string> withheld_classes;
};

struct ToolContext {
    sbi_core::http2::Client* client = nullptr;
    nlohmann::json config;
};

struct Tool {
    std::string name;
    std::string description;
    nlohmann::json input_schema;
    bool returns_pii = false;
    // Which argument names the subject, for the audit row. Empty for non-PII tools.
    std::string subject_arg;
    std::function<ToolResult(const nlohmann::json& args, ToolContext&)> invoke;
};

// The v1 registry. Read `docs/AI_AGENTS_AND_MODELS.md` for why these five and not others: each is
// backed by data this system already holds, so none of them fabricates an answer.
std::vector<Tool> build_tool_registry();

const Tool* find_tool(const std::vector<Tool>& tools, const std::string& name);

} // namespace mcp
