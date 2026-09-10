#include "agent_scope.hpp"

#include <spdlog/spdlog.h>

namespace mcp {

AgentScopeRegistry AgentScopeRegistry::from_config(const nlohmann::json& config) {
    AgentScopeRegistry registry;
    if (!config.contains("agents") || !config.at("agents").is_object()) {
        // No agents configured means no agent can call anything. The server still starts, so the
        // misconfiguration is visible through denied calls and audit rows rather than a silent
        // wide-open default.
        spdlog::warn("mcp: config has no `agents` object -- every tool call will be denied");
        return registry;
    }
    for (const auto& [agent_id, spec] : config.at("agents").items()) {
        AgentScope scope;
        scope.agent_id = agent_id;
        if (spec.contains("tools") && spec.at("tools").is_array()) {
            for (const auto& t : spec.at("tools")) {
                if (t.is_string()) {
                    scope.allowed_tools.insert(t.get<std::string>());
                }
            }
        }
        if (spec.contains("pinnedSubject") && spec.at("pinnedSubject").is_string()) {
            scope.pinned_subject = spec.at("pinnedSubject").get<std::string>();
        }
        if (scope.allowed_tools.empty()) {
            spdlog::warn("mcp: agent '{}' has an empty tool allow-list and can call nothing",
                         agent_id);
        }
        registry.agents_.emplace(agent_id, std::move(scope));
    }
    return registry;
}

bool AgentScopeRegistry::is_known(const std::string& agent_id) const {
    return agents_.find(agent_id) != agents_.end();
}

bool AgentScopeRegistry::may_call(const std::string& agent_id, const std::string& tool_name) const {
    const auto it = agents_.find(agent_id);
    // An unknown agent is denied rather than defaulted. Anything else means a typo in an agent id
    // silently becomes a new, unscoped identity.
    if (it == agents_.end()) {
        return false;
    }
    return it->second.allowed_tools.count(tool_name) > 0;
}

std::string AgentScopeRegistry::pinned_subject(const std::string& agent_id) const {
    const auto it = agents_.find(agent_id);
    return it == agents_.end() ? std::string{} : it->second.pinned_subject;
}

bool AgentScopeRegistry::pin_subject(const std::string& agent_id, const std::string& subject) {
    const auto it = agents_.find(agent_id);
    if (it == agents_.end()) {
        spdlog::warn("mcp: cannot pin unknown agent '{}'", agent_id);
        return false;
    }
    it->second.pinned_subject = subject;
    spdlog::info("mcp: agent '{}' pinned to a single subject for this session", agent_id);
    return true;
}

std::unordered_set<std::string> AgentScopeRegistry::tools_for(const std::string& agent_id) const {
    const auto it = agents_.find(agent_id);
    return it == agents_.end() ? std::unordered_set<std::string>{} : it->second.allowed_tools;
}

} // namespace mcp
