// A bounded PostgreSQL connection pool, so a store serves N concurrent requests instead of
// serialising every request behind one connection+mutex. Concurrency-within-an-instance -- the
// per-instance half of horizontal scaling (project_autoscaling_mandate). libpqxx connections are
// not thread-safe, so each request LEASES one exclusively for the duration of its transaction.
//
// Fail-fast (architecture-bonded rule, see nf_config.hpp): every connection is opened at
// construction; if any cannot connect, the process exits rather than serve degraded.
//
// ADR-0445/ADR-0449: `acquire()` used to `cv_.wait()` with no bound -- real, disclosed debt
// (ADR-0445): a fully exhausted pool hung the calling thread forever, with no metric ever showing
// it happened. Hardened here: a bounded wait that throws `PoolExhaustedError` (a real, recoverable
// overload condition, not a bug) when nothing frees up in time, plus a counter so it's observable.
// Throwing rather than returning `std::optional<Lease>` is a deliberate, narrow choice, not a
// reach for convenience: every call site already either (a) sits inside an HTTP route handler,
// which libs/sbi-core's own generic handler-exception boundary (ADR-0360) already converts to a
// ProblemDetails response -- widened here to answer 503 specifically for this exception rather than
// the generic 500 -- or (b) is nfs/chf/src/rating_decision_store.cpp's own two methods, which
// already wrap their own pool access in a local try/catch with the explicit, pre-existing
// "best-effort, must never block or fail the real charging response" discipline. Both real
// backstops were confirmed to exist, not assumed, before this was built on top of them -- see
// ADR-0449 for the call-site audit. This keeps all ~50 existing `pool_.acquire()` call sites
// byte-identical on the happy path (still `auto lease = pool_.acquire(); pqxx::work txn(lease.
// conn());`), with no function's return type widened just to thread an error through one layer.
#pragma once

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <pqxx/pqxx>
#include <stdexcept>
#include <string>
#include <vector>

#include "nf_config/nf_config.hpp"

namespace nf_config {

// A real, recoverable "the pool was fully checked out and nothing freed up in time" outcome --
// deliberately a distinct type from a generic pqxx/std::exception, so a catch site (ADR-0360's
// generic handler boundary) can tell "we're overloaded, back off" (503) apart from "something is
// actually broken" (500) without parsing message text.
class PoolExhaustedError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class PgPool {
public:
    // acquire_timeout: how long acquire() waits for a connection to free up before throwing.
    // Trailing default (5s -- generous relative to any real query this codebase runs, so normal
    // operation never observes it) rather than a new required parameter threaded through every one
    // of the ~10 existing call sites/config files/schemas that construct a PgPool: this is a
    // library-level safety bound, not a per-deployment tuning knob any operator has asked to set
    // per NF yet -- extend to a configured value (per CLAUDE.md's no-speculative-abstraction rule)
    // when one actually needs a different bound, not speculatively for all of them now.
    PgPool(const std::string& conninfo,
           std::size_t size,
           std::chrono::milliseconds acquire_timeout = std::chrono::milliseconds(5000))
        : acquire_timeout_(acquire_timeout) {
        if (size == 0) {
            size = 1;
        }
        try {
            for (std::size_t i = 0; i < size; ++i) {
                conns_.push_back(std::make_unique<pqxx::connection>(conninfo));
            }
        } catch (const std::exception& e) {
            fatal(std::string("PostgreSQL connection pool could not connect at startup: ") +
                  e.what());
        }
        for (auto& c : conns_) {
            free_.push_back(c.get());
        }
    }

    // RAII exclusive lease of one connection; returned to the pool on destruction.
    class Lease {
    public:
        Lease(PgPool* pool, pqxx::connection* conn) : pool_(pool), conn_(conn) {}
        ~Lease() {
            if (pool_ != nullptr) {
                pool_->release(conn_);
            }
        }
        Lease(Lease&& o) noexcept : pool_(o.pool_), conn_(o.conn_) {
            o.pool_ = nullptr;
            o.conn_ = nullptr;
        }
        Lease& operator=(Lease&&) = delete;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
        pqxx::connection& conn() { return *conn_; }

    private:
        PgPool* pool_;
        pqxx::connection* conn_;
    };

    // Throws PoolExhaustedError (not: blocks forever) if no connection frees up within
    // acquire_timeout_. See this file's own header for why every real call site is already safe
    // to receive that exception.
    Lease acquire() {
        std::unique_lock<std::mutex> lk(m_);
        const bool got = cv_.wait_for(lk, acquire_timeout_, [this] { return !free_.empty(); });
        if (!got) {
            exhaustion_count_.fetch_add(1, std::memory_order_relaxed);
            throw PoolExhaustedError(
                "PostgreSQL connection pool exhausted: no connection freed up within " +
                std::to_string(acquire_timeout_.count()) + "ms (pool size " +
                std::to_string(conns_.size()) + ")");
        }
        auto* c = free_.back();
        free_.pop_back();
        return Lease(this, c);
    }

    std::size_t size() const { return conns_.size(); }

    // Real metrics ADR-0445 flagged as missing. `in_use()` locks (a point-in-time snapshot for a
    // gauge callback, not a hot path); `exhaustion_count()` is lock-free (a monotonic counter
    // read).
    std::size_t in_use() const {
        std::lock_guard<std::mutex> lk(m_);
        return conns_.size() - free_.size();
    }
    std::uint64_t exhaustion_count() const {
        return exhaustion_count_.load(std::memory_order_relaxed);
    }

private:
    void release(pqxx::connection* c) {
        {
            std::lock_guard<std::mutex> lk(m_);
            free_.push_back(c);
        }
        cv_.notify_one();
    }

    std::vector<std::unique_ptr<pqxx::connection>> conns_;
    std::vector<pqxx::connection*> free_;
    mutable std::mutex m_;
    std::condition_variable cv_;
    std::chrono::milliseconds acquire_timeout_;
    std::atomic<std::uint64_t> exhaustion_count_{0};
};

} // namespace nf_config
