#include "pfcp_peer.hpp"

#include <spdlog/spdlog.h>

#include "pfcp_core/ie.hpp"

namespace smf {

PfcpPeer::PfcpPeer()
    : socket_(ioc_,
              boost::asio::ip::udp::endpoint(boost::asio::ip::udp::v4(),
                                             pfcp_core::kSmfCpFunctionPfcpPort)) {
    // SO_RCVTIMEO purely so receive_loop wakes periodically to check stop_ on shutdown -- distinct
    // from send_request_and_await_response's own T1 retry timing, which uses response_cv_'s own
    // wait_for below, not this socket-level timeout.
    constexpr timeval kPollTimeout{.tv_sec = 1, .tv_usec = 0};
    setsockopt(
        socket_.native_handle(), SOL_SOCKET, SO_RCVTIMEO, &kPollTimeout, sizeof(kPollTimeout));
    receive_thread_ = std::thread(&PfcpPeer::receive_loop, this);
    spdlog::info("smf: PFCP peer listening on 0.0.0.0:{}", pfcp_core::kSmfCpFunctionPfcpPort);
}

PfcpPeer::~PfcpPeer() {
    stop_ = true;
    // The receive thread sits in a synchronous receive_from. Asio implements that with its own
    // non-blocking socket plus poll(), so the SO_RCVTIMEO set in the constructor never fires and
    // the thread never wakes to see stop_ -- join() then blocks forever. Since ADR-0353 main()
    // returns on SIGTERM and this destructor actually runs; the SMF hung on every orderly stop
    // and 16 CI tests timed out in teardown (2026-09-14). Same cure as the UPF's PFCP loop
    // (ADR-0357): one datagram from the loopback makes receive_from return, the loop reads stop_.
    try {
        boost::asio::io_context nudge_ioc;
        boost::asio::ip::udp::socket nudge(nudge_ioc, boost::asio::ip::udp::v4());
        const std::uint8_t zero = 0;
        nudge.send_to(boost::asio::buffer(&zero, 1),
                      boost::asio::ip::udp::endpoint(boost::asio::ip::make_address("127.0.0.1"),
                                                     pfcp_core::kSmfCpFunctionPfcpPort));
    } catch (const std::exception&) {
        // Nothing useful to do during shutdown; the join below then waits for a real datagram.
    }
    if (receive_thread_.joinable()) {
        receive_thread_.join();
    }
}

void PfcpPeer::set_session_report_handler(SessionReportHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    on_session_report_request_ = std::move(handler);
}

void PfcpPeer::set_node_report_handler(NodeReportHandler handler) {
    std::lock_guard<std::mutex> lock(handler_mutex_);
    on_node_report_request_ = std::move(handler);
}

std::uint32_t PfcpPeer::allocate_sequence_number() {
    return next_sequence_number_++;
}

void PfcpPeer::receive_loop() {
    while (!stop_) {
        std::vector<std::uint8_t> recv_buf(2048);
        boost::asio::ip::udp::endpoint sender;
        boost::system::error_code ec;
        const std::size_t n = socket_.receive_from(boost::asio::buffer(recv_buf), sender, 0, ec);
        if (stop_) {
            break; // the shutdown nudge, or a real datagram that arrived at the same moment
        }
        if (ec) {
            continue; // real recv error or (far more often) the SO_RCVTIMEO poll interval expiring
        }

        std::vector<std::uint8_t> datagram(recv_buf.begin(),
                                           recv_buf.begin() + static_cast<std::ptrdiff_t>(n));
        std::size_t offset = 0;
        std::uint16_t ies_length = 0;
        const auto header = pfcp_core::decode_header(datagram, offset, ies_length);
        if (!header.has_value() || offset + ies_length > datagram.size()) {
            spdlog::warn("smf: PFCP peer dropped a malformed datagram from {}",
                         sender.address().to_string());
            continue;
        }
        std::vector<std::uint8_t> ie_bytes(datagram.begin() + static_cast<std::ptrdiff_t>(offset),
                                           datagram.begin() +
                                               static_cast<std::ptrdiff_t>(offset + ies_length));

        if (header->message_type == pfcp_core::MessageType::SessionReportRequest) {
            SessionReportHandler handler;
            {
                std::lock_guard<std::mutex> lock(handler_mutex_);
                handler = on_session_report_request_;
            }
            if (handler) {
                handler(*header, ie_bytes, sender);
            } else {
                spdlog::warn("smf: received a Sx Session Report Request with no handler installed "
                             "yet, dropping");
            }
            continue;
        }

        if (header->message_type == pfcp_core::MessageType::NodeReportRequest) {
            NodeReportHandler handler;
            {
                std::lock_guard<std::mutex> lock(handler_mutex_);
                handler = on_node_report_request_;
            }
            if (handler) {
                handler(*header, ie_bytes, sender);
            } else {
                spdlog::warn(
                    "smf: received a Sx Node Report Request with no handler installed yet, "
                    "dropping");
            }
            continue;
        }

        {
            std::lock_guard<std::mutex> lock(response_mutex_);
            pending_responses_[header->sequence_number] =
                PendingResponse{header->message_type, std::move(ie_bytes)};
        }
        response_cv_.notify_all();
    }
}

std::optional<std::vector<std::uint8_t>>
PfcpPeer::send_request_and_await_response(const boost::asio::ip::udp::endpoint& target,
                                          const std::vector<std::uint8_t>& request_pdu,
                                          pfcp_core::MessageType expected_response_type,
                                          std::uint32_t sequence_number,
                                          const std::string& procedure_name) {
    constexpr int kN1Retries = 3;
    constexpr auto kT1Timeout = std::chrono::seconds(2);

    for (int attempt = 0; attempt < kN1Retries; ++attempt) {
        {
            std::lock_guard<std::mutex> send_lock(send_mutex_);
            boost::system::error_code ec;
            socket_.send_to(boost::asio::buffer(request_pdu), target, 0, ec);
            if (ec) {
                spdlog::warn("smf: PFCP {} send failed: {}", procedure_name, ec.message());
                continue;
            }
        }

        std::unique_lock<std::mutex> lock(response_mutex_);
        const bool arrived = response_cv_.wait_for(lock, kT1Timeout, [&] {
            return pending_responses_.find(sequence_number) != pending_responses_.end();
        });
        if (!arrived) {
            spdlog::warn(
                "smf: PFCP {} attempt {} timed out, retrying", procedure_name, attempt + 1);
            continue;
        }
        auto pending = std::move(pending_responses_.at(sequence_number));
        pending_responses_.erase(sequence_number);
        lock.unlock();

        if (pending.message_type != expected_response_type) {
            spdlog::warn("smf: PFCP {} got a response with the right sequence number but wrong "
                         "message type, treating as failed",
                         procedure_name);
            return std::nullopt;
        }
        return pending.ie_bytes;
    }
    spdlog::warn("smf: PFCP {} exhausted {} retries", procedure_name, kN1Retries);
    return std::nullopt;
}

void PfcpPeer::send_fire_and_forget(const boost::asio::ip::udp::endpoint& target,
                                    const std::vector<std::uint8_t>& pdu) {
    std::lock_guard<std::mutex> send_lock(send_mutex_);
    boost::system::error_code ec;
    socket_.send_to(boost::asio::buffer(pdu), target, 0, ec);
    if (ec) {
        spdlog::warn("smf: PFCP fire-and-forget send failed: {}", ec.message());
    }
}

} // namespace smf
