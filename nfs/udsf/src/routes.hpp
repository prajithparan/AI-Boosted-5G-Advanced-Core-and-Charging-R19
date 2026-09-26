#pragma once

#include "sbi_core/http2_server.hpp"

#include <string>

#include "service.hpp"

namespace udsf {

// Every handler goes through this: one OpenTelemetry span per operation and a Prometheus counter
// udsf_requests_total{operation,status}.
sbi_core::http2::Handler instrument(const std::string& operation, sbi_core::http2::Handler h);

// Nudsf_DataRepository (TS 29.598 clause 6.1): 21 operations.
void add_dr_routes(sbi_core::http2::Server& server, Ctx& ctx);
// Nudsf_Timer (TS 29.598 clause 6.2): 6 operations.
void add_timer_routes(sbi_core::http2::Server& server, Ctx& ctx);

// Timer schedule helpers shared by the routes and the expiry worker.
std::int64_t timer_delete_at_ms(const Doc& timer);

} // namespace udsf
