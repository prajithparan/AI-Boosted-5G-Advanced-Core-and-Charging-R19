// ADR-0331: the governance rules of the MCP tool server, tested as rules rather than as plumbing.
//
// These assert the two properties the PII layer exists to guarantee, both of which are easy to
// believe are true while being false:
//
//   1. An agent may only call the tools its scope allows, and may only read a subject it is
//      pinned to. Enforced server-side; an agent's own restraint is not an access control.
//   2. A PII read that cannot be audited does not happen.
//
// Rule 2 is verified by pointing the server at an audit database that does not exist and checking
// that a PII tool REFUSES rather than answering. That is the only way to test it: a working audit
// proves the happy path, not the guarantee.

#include <nlohmann/json.hpp>

#include "agent_scope.hpp"
#include "pii_audit.hpp"

#include <gtest/gtest.h>

using nlohmann::json;

namespace {

json scope_config() {
    return json{{"agents",
                 json{{"customer-agent",
                       json{{"tools", json::array({"get_balance", "explain_charge"})},
                            {"pinnedSubject", "imsi-999700000000001"}}},
                      {"ops-agent", json{{"tools", json::array({"list_nf_instances"})}}},
                      {"broken-agent", json::object()}}}};
}

} // namespace

TEST(McpAgentScope, AnAgentMayCallOnlyItsOwnTools) {
    const auto scopes = mcp::AgentScopeRegistry::from_config(scope_config());
    EXPECT_TRUE(scopes.may_call("customer-agent", "get_balance"));
    EXPECT_TRUE(scopes.may_call("ops-agent", "list_nf_instances"));
    // The crossing case that matters: an ops agent has no business reading a customer's balance.
    EXPECT_FALSE(scopes.may_call("ops-agent", "get_balance"));
    EXPECT_FALSE(scopes.may_call("customer-agent", "list_nf_instances"));
}

TEST(McpAgentScope, AnUnknownAgentIsDeniedRatherThanDefaulted) {
    const auto scopes = mcp::AgentScopeRegistry::from_config(scope_config());
    // A typo in an agent id must not silently become a new, unscoped identity.
    EXPECT_FALSE(scopes.is_known("custmer-agent"));
    EXPECT_FALSE(scopes.may_call("custmer-agent", "get_balance"));
    EXPECT_FALSE(scopes.may_call("", "get_balance"));
}

TEST(McpAgentScope, AnAgentWithNoToolListCanCallNothing) {
    const auto scopes = mcp::AgentScopeRegistry::from_config(scope_config());
    // A misconfigured agent must be able to do nothing, not everything. This is the direction a
    // permissive default would fail in, silently.
    EXPECT_TRUE(scopes.is_known("broken-agent"));
    EXPECT_FALSE(scopes.may_call("broken-agent", "get_balance"));
    EXPECT_TRUE(scopes.tools_for("broken-agent").empty());
}

TEST(McpAgentScope, APinnedAgentIsBoundToOneSubscriber) {
    const auto scopes = mcp::AgentScopeRegistry::from_config(scope_config());
    EXPECT_EQ(scopes.pinned_subject("customer-agent"), "imsi-999700000000001");
    // Unpinned is represented as empty, and is what lets an agent read any subject -- which is
    // why a customer agent must never be run unpinned.
    EXPECT_TRUE(scopes.pinned_subject("ops-agent").empty());
}

TEST(McpPiiAudit, AnUnwritableAuditRefusesRatherThanDegrading) {
    // Deliberately unreachable database. The store must report itself unusable and every record()
    // must fail -- which is what makes the server refuse to start and a PII read refuse to answer.
    // A "best effort" audit that silently succeeded here would be indistinguishable from no audit
    // at exactly the moment it matters.
    mcp::PiiAuditStore audit("postgresql://nobody@127.0.0.1:1/definitely-not-a-database");
    EXPECT_FALSE(audit.is_connected());
    EXPECT_FALSE(audit.record(mcp::PiiAccess{.agent_id = "customer-agent",
                                             .principal = "someone",
                                             .tool_name = "get_balance",
                                             .subject_id = "imsi-999700000000001",
                                             .outcome = mcp::AccessOutcome::Granted}));
}

TEST(McpPiiAudit, OutcomeNamesAreStableBecauseTheSchemaConstrainsThem) {
    // The table has a CHECK constraint on these exact strings; a rename here would make every
    // audit write fail at runtime rather than at compile time.
    EXPECT_STREQ(mcp::to_string(mcp::AccessOutcome::Granted), "granted");
    EXPECT_STREQ(mcp::to_string(mcp::AccessOutcome::DeniedScope), "denied_scope");
    EXPECT_STREQ(mcp::to_string(mcp::AccessOutcome::DeniedPolicy), "denied_policy");
}

TEST(McpAgentScope, ALauncherPinOverridesConfigAndCannotCreateAnAgent) {
    // ADR-0333: the pin comes from the launcher, which already knows whose session this is. The
    // agent never chooses its own subject -- an agent that could would be able to enumerate the
    // subscriber base with the same tool it legitimately uses for one customer.
    auto scopes = mcp::AgentScopeRegistry::from_config(scope_config());
    EXPECT_TRUE(scopes.pin_subject("customer-agent", "imsi-999700000000999"));
    EXPECT_EQ(scopes.pinned_subject("customer-agent"), "imsi-999700000000999");

    // Pinning an agent that does not exist must fail rather than quietly creating one: a typo in
    // a launcher script would otherwise mint a new identity with no tool scope but a real pin,
    // and the failure would only show up as confusing denials later.
    EXPECT_FALSE(scopes.pin_subject("customre-agent", "imsi-999700000000999"));
    EXPECT_FALSE(scopes.is_known("customre-agent"));
}
