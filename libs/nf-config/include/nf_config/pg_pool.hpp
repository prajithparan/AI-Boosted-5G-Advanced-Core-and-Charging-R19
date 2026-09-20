// A bounded PostgreSQL connection pool, so a store serves N concurrent requests instead of
// serialising every request behind one connection+mutex. Concurrency-within-an-instance -- the
// per-instance half of horizontal scaling (project_autoscaling_mandate). libpqxx connections are
// not thread-safe, so each request LEASES one exclusively for the duration of its transaction.
//
// Fail-fast (architecture-bonded rule, see nf_config.hpp): every connection is opened at
// construction; if any cannot connect, the process exits rather than serve degraded.
#pragma once

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <pqxx/pqxx>
#include <string>
#include <vector>

#include "nf_config/nf_config.hpp"

namespace nf_config {

class PgPool {
public:
    PgPool(const std::string& conninfo, std::size_t size) {
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

    Lease acquire() {
        std::unique_lock<std::mutex> lk(m_);
        cv_.wait(lk, [this] { return !free_.empty(); });
        auto* c = free_.back();
        free_.pop_back();
        return Lease(this, c);
    }

    std::size_t size() const { return conns_.size(); }

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
    std::mutex m_;
    std::condition_variable cv_;
};

} // namespace nf_config
