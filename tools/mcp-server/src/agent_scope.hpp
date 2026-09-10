#pragma once

// ADR-0331: per-agent tool scoping, enforced server-side.
//
// The rule this exists to make true: least privilege per agent. A technical/ops agent has no
// business reading a subscriber's bill; a customer agent has no business enumerating other
// subscribers. Scope is checked HERE, before a tool runs, never by trusting the agent to limit
// itself -- an agent's own restraint is not an access control.
//
// Scopes are configuration (config/mcp-server.json), not code, for the same reason PCF's
// policy_counter_actions are: adding an agent or narrowing a remit is an operator decision, and
// making it a code change guarantees it happens late or not at all.

#include <nlohmann/json.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>

namespace mcp {

struct AgentScope {
    std::string agent_id;
    std::unordered_set<std::string> allowed_tools;
    // The subject this agent may read, when it is pinned to one. Empty means the agent is not
    // restricted to a single subscriber -- which is a deliberately conspicuous configuration,
    // because it is what a customer agent must never have.
    std::string pinned_subject;
};

class AgentScopeRegistry {
public:
    // Parses the `agents` object of the server config. An agent with no `tools` array gets an
    // EMPTY allow-list, not a permissive one: a misconfigured agent must be able to do nothing,
    // not everything.
    static AgentScopeRegistry from_config(const nlohmann::json& config);

    bool is_known(const std::string& agent_id) const;
    bool may_call(const std::string& agent_id, const std::string& tool_name) const;
    // Empty when the agent is not pinned to one subscriber.
    std::string pinned_subject(const std::string& agent_id) const;
    std::unordered_set<std::string> tools_for(const std::string& agent_id) const;

    // ADR-0333: pin an agent to one subscriber at launch time.
    //
    // This is the property that makes a customer agent safe to point at a live subscriber base:
    // the subject comes from the LAUNCHER -- a trusted caller that already knows which customer
    // the session is about -- and never from the agent or from the model driving it. An agent
    // that could choose its own subject could enumerate every subscriber using the same tool it
    // legitimately uses for one.
    //
    // Returns false for an unknown agent, so a typo in a launcher cannot create an unpinned
    // identity by accident.
    bool pin_subject(const std::string& agent_id, const std::string& subject);

private:
    std::unordered_map<std::string, AgentScope> agents_;
};

} // namespace mcp
