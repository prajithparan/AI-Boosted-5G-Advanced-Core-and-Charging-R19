// ADR-0459: an accepted SCTP association must not inherit the listener's SO_RCVTIMEO.
//
// The AMF puts a 500 ms receive timeout on its NGAP listener so its accept loop can poll for
// shutdown (ADR-0394). Linux copies that option to every accepted socket, and receive() reports a
// timeout as "nothing came back" -- so the AMF tore down any gNB association that was idle for
// 500 ms. Found as a TSan-only failure of AmfNgapTestGnb.FullN2HandoverRelayThroughExecution-
// AndSourceRelease: the target gNB sat idle while the UE registered, the AMF logged "gNB
// association closed" ~500 ms after its NGSetup, and the handover found no target.
//
// This drives the library directly: a listener with a short poll timeout, a client that stays
// quiet for several times that, then sends. The accepted side must still receive the message.

#include <chrono>
#include <cstdint>
#include <optional>
#include <thread>
#include <vector>

#include "ngap_core/sctp_socket.hpp"

#include <gtest/gtest.h>

namespace {

constexpr const char* kAddress = "127.0.0.1";
constexpr std::uint16_t kPort = 38499; // clear of the AMF's 38412 used by the NGAP suites

} // namespace

TEST(SctpAcceptTimeout, AnAcceptedAssociationDoesNotInheritTheListenersPollTimeout) {
    ngap_core::SctpSocket listener;
    listener.bind_and_listen(kAddress, kPort);
    listener.set_receive_timeout(std::chrono::milliseconds(100));

    std::thread client_thread([] {
        ngap_core::SctpSocket client;
        client.connect(kAddress, kPort);
        // Idle for 5x the listener's poll interval before saying anything.
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        client.send({0x01, 0x02, 0x03});
        std::this_thread::sleep_for(std::chrono::milliseconds(200)); // let it be read
    });

    std::optional<ngap_core::SctpSocket> accepted;
    for (int attempt = 0; attempt < 50 && !accepted.has_value(); ++attempt) {
        accepted = listener.accept_or_timeout();
    }
    ASSERT_TRUE(accepted.has_value()) << "the client never connected";

    const auto received = accepted->receive();
    client_thread.join();
    EXPECT_EQ(received, (std::vector<std::uint8_t>{0x01, 0x02, 0x03}))
        << "receive() returned before the client spoke -- the accepted socket inherited the "
           "listener's receive timeout";
}
