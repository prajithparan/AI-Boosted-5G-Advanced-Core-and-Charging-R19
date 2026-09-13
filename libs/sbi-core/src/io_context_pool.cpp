#include "sbi_core/io_context_pool.hpp"

#include <boost/asio/signal_set.hpp>
#include <spdlog/spdlog.h>

#include <algorithm>
#include <thread>
#include <vector>

namespace sbi_core {

void run_multi_threaded(boost::asio::io_context& ioc) {
    // ADR-0353: stop on SIGTERM/SIGINT so main() RETURNS instead of the process being terminated
    // where it stands.
    //
    // Nothing in this codebase caught either signal, which meant no NF ever exited cleanly and no
    // destructor ever ran. For most NFs that is merely untidy. For CHF it lost billing data:
    // ADR-0338 gave CdrWriter a destructor that flushes buffered CDRs precisely so "a clean
    // shutdown that dropped buffered rows" could not happen -- and then there was no way to shut
    // down cleanly, so that destructor was unreachable. Measured, not theorised: the 3M-CDR run
    // ended with 19 rows missing across the last 15 sessions, all in the final 20 refs, with no
    // write error logged anywhere, because eight CHF instances were SIGTERMed while each held a
    // partial batch. `docker stop` and a Kubernetes pod eviction both send exactly this signal.
    //
    // DISCLOSED: ioc.stop() is abrupt -- queued handlers do not run, so a request in flight at the
    // moment of the signal is dropped rather than completed. That is a strictly smaller loss than
    // dropping every buffered CDR, and a graceful drain (stop accepting, finish in-flight, then
    // stop) is the better answer whenever someone needs it. This is the durability fix, not a
    // full graceful-shutdown implementation, and is not claimed as one.
    boost::asio::signal_set signals(ioc, SIGINT, SIGTERM);
    signals.async_wait([&ioc](const boost::system::error_code& ec, int signal_number) {
        if (ec) {
            return;
        }
        spdlog::info("sbi: signal {} received -- stopping, so buffered state is flushed on the way "
                     "out",
                     signal_number);
        ioc.stop();
    });

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
