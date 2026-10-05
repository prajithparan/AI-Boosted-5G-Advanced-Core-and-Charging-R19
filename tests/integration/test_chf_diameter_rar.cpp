// ADR-0448 (increment 2 of ADR-0445/0446, continued): real end-to-end coverage for Diameter
// RAR/RAA (RFC 6733 section 8.3 Re-Auth-Request/Answer, Command-Code 258).
//
// diameter_server.cpp/.hpp implement `DiameterServer::send_reauth_request()` -- it sends a real
// RAR for a live Gy session and blocks for the matching RAA. main.cpp exposes it through a new
// `POST /chf-admin/v1/sessions/{SessionId}/reauthorize` route, since nothing in this codebase
// (no reconciliation sweep, no other caller) exercises it otherwise. This file is the first real
// proof the whole path works: a fake Diameter peer establishes a real Gy session over the wire
// (CER/CEA, then a real CCR-Initial/CCA), the test calls the new admin endpoint, and the fake peer
// answers the RAR CHF pushes back on that same connection with a real RAA -- then both the wire
// protocol and the HTTP response are asserted.
//
// Modeled on test_chf_protocol_ceilings.cpp's fake-peer pattern (raw boost::asio socket, hand-built
// AVPs via diameter_core's own codec) -- that file's own CCR-Initial is deliberately minimal
// (Session-Id only) because its ceiling check fires before decode_ccr ever runs. This test needs a
// CCR-Initial decode_ccr() actually accepts, so it adds the two other mandatory AVPs (CC-Request-
// Type, CC-Request-Number per dict_dcca.c:1360-1367) -- no Subscription-Id/MSCC, since session
// registration into the RAR registry (diameter_server.cpp's `session_to_connection_`) happens
// unconditionally before CHF's own per-MSCC charge_one_usage loop, so this test needs neither a
// live product-catalog/balance-management nor a real rated usage to prove the RAR/RAA mechanism.
//
// CHF_CDR_DIRECT_INSERT=false / CHF_CDR_EVENT_BUS_BROKERS=<unreachable>: same real, disclosed
// workaround scripts/run-chf-rating-baseline.sh already uses -- CHF's default direct-insert CDR
// mode FATALs at startup without a reachable Doris, and this test needs neither Doris nor a CDR
// (no MSCC means charge_one_usage, and therefore any CDR write, never runs).

#include "sbi_core/http2_client.hpp"

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>
#include <nlohmann/json.hpp>

#include <chrono>
#include <optional>
#include <string>
#include <thread>
#include <vector>

#include "diameter_core/avp.hpp"
#include "diameter_core/dictionary.hpp"
#include "diameter_core/header.hpp"
#include "spawn_guard.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;
using nlohmann::json;

sbi_core::http2::Client make_mtls_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

std::string fetch_token(sbi_core::http2::Client& client, const std::string& scope) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=" + scope +
               "&targetNfType=CHF";
    auto resp = client.send(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

std::vector<std::uint8_t> build_cer() {
    using namespace diameter_core;
    std::vector<std::uint8_t> avps;
    Avp origin_host;
    origin_host.code = dictionary::Avp::kOriginHost;
    origin_host.flags = AvpFlag::kMandatory;
    origin_host.data = encode_octet_string("test-peer.example.com");
    encode_avp(avps, origin_host);
    Avp origin_realm;
    origin_realm.code = dictionary::Avp::kOriginRealm;
    origin_realm.flags = AvpFlag::kMandatory;
    origin_realm.data = encode_octet_string("example.com");
    encode_avp(avps, origin_realm);

    Header h;
    h.flags = CommandFlag::kRequest;
    h.command_code = dictionary::Command::kCapabilitiesExchange;
    h.application_id = 0;
    h.hop_by_hop_id = 0x0BADF00D;
    h.end_to_end_id = 0x0BADBEEF;
    auto msg = encode_header(h, static_cast<std::uint32_t>(avps.size()));
    msg.insert(msg.end(), avps.begin(), avps.end());
    return msg;
}

// A real CCR-Initial: Session-Id, CC-Request-Type=INITIAL, CC-Request-Number=0 -- the three
// mandatory AVPs decode_ccr() requires (diameter_server.cpp:225-227). No Subscription-Id/MSCC;
// see this file's header for why that's enough to register the session for RAR.
std::vector<std::uint8_t> build_real_ccr_initial(const std::string& session_id) {
    using namespace diameter_core;
    std::vector<std::uint8_t> avps;
    Avp session_id_avp;
    session_id_avp.code = dictionary::Avp::kSessionId;
    session_id_avp.flags = AvpFlag::kMandatory;
    session_id_avp.data = encode_octet_string(session_id);
    encode_avp(avps, session_id_avp);

    Avp cc_request_type_avp;
    cc_request_type_avp.code = dictionary::Dcc::kCcRequestType;
    cc_request_type_avp.flags = AvpFlag::kMandatory;
    cc_request_type_avp.data = encode_integer32(dictionary::Dcc::CcRequestType::kInitial);
    encode_avp(avps, cc_request_type_avp);

    Avp cc_request_number_avp;
    cc_request_number_avp.code = dictionary::Dcc::kCcRequestNumber;
    cc_request_number_avp.flags = AvpFlag::kMandatory;
    cc_request_number_avp.data = encode_unsigned32(0);
    encode_avp(avps, cc_request_number_avp);

    Header h;
    h.flags = CommandFlag::kRequest;
    h.command_code = dictionary::Command::kCreditControl;
    h.application_id = dictionary::Dcc::kApplicationId;
    h.hop_by_hop_id = 0x00C0FFEE;
    h.end_to_end_id = 0x00DECADE;
    auto msg = encode_header(h, static_cast<std::uint32_t>(avps.size()));
    msg.insert(msg.end(), avps.begin(), avps.end());
    return msg;
}

// A real RAA: Session-Id, Result-Code, Origin-Host, Origin-Realm -- the fake peer's own answer to
// CHF's RAR, echoing the RAR's Hop-by-Hop/End-to-End identifiers per RFC 6733 section 3.
std::vector<std::uint8_t> build_raa(std::uint32_t hop_by_hop_id,
                                    std::uint32_t end_to_end_id,
                                    const std::string& session_id,
                                    std::int32_t result_code) {
    using namespace diameter_core;
    std::vector<std::uint8_t> avps;
    Avp session_id_avp;
    session_id_avp.code = dictionary::Avp::kSessionId;
    session_id_avp.flags = AvpFlag::kMandatory;
    session_id_avp.data = encode_octet_string(session_id);
    encode_avp(avps, session_id_avp);

    Avp result_code_avp;
    result_code_avp.code = dictionary::Avp::kResultCode;
    result_code_avp.flags = AvpFlag::kMandatory;
    result_code_avp.data = encode_integer32(result_code);
    encode_avp(avps, result_code_avp);

    Avp origin_host_avp;
    origin_host_avp.code = dictionary::Avp::kOriginHost;
    origin_host_avp.flags = AvpFlag::kMandatory;
    origin_host_avp.data = encode_octet_string("test-peer.example.com");
    encode_avp(avps, origin_host_avp);

    Avp origin_realm_avp;
    origin_realm_avp.code = dictionary::Avp::kOriginRealm;
    origin_realm_avp.flags = AvpFlag::kMandatory;
    origin_realm_avp.data = encode_octet_string("example.com");
    encode_avp(avps, origin_realm_avp);

    Header h;
    h.flags = CommandFlag::kProxiable; // Answer: R-bit clear.
    h.command_code = dictionary::Command::kReAuth;
    h.application_id = dictionary::Dcc::kApplicationId;
    h.hop_by_hop_id = hop_by_hop_id;
    h.end_to_end_id = end_to_end_id;
    auto msg = encode_header(h, static_cast<std::uint32_t>(avps.size()));
    msg.insert(msg.end(), avps.begin(), avps.end());
    return msg;
}

} // namespace

TEST(ChfDiameterRar, RealRarRaaRoundTripViaAdminEndpoint) {
    ::setenv("CHF_CDR_DIRECT_INSERT", "false", 1);
    ::setenv("CHF_CDR_EVENT_BUS_BROKERS", "127.0.0.1:19999", 1);
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess chf(CHF_PATH);
    ::unsetenv("CHF_CDR_DIRECT_INSERT");
    ::unsetenv("CHF_CDR_EVENT_BUS_BROKERS");
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(chf.pid(), 0);

    // CI TSan failure 2026-10-04 (run 37220853366): this test's own
    // CHF_CDR_EVENT_BUS_BROKERS=127.0.0.1:19999 points at a broker that is never actually
    // started here -- librdkafka's background connect/retry threads compete for scheduler time
    // with CHF's own startup under TSan's heavy instrumentation, which the default 10s budget
    // (100 * 100ms) isn't always enough for. A real, explainable slow path, not a hang: CHF does
    // come up, just not inside the default window on this specific combination. 400 attempts
    // (40s) only on these two calls, not the harness default, so a genuine startup hang elsewhere
    // still fails fast.
    ASSERT_TRUE(nf_test::wait_tcp_listening(diameter_core::kDiameterTcpPort, 400))
        << "chf's Diameter port never opened";
    ASSERT_TRUE(nf_test::wait_tcp_listening(7784, 400)) << "chf's SBI port never opened";

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock(ioc);
    boost::system::error_code ec;
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), diameter_core::kDiameterTcpPort}, ec);
    ASSERT_FALSE(ec) << ec.message();

    // Real CER/CEA handshake.
    const auto cer = build_cer();
    boost::asio::write(sock, boost::asio::buffer(cer), ec);
    ASSERT_FALSE(ec) << ec.message();
    std::vector<std::uint8_t> cea_header_bytes(20);
    boost::asio::read(sock, boost::asio::buffer(cea_header_bytes), ec);
    ASSERT_FALSE(ec) << "no CEA: " << ec.message();
    std::size_t offset = 0;
    std::uint32_t cea_avps_length = 0;
    const auto cea_header = diameter_core::decode_header(cea_header_bytes, offset, cea_avps_length);
    ASSERT_TRUE(cea_header.has_value());
    EXPECT_EQ(cea_header->command_code, diameter_core::dictionary::Command::kCapabilitiesExchange);
    std::vector<std::uint8_t> cea_avps_bytes(cea_avps_length);
    boost::asio::read(sock, boost::asio::buffer(cea_avps_bytes), ec);
    ASSERT_FALSE(ec) << ec.message();

    // Real CCR-Initial/CCA -- establishes and registers the Gy session.
    const std::string session_id = "test-peer.example.com;rar-e2e;1";
    const auto ccr = build_real_ccr_initial(session_id);
    boost::asio::write(sock, boost::asio::buffer(ccr), ec);
    ASSERT_FALSE(ec) << ec.message();

    std::vector<std::uint8_t> cca_header_bytes(20);
    boost::asio::read(sock, boost::asio::buffer(cca_header_bytes), ec);
    ASSERT_FALSE(ec) << "no CCA: " << ec.message();
    offset = 0;
    std::uint32_t cca_avps_length = 0;
    const auto cca_header = diameter_core::decode_header(cca_header_bytes, offset, cca_avps_length);
    ASSERT_TRUE(cca_header.has_value());
    EXPECT_EQ(cca_header->command_code, diameter_core::dictionary::Command::kCreditControl);
    std::vector<std::uint8_t> cca_avps_bytes(cca_avps_length);
    boost::asio::read(sock, boost::asio::buffer(cca_avps_bytes), ec);
    ASSERT_FALSE(ec) << ec.message();
    const auto cca_avps = diameter_core::decode_avps(cca_avps_bytes);
    ASSERT_TRUE(cca_avps.has_value());
    const auto* cca_result_code =
        diameter_core::find_avp(*cca_avps, diameter_core::dictionary::Avp::kResultCode);
    ASSERT_NE(cca_result_code, nullptr);
    EXPECT_EQ(diameter_core::decode_integer32(cca_result_code->data),
              diameter_core::dictionary::ResultCode::kDiameterSuccess)
        << "real CCR-Initial must succeed for the session to be registered for RAR";

    // Background: read the RAR CHF pushes once the admin endpoint below is called, and answer it
    // with a real RAA -- on the SAME connection, exactly as a real PCEF would.
    std::optional<diameter_core::Header> rar_header;
    std::vector<std::uint8_t> rar_avps_bytes;
    std::thread peer_thread([&]() {
        std::vector<std::uint8_t> rar_header_bytes(20);
        boost::system::error_code rec_ec;
        boost::asio::read(sock, boost::asio::buffer(rar_header_bytes), rec_ec);
        if (rec_ec) {
            ADD_FAILURE() << "no RAR arrived: " << rec_ec.message();
            return;
        }
        std::size_t rar_offset = 0;
        std::uint32_t rar_avps_length = 0;
        rar_header = diameter_core::decode_header(rar_header_bytes, rar_offset, rar_avps_length);
        if (!rar_header.has_value()) {
            ADD_FAILURE() << "malformed RAR header";
            return;
        }
        EXPECT_EQ(rar_header->command_code, diameter_core::dictionary::Command::kReAuth);
        EXPECT_NE(rar_header->flags & diameter_core::CommandFlag::kRequest, 0)
            << "RAR must be a real Request (R-bit set)";

        rar_avps_bytes.resize(rar_avps_length);
        if (rar_avps_length > 0) {
            boost::asio::read(sock, boost::asio::buffer(rar_avps_bytes), rec_ec);
            if (rec_ec) {
                ADD_FAILURE() << "RAR AVPs never arrived: " << rec_ec.message();
                return;
            }
        }
        const auto rar_avps = diameter_core::decode_avps(rar_avps_bytes);
        if (!rar_avps.has_value()) {
            ADD_FAILURE() << "malformed RAR AVPs";
            return;
        }
        const auto* rar_session_id_avp =
            diameter_core::find_avp(*rar_avps, diameter_core::dictionary::Avp::kSessionId);
        ASSERT_NE(rar_session_id_avp, nullptr);
        EXPECT_EQ(diameter_core::decode_octet_string(rar_session_id_avp->data), session_id)
            << "RAR must carry the real session CHF was asked to reauthorize, not a fabricated one";

        const auto raa = build_raa(rar_header->hop_by_hop_id,
                                   rar_header->end_to_end_id,
                                   session_id,
                                   diameter_core::dictionary::ResultCode::kDiameterSuccess);
        boost::asio::write(sock, boost::asio::buffer(raa), rec_ec);
        if (rec_ec) {
            ADD_FAILURE() << "failed to write RAA: " << rec_ec.message();
        }
    });

    // Foreground: the real trigger -- an operator (or, eventually, a reconciliation sweep) calling
    // the new admin endpoint, blocking for the full RAR/RAA round trip CHF performs underneath it.
    auto client = make_mtls_client();
    const auto token = fetch_token(client, "chf-admin");
    ASSERT_FALSE(token.empty()) << "failed to obtain an NRF-issued token";

    sbi_core::http2::ClientRequest reauth_req;
    reauth_req.method = "POST";
    reauth_req.url = "https://127.0.0.1:7784/chf-admin/v1/sessions/" + session_id + "/reauthorize";
    reauth_req.headers.emplace("authorization", "Bearer " + token);
    const auto reauth_resp = client.send(reauth_req);
    ASSERT_TRUE(reauth_resp.has_value());

    peer_thread.join();

    ASSERT_EQ(reauth_resp->status, 200) << reauth_resp->body;
    const auto body = json::parse(reauth_resp->body);
    EXPECT_EQ(body.at("sessionId").get<std::string>(), session_id);
    EXPECT_EQ(body.at("resultCode").get<int>(),
              diameter_core::dictionary::ResultCode::kDiameterSuccess);
}

TEST(ChfDiameterRar, UnknownSessionReturns504) {
    ::setenv("CHF_CDR_DIRECT_INSERT", "false", 1);
    ::setenv("CHF_CDR_EVENT_BUS_BROKERS", "127.0.0.1:19999", 1);
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess chf(CHF_PATH);
    ::unsetenv("CHF_CDR_DIRECT_INSERT");
    ::unsetenv("CHF_CDR_EVENT_BUS_BROKERS");
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(chf.pid(), 0);
    // Same TSan-under-event-bus-mode slow start as RealRarRaaRoundTripViaAdminEndpoint above --
    // same rationale, same 400-attempt (40s) budget just for this call.
    ASSERT_TRUE(nf_test::wait_tcp_listening(7784, 400)) << "chf's SBI port never opened";

    // No Diameter connection was ever made for this session -- send_reauth_request's own
    // documented "no live connection holds this session" outcome (diameter_server.cpp:791-793).
    auto client = make_mtls_client();
    const auto token = fetch_token(client, "chf-admin");
    ASSERT_FALSE(token.empty()) << "failed to obtain an NRF-issued token";

    sbi_core::http2::ClientRequest reauth_req;
    reauth_req.method = "POST";
    reauth_req.url = "https://127.0.0.1:7784/chf-admin/v1/sessions/no-such-session;1;1/reauthorize";
    reauth_req.headers.emplace("authorization", "Bearer " + token);
    const auto reauth_resp = client.send(reauth_req);
    ASSERT_TRUE(reauth_resp.has_value());
    EXPECT_EQ(reauth_resp->status, 504);
}
