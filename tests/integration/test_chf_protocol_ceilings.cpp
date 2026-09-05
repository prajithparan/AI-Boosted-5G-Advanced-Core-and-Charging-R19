// ADR-0295: end-to-end tests for the Diameter and SS7/M3UA TPS ceilings.
//
// ADR-0290 closed a real data race in these two ceilings and, in the same breath, recorded that
// neither had an end-to-end test -- `docs/COMPLIANCE_P1_P15.md`'s P15 row and blocker 5 have said
// so ever since. Only the SBI ceiling had one. That is the gap this file closes.
//
// The two protocols are asserted differently on purpose, because they SHED differently:
//
//   - Diameter ANSWERS. RFC 6733's DIAMETER_TOO_BUSY (3004) is a real transient-failure code a
//     peer backs off on, so the assertion is the answer itself: command code echoed, Result-Code
//     3004. Nothing else in CHF produces that code, so this cannot pass by accident.
//   - SS7/M3UA DROPS. ADR-0288's deliberate choice: a TCAP abort costs nearly what serving the
//     message would, so the peer's own invoke timer recovers the dialogue instead. That means
//     "no reply arrived" is ALSO what a malformed message produces, and an assertion on silence
//     would prove nothing. So this test reads CHF's own log for the specific line the shed path
//     emits -- the same evidence standard ADR-0269/ADR-0270 used against AMF's and UPF's logs,
//     now automated rather than done by hand (`SpawnedProcess`'s new `log_path`).
//
// Both ceilings are configured through the environment (`CHF_DIAMETER_MAX_TPS` etc., ADR-0295), so
// no test value lives in checked-in config. Burst is set BELOW one token, which makes the very
// first request after the handshake shed deterministically -- no racing against a refill, and no
// dependence on the charging engine's backends being up, since both ceilings sit in front of
// dispatch.

#include <boost/asio/connect.hpp>
#include <boost/asio/io_context.hpp>
#include <boost/asio/ip/tcp.hpp>
#include <boost/asio/read.hpp>
#include <boost/asio/write.hpp>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#include "cap_core/cap_dictionary.hpp"
#include "cap_core/cap_operations.hpp"
#include "diameter_core/avp.hpp"
#include "diameter_core/dictionary.hpp"
#include "diameter_core/header.hpp"
#include "spawn_guard.hpp"
#include "ss7_core/m3ua_asp.hpp"
#include "ss7_core/m3ua_dictionary.hpp"
#include "ss7_core/m3ua_header.hpp"
#include "ss7_core/m3ua_protocol_data.hpp"
#include "ss7_core/m3ua_tlv.hpp"
#include "ss7_core/sccp_dictionary.hpp"
#include "ss7_core/sccp_udt.hpp"
#include "ss7_core/sctp_socket.hpp"
#include "tcap_core/component.hpp"
#include "tcap_core/dialogue_portion.hpp"
#include "tcap_core/message.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

// A burst below one token: TokenBucket starts full at `capacity`, and try_acquire needs a whole
// token, so the FIRST request is shed. Deterministic, with no sleep and no refill race.
constexpr const char* kBelowOneToken = "0.25";
constexpr const char* kEssentiallyZeroTps = "0.001";

std::string log_file(const char* stem) {
    return (std::filesystem::temp_directory_path() /
            (std::string(stem) + "-" + std::to_string(::getpid()) + ".log"))
        .string();
}

bool wait_for_line(const std::string& path, const std::string& needle, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        std::ifstream in(path);
        std::string contents((std::istreambuf_iterator<char>(in)),
                             std::istreambuf_iterator<char>());
        if (contents.find(needle) != std::string::npos) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
}

bool wait_for_tcp(std::uint16_t port, std::chrono::seconds limit) {
    const auto deadline = std::chrono::steady_clock::now() + limit;
    while (std::chrono::steady_clock::now() < deadline) {
        boost::asio::io_context ioc;
        boost::asio::ip::tcp::socket sock(ioc);
        boost::system::error_code ec;
        sock.connect({boost::asio::ip::make_address("127.0.0.1"), port}, ec);
        if (!ec) {
            return true;
        }
        std::this_thread::sleep_for(50ms);
    }
    return false;
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

// A CCR-I carrying only Session-Id. That is deliberately less than a servable CCR: the ceiling is
// checked BEFORE the AVPs are decoded, so a shed must happen regardless of the body -- and if the
// ceiling were not working, this message would fail some other way rather than returning 3004,
// which is exactly the discrimination the test needs.
std::vector<std::uint8_t> build_ccr_initial() {
    using namespace diameter_core;
    std::vector<std::uint8_t> avps;
    Avp session_id;
    session_id.code = dictionary::Avp::kSessionId;
    session_id.flags = AvpFlag::kMandatory;
    session_id.data = encode_octet_string("test-peer.example.com;1;1");
    encode_avp(avps, session_id);

    Header h;
    h.flags = CommandFlag::kRequest;
    h.command_code = dictionary::Command::kCreditControl;
    h.application_id = 4; // Diameter Credit-Control Application
    h.hop_by_hop_id = 0x00C0FFEE;
    h.end_to_end_id = 0x00DECADE;
    auto msg = encode_header(h, static_cast<std::uint32_t>(avps.size()));
    msg.insert(msg.end(), avps.begin(), avps.end());
    return msg;
}

void send_m3ua(ss7_core::SctpSocket& sock,
               std::uint8_t cls,
               std::uint8_t type,
               const std::vector<std::uint8_t>& body) {
    auto msg = ss7_core::encode_m3ua_header({cls, type}, static_cast<std::uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    sock.send(msg);
}

// The real InitialDP CHF would answer with RequestReportBCSMEvent+ApplyCharging, wrapped through
// TCAP -> SCCP -> M3UA exactly as a real gsmSSF would send it.
std::vector<std::uint8_t> build_initial_dp_data() {
    cap_core::InitialDpArg idp;
    idp.service_key = 1;
    idp.called_party_number = {0x00, 0x11, 0x22};
    idp.event_type_bcsm = 12; // collectedInfo
    tcap_core::Invoke invoke;
    invoke.invoke_id = 1;
    invoke.operation_code.local = cap_core::Opcode::kInitialDp;
    invoke.parameter = cap_core::encode_initial_dp_arg(idp);

    tcap_core::DialogueRequest aarq;
    aarq.application_context_name = cap_core::kGsmssfScfGenericAcOid;
    tcap_core::TcBegin begin;
    begin.originating_transaction_id = {0x00, 0x00, 0x00, 0x07};
    begin.dialogue_portion = tcap_core::encode_dialogue_portion_request(aarq);
    begin.components.push_back(tcap_core::encode_invoke(invoke));

    ss7_core::SccpUdt udt;
    udt.protocol_class = ss7_core::dictionary::ProtocolClass::kClass0;
    udt.called_party.ssn_present = true;
    udt.called_party.ssn = ss7_core::dictionary::SubsystemNumber::kHlr;
    udt.calling_party.ssn_present = true;
    udt.calling_party.ssn = ss7_core::dictionary::SubsystemNumber::kMsc;
    udt.data = tcap_core::encode_tc_begin(begin);

    ss7_core::M3uaProtocolData pd;
    pd.opc = 1;
    pd.dpc = 2;
    pd.si = ss7_core::dictionary::ServiceIndicator::kSccp;
    pd.ni = 2;
    pd.sls = 0;
    pd.user_protocol_data = ss7_core::encode_sccp_udt(udt);
    ss7_core::M3uaTlv pd_tlv;
    pd_tlv.tag = ss7_core::dictionary::ParamTag::kProtocolData;
    pd_tlv.value = ss7_core::encode_m3ua_protocol_data(pd);
    std::vector<std::uint8_t> out;
    ss7_core::encode_m3ua_tlv(out, pd_tlv);
    return out;
}

} // namespace

TEST(ChfProtocolCeilings, DiameterAnswersTooBusyWhenTheCeilingIsExceeded) {
    const std::string chf_log = log_file("chf-diameter-ceiling");
    ::setenv("CHF_DIAMETER_MAX_TPS", kEssentiallyZeroTps, 1);
    ::setenv("CHF_DIAMETER_TPS_BURST", kBelowOneToken, 1);
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess chf(CHF_PATH, chf_log.c_str());
    ::unsetenv("CHF_DIAMETER_MAX_TPS");
    ::unsetenv("CHF_DIAMETER_TPS_BURST");
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(chf.pid(), 0);

    ASSERT_TRUE(wait_for_tcp(diameter_core::kDiameterTcpPort, 20s))
        << "chf's Diameter port never opened";
    ASSERT_TRUE(wait_for_line(chf_log, "Diameter TPS ceiling active", 20s))
        << "chf did not apply the ceiling from the environment -- the rest of this test would "
           "prove nothing";

    boost::asio::io_context ioc;
    boost::asio::ip::tcp::socket sock(ioc);
    boost::system::error_code ec;
    sock.connect({boost::asio::ip::make_address("127.0.0.1"), diameter_core::kDiameterTcpPort}, ec);
    ASSERT_FALSE(ec) << ec.message();

    // Real CER/CEA first: the ceiling lives in the post-handshake loop, so the handshake itself
    // must still succeed. Asserting that here also rules out "the whole connection is broken" as
    // an explanation for whatever the next message returns.
    const auto cer = build_cer();
    boost::asio::write(sock, boost::asio::buffer(cer), ec);
    ASSERT_FALSE(ec) << ec.message();

    std::vector<std::uint8_t> cea_header(20);
    boost::asio::read(sock, boost::asio::buffer(cea_header), ec);
    ASSERT_FALSE(ec) << "no CEA: " << ec.message();
    std::size_t offset = 0;
    std::uint32_t cea_avps_length = 0;
    const auto cea = diameter_core::decode_header(cea_header, offset, cea_avps_length);
    ASSERT_TRUE(cea.has_value());
    EXPECT_EQ(cea->command_code, diameter_core::dictionary::Command::kCapabilitiesExchange);
    std::vector<std::uint8_t> cea_avps(cea_avps_length);
    boost::asio::read(sock, boost::asio::buffer(cea_avps), ec);
    ASSERT_FALSE(ec) << ec.message();

    const auto ccr = build_ccr_initial();
    boost::asio::write(sock, boost::asio::buffer(ccr), ec);
    ASSERT_FALSE(ec) << ec.message();

    std::vector<std::uint8_t> answer_header(20);
    boost::asio::read(sock, boost::asio::buffer(answer_header), ec);
    ASSERT_FALSE(ec) << "the ceiling dropped the connection instead of answering: " << ec.message();
    offset = 0;
    std::uint32_t answer_avps_length = 0;
    const auto answer = diameter_core::decode_header(answer_header, offset, answer_avps_length);
    ASSERT_TRUE(answer.has_value());
    EXPECT_EQ(answer->command_code, diameter_core::dictionary::Command::kCreditControl)
        << "a shed answer must echo the request's command code";
    EXPECT_EQ(answer->hop_by_hop_id, 0x00C0FFEEu) << "hop-by-hop id must be echoed";

    std::vector<std::uint8_t> answer_avps_bytes(answer_avps_length);
    boost::asio::read(sock, boost::asio::buffer(answer_avps_bytes), ec);
    ASSERT_FALSE(ec) << ec.message();
    const auto answer_avps = diameter_core::decode_avps(answer_avps_bytes);
    ASSERT_TRUE(answer_avps.has_value());
    const auto* result_code =
        diameter_core::find_avp(*answer_avps, diameter_core::dictionary::Avp::kResultCode);
    ASSERT_NE(result_code, nullptr) << "the shed answer carried no Result-Code";
    const auto value = diameter_core::decode_integer32(result_code->data);
    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, diameter_core::dictionary::ResultCode::kDiameterTooBusy)
        << "overload must be signalled as the transient DIAMETER_TOO_BUSY (3004), not a permanent "
           "failure a peer would stop retrying on";

    std::error_code rm;
    std::filesystem::remove(chf_log, rm);
}

TEST(ChfProtocolCeilings, Ss7MessageIsDroppedRatherThanAnsweredWhenTheCeilingIsExceeded) {
    const std::string chf_log = log_file("chf-cap-ceiling");
    ::setenv("CHF_CAP_MAX_TPS", kEssentiallyZeroTps, 1);
    ::setenv("CHF_CAP_TPS_BURST", kBelowOneToken, 1);
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess chf(CHF_PATH, chf_log.c_str());
    ::unsetenv("CHF_CAP_MAX_TPS");
    ::unsetenv("CHF_CAP_TPS_BURST");
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(chf.pid(), 0);

    ASSERT_TRUE(wait_for_line(chf_log, "CAP/SS7 TPS ceiling active", 20s))
        << "chf did not apply the CAP ceiling from the environment";
    ASSERT_TRUE(wait_for_line(chf_log, "CAP (gsmSCF) listening", 20s));

    ss7_core::SctpSocket sock;
    sock.connect("127.0.0.1", ss7_core::dictionary::kSctpPort);

    // Real M3UA activation, initiator role (RFC 4666 §3.5/§3.7) -- the ceiling sits after it.
    send_m3ua(
        sock,
        ss7_core::dictionary::MessageClass::kAspsm,
        ss7_core::dictionary::AspsmMessageType::kAspUp,
        ss7_core::encode_asp_state_message(ss7_core::dictionary::AspsmMessageType::kAspUp, {}));
    ASSERT_FALSE(sock.receive().empty()) << "no ASP Up Ack";
    ss7_core::AspTrafficMessage active;
    active.traffic_mode_type = ss7_core::dictionary::TrafficModeType::kOverride;
    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kAsptm,
              ss7_core::dictionary::AsptmMessageType::kAspActive,
              ss7_core::encode_asp_traffic_message(
                  ss7_core::dictionary::AsptmMessageType::kAspActive, active));
    ASSERT_FALSE(sock.receive().empty()) << "no ASP Active Ack";

    // A real InitialDP. What makes the drop attributable to the ceiling is the control case
    // below (`Ss7InitialDpIsServedWhenNoCeilingIsConfigured`): with no ceiling, this same message
    // reaches CHF's CAP handler. It does not get as far as a reply there either -- it carries no
    // IMSI, and CHF says so in its log -- so the proven contrast is "dispatched vs. shed", not
    // "answered vs. silent". Stated at that strength and no higher.
    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kTransfer,
              ss7_core::dictionary::TransferMessageType::kData,
              build_initial_dp_data());

    // The log line is the assertion, for the reason this file's header gives: a drop is silent by
    // design, so silence alone would be indistinguishable from the message being ignored for some
    // other reason.
    EXPECT_TRUE(wait_for_line(chf_log, "CAP message dropped at the configured TPS ceiling", 10s))
        << "the InitialDP was not shed -- either the ceiling did not apply to it, or it was "
           "swallowed somewhere else, and either way P15's SS7 claim is unproven";

    std::error_code rm;
    std::filesystem::remove(chf_log, rm);
}

// The control for the test above, and the reason its attribution claim is allowed to stand at all.
// Without this case, "the message was dropped" would be unattributable: CHF's InitialDP path calls
// the charging engine, which needs bss/product-catalog and bss/balance-management, and neither is
// spawned here -- so silence could equally mean the message was simply unservable. Same two
// processes, same message, no ceiling: the message reaches the CAP handler. That is the contrast,
// and it is deliberately not stated as "would have been answered", which this does not show.
TEST(ChfProtocolCeilings, Ss7InitialDpIsServedWhenNoCeilingIsConfigured) {
    const std::string chf_log = log_file("chf-cap-no-ceiling");
    // No CHF_CAP_MAX_TPS: config/chf.json's own value governs, and it is absent/disabled there.
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess chf(CHF_PATH, chf_log.c_str());
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(chf.pid(), 0);
    ASSERT_TRUE(wait_for_line(chf_log, "CAP (gsmSCF) listening", 20s));

    ss7_core::SctpSocket sock;
    sock.connect("127.0.0.1", ss7_core::dictionary::kSctpPort);
    send_m3ua(
        sock,
        ss7_core::dictionary::MessageClass::kAspsm,
        ss7_core::dictionary::AspsmMessageType::kAspUp,
        ss7_core::encode_asp_state_message(ss7_core::dictionary::AspsmMessageType::kAspUp, {}));
    ASSERT_FALSE(sock.receive().empty()) << "no ASP Up Ack";
    ss7_core::AspTrafficMessage active;
    active.traffic_mode_type = ss7_core::dictionary::TrafficModeType::kOverride;
    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kAsptm,
              ss7_core::dictionary::AsptmMessageType::kAspActive,
              ss7_core::encode_asp_traffic_message(
                  ss7_core::dictionary::AsptmMessageType::kAspActive, active));
    ASSERT_FALSE(sock.receive().empty()) << "no ASP Active Ack";

    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kTransfer,
              ss7_core::dictionary::TransferMessageType::kData,
              build_initial_dp_data());

    // CHF must at least REACH the InitialDP handler. Whether it can complete the charging decision
    // depends on backends this test does not run, so the assertion is that the message was
    // dispatched rather than shed -- which is precisely the contrast the drop test needs, and no
    // more than that.
    EXPECT_TRUE(wait_for_line(chf_log, "InitialDP", 10s))
        << "with no ceiling configured, the InitialDP never reached CHF's CAP handler -- so the "
           "drop test above is NOT attributable to the ceiling, and its comment claiming so must "
           "be removed rather than left standing";
    EXPECT_FALSE(wait_for_line(chf_log, "CAP message dropped at the configured TPS ceiling", 1s))
        << "a ceiling fired with none configured";

    std::error_code rm;
    std::filesystem::remove(chf_log, rm);
}
