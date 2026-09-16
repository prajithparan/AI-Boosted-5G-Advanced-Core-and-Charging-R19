#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "li_core/x1.hpp"

// The NE side of LI_X1 (ADR-0372): the request handler an NF's POI installs on its own HTTP
// server at /X1/NE (TS 103 221-1 7.2.2.2), and the keepalive state machine of clause 6.6.2.
// Transport-agnostic -- the NF already runs an nghttp2 server; this turns a received X1 request
// body into the response body. X1-level errors are returned as a 200 OK body carrying an
// ErrorResponse (7.2.2.2), so `handle_request` always yields a body and the caller always
// answers HTTP 200 unless the HTTP layer itself failed.

#pragma GCC visibility push(default)
namespace li_core::x1 {

// What the POI's task store does with each provisioning request. Each returns std::nullopt on
// success or the X1 error code to report (the handler builds the ErrorResponse). The handler
// fills OK responses and the envelope; these only decide accept/reject and record the task.
struct TaskStoreCallbacks {
    std::function<std::optional<ErrorCode>(const TaskDetails&)> activate_task;
    std::function<std::optional<ErrorCode>(const TaskDetails&)> modify_task;
    std::function<std::optional<ErrorCode>(const std::string& xid)> deactivate_task;
    std::function<std::optional<ErrorCode>()> deactivate_all_tasks;
    std::function<std::optional<ErrorCode>(const DestinationDetails&)> create_destination;
    std::function<std::optional<ErrorCode>(const std::string& did)> remove_destination;
    std::function<std::optional<ErrorCode>()> remove_all_destinations;
    // Optional identity check (8.x): the ADMF/NE identifiers vs. the TLS peer certificate. Return
    // the error code (1040/1060) to reject, std::nullopt to accept. If unset, identifiers are not
    // checked at the X1 layer.
    std::function<std::optional<ErrorCode>(const MessageHeader&)> check_identity;
    // This NE's own identifier, stamped into every response's neIdentifier if a request omitted a
    // usable one; the ADMF identifier is echoed from the request.
    std::string ne_identifier;
    bool keepalive_supported = true;
};

// Handle one X1 request document (a RequestContainer). Returns the X1Response (or
// X1TopLevelErrorResponse) document to send back with HTTP 200.
std::string handle_request(const std::string& xml, const TaskStoreCallbacks& cb);

// TS 103 221-1 6.6.2 keepalive state machine, pure and testable (no real clock/socket). The NE
// asserts the ADMF is alive; if TIME_P2 passes with no X1 request it raises a fault and, unless
// disabled, eventually deactivates all tasks as a security measure.
class KeepaliveMonitor {
public:
    struct Config {
        std::chrono::seconds time_p2{3600}; // no-X1 window before a fault (default 1 h)
        std::chrono::seconds time_p1{60};   // wait for the ReportNEIssue OK (default 1 min)
        std::chrono::seconds time_p3{7200}; // after the OK, before deactivating (default 2 h)
        bool allow_deactivate_all = false;  // the security default is configurable
    };
    // What the caller should do as a result of a tick/event.
    enum class Action : std::uint8_t {
        None,
        SendFaultReport,    // ReportNEIssue "FaultReport", code 9050
        SendFaultCleared,   // ReportNEIssue "FaultCleared"
        DeactivateAllTasks, // + ReportNEIssue "Alert" code 10000
    };
    explicit KeepaliveMonitor(Config cfg) : cfg_(cfg) {}

    // Any X1 request (including Keepalive) resets P2 and clears an outstanding fault.
    Action on_x1_request(std::chrono::steady_clock::time_point now);
    // The ADMF acknowledged the fault report: stop P1, start P3 (if deactivation is allowed).
    Action on_fault_report_ack(std::chrono::steady_clock::time_point now);
    // Called periodically. Emits the transitions when P2, P1 or P3 expire.
    Action tick(std::chrono::steady_clock::time_point now);

    static constexpr int kKeepalivesNotReceived = 9050;
    static constexpr int kDatabaseCleared = 10000;

private:
    enum class State : std::uint8_t { Normal, FaultRaised, AwaitingDeactivation };
    Config cfg_;
    State state_ = State::Normal;
    std::chrono::steady_clock::time_point last_x1_{};
    std::chrono::steady_clock::time_point fault_at_{};
    std::chrono::steady_clock::time_point ack_at_{};
    bool seen_first_ = false;
};

} // namespace li_core::x1
#pragma GCC visibility pop
