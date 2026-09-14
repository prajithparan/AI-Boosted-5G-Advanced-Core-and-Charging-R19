#pragma once

#include <boost/asio/io_context.hpp>

#include <functional>

// ADR-0239: real server-side request concurrency. Every NF's main() used to call ioc.run() once,
// single-threaded -- meaning the entire process, however many TCP connections or HTTP/2 streams
// were open, could only ever process one request at a time. Server::Connection is now
// strand-per-connection with off-strand handler dispatch (see http2_server.cpp), which makes it
// SAFE to drive the same io_context from multiple threads; this is the piece that actually does
// it, replacing a bare `ioc.run();` call.

namespace sbi_core {

// Runs `ioc` on a small pool of worker threads (the calling thread plus N-1 additional threads,
// N = std::thread::hardware_concurrency(), floored at 2 so single-core/undetectable-core-count
// environments still get at least one extra worker thread rather than silently degrading back to
// the old single-threaded behavior). Blocks until ioc runs out of work (or is stopped), same as a
// direct ioc.run() call would -- a drop-in replacement, just with real parallelism.
void run_multi_threaded(boost::asio::io_context& ioc);

// ADR-0353/0357: stop `ioc` when the process receives SIGTERM or SIGINT.
//
// run_multi_threaded registers its own io_context automatically. An NF that ALSO blocks a thread
// on a second io_context -- UPF's PFCP loop, AMF's NGAP task -- must register that one too, or
// the signal stops only the SBI loop and the process hangs in the other. Found exactly that way:
// UPF logged "signal 15 received" and then sat in the PFCP socket's recv until SIGKILL.
//
// The watcher is a single process-wide signal_set on its own tiny thread, so it does not depend
// on any registered io_context still running when the signal arrives, and stops every registered
// io_context at once. Registering the same io_context twice is harmless.
void stop_on_shutdown_signal(boost::asio::io_context& ioc);

// Run `fn` when the process receives SIGTERM or SIGINT, before the registered io_contexts are
// stopped. For the loop that ioc.stop() cannot reach: a thread blocked in a SYNCHRONOUS socket
// call (UPF's PFCP receive_from) is not inside run(), so it must be woken some other way -- the
// callback sets a stop flag and nudges the socket. Runs on the watcher's thread; keep it short.
void on_shutdown_signal(std::function<void()> fn);

} // namespace sbi_core
