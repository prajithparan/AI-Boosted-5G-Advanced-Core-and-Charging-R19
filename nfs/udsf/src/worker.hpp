#pragma once

// The UDSF's own clock (ADR-0402): record ttl expiry (TS 29.598 5.2.2.6.2), subscription expiry
// and its advance notice (5.2.2.6.4), timer expiry with periodic repetition and deleteAfter
// (5.3.2.6.2), and delivery of every queued notification (including data-change, 5.2.2.6.3).
//
// Every replica runs one worker. Due work is claimed from Valkey sorted sets with ZREM, so each
// expiry is handled by exactly one replica; notifications are popped from a Valkey list, so each
// is POSTed once. Delivery is at-most-once: a failed POST is logged and counted, not retried
// (disclosed in ADR-0402).

#include "sbi_core/http2_client.hpp"

#include <atomic>
#include <chrono>

#include "service.hpp"

namespace udsf {

class Worker {
public:
    Worker(Ctx& ctx, sbi_core::http2::Client& client, std::chrono::milliseconds interval);
    void run(); // until stop()
    void stop() { stop_ = true; }
    // One pass over every storage; returns the number of notifications delivered (tests/CLI).
    int tick();

private:
    void sweep(const StorageRef& s, std::int64_t now);
    int deliver(const StorageRef& s);

    Ctx& ctx_;
    sbi_core::http2::Client& client_;
    std::chrono::milliseconds interval_;
    std::atomic<bool> stop_{false};
};

} // namespace udsf
