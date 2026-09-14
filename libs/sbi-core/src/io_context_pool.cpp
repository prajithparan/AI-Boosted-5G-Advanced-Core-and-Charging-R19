#include "sbi_core/io_context_pool.hpp"

#include <boost/asio/executor_work_guard.hpp>
#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace sbi_core {

namespace {

// One watcher per process. It owns its own io_context and thread, so the signal is caught even
// after the registered contexts have stopped or before the first has started -- registration
// order across threads is otherwise a race (UPF registers its SBI context from a detached thread
// and its PFCP context from main, in whichever order the scheduler picks).
class ShutdownWatcher {
public:
    static ShutdownWatcher& instance() {
        static ShutdownWatcher w;
        return w;
    }

    void on_signal(std::function<void()> fn) {
        const std::lock_guard<std::mutex> lock(mutex_);
        callbacks_.push_back(std::move(fn));
    }

    void watch(boost::asio::io_context& target) {
        const std::lock_guard<std::mutex> lock(mutex_);
        for (auto* t : targets_) {
            if (t == &target) {
                return;
            }
        }
        targets_.push_back(&target);
    }

private:
    ShutdownWatcher()
        : signals_(ioc_, SIGINT, SIGTERM), guard_(boost::asio::make_work_guard(ioc_)) {
        signals_.async_wait([this](const boost::system::error_code& ec, int signal_number) {
            if (ec) {
                return;
            }
            spdlog::info("sbi: signal {} received -- stopping every io_context, so buffered state "
                         "is flushed on the way out",
                         signal_number);
            const std::lock_guard<std::mutex> lock(mutex_);
            for (auto& fn : callbacks_) {
                fn();
            }
            for (auto* t : targets_) {
                t->stop(); // thread-safe in Asio; wakes whichever thread is inside run()
            }
        });
        thread_ = std::thread([this] { ioc_.run(); });
    }

    // Joined, not detached. The first version detached it, and the process then SIGSEGVed on
    // exit: static destruction tore this object down while the thread was still inside
    // ioc_.run(). RAII all the way -- stop the loop, join the thread, then the members go.
    ~ShutdownWatcher() {
        ioc_.stop();
        if (thread_.joinable()) {
            thread_.join();
        }
    }

    boost::asio::io_context ioc_;
    boost::asio::signal_set signals_;
    boost::asio::executor_work_guard<boost::asio::io_context::executor_type> guard_;
    std::thread thread_;
    std::mutex mutex_;
    std::vector<boost::asio::io_context*> targets_;
    std::vector<std::function<void()>> callbacks_;
};

} // namespace

void stop_on_shutdown_signal(boost::asio::io_context& ioc) {
    ShutdownWatcher::instance().watch(ioc);
}

void on_shutdown_signal(std::function<void()> fn) {
    ShutdownWatcher::instance().on_signal(std::move(fn));
}

void run_multi_threaded(boost::asio::io_context& ioc) {
    // ADR-0353: stop on SIGTERM/SIGINT so main() RETURNS instead of the process being terminated
    // where it stands -- nothing in this codebase caught either signal before, so no destructor
    // ever ran, and CHF lost buffered CDRs on every orderly stop. ADR-0357 moved the watcher out
    // of this function into a process-wide one (above), because an NF with a second blocking
    // io_context was only half-stopped by a signal_set tied to this one.
    //
    // DISCLOSED: ioc.stop() is abrupt -- queued handlers do not run, so a request in flight at the
    // moment of the signal is dropped rather than completed. A strictly smaller loss than dropping
    // every buffered CDR; a graceful drain is the better answer whenever someone needs it.
    stop_on_shutdown_signal(ioc);

    const unsigned int worker_count = std::max(2u, std::thread::hardware_concurrency());

    std::vector<std::thread> workers;
    workers.reserve(worker_count - 1);
    for (unsigned int i = 1; i < worker_count; ++i) {
        workers.emplace_back([&ioc] { ioc.run(); });
    }

    ioc.run(); // calling thread participates too, instead of just waiting on the others

    for (auto& worker : workers) {
        worker.join();
    }
}

} // namespace sbi_core
