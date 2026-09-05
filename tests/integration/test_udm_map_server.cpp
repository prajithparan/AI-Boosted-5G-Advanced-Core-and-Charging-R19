// ADR-0299: UDM answering VLR-initiated MAP, over a real SCTP association.
//
// The direction analysis that produced this test is the point of it. `updateLocation`,
// `sendAuthenticationInfo` and `purgeMS` run VLR -> HLR, so no amount of client-side encoding
// could ever have exercised them -- they needed a server. This test IS a VLR: it opens the
// association, does the real M3UA activation, sends a real TC-Begin carrying a real Invoke, and
// checks what comes back.
//
// It asserts three things, deliberately including the negative one:
//   * updateLocation is answered with a ReturnResultLast carrying this UDM's own hlr-Number;
//   * the subscriber really is recorded, checked over the SBI side rather than by trusting the
//     MAP answer -- a server that replies correctly and stores nothing would pass a weaker test;
//   * an unimplemented opcode comes back as a real ReturnError, not silence. A VLR that gets no
//     answer cannot tell "unsupported" from "dead", and waits out its invoke timer either way.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <chrono>
#include <string>
#include <thread>

#include "map_core/map_dictionary.hpp"
#include "map_core/map_operations.hpp"
#include "spawn_guard.hpp"
#include "ss7_core/m3ua_asp.hpp"
#include "ss7_core/m3ua_dictionary.hpp"
#include "ss7_core/m3ua_header.hpp"
#include "ss7_core/m3ua_protocol_data.hpp"
#include "ss7_core/m3ua_tlv.hpp"
#include "ss7_core/sccp_dictionary.hpp"
#include "ss7_core/sccp_udt.hpp"
#include "ss7_core/sctp_socket.hpp"
#include "tbcd_core/tbcd.hpp"
#include "tcap_core/component.hpp"
#include "tcap_core/dialogue_portion.hpp"
#include "tcap_core/message.hpp"

#include <gtest/gtest.h>

namespace {

using nlohmann::json;
using namespace std::chrono_literals;

constexpr std::uint16_t kMapServerPort = 12906;
constexpr const char* kImsiDigits = "999700000000902";
constexpr const char* kHlrNumber = "99970000001";

void send_m3ua(ss7_core::SctpSocket& sock,
               std::uint8_t cls,
               std::uint8_t type,
               const std::vector<std::uint8_t>& body) {
    auto msg = ss7_core::encode_m3ua_header({cls, type}, static_cast<std::uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    sock.send(msg);
}

// Sends one MAP Invoke inside a real TC-Begin and returns the decoded component of the TC-End.
std::optional<tcap_core::Component> run_dialogue(ss7_core::SctpSocket& sock,
                                                 std::int32_t opcode,
                                                 const std::vector<std::uint32_t>& context,
                                                 const std::vector<std::uint8_t>& parameter) {
    tcap_core::Invoke invoke;
    invoke.invoke_id = 1;
    invoke.operation_code.local = opcode;
    invoke.parameter = parameter;

    tcap_core::DialogueRequest aarq;
    aarq.application_context_name = context;
    tcap_core::TcBegin begin;
    begin.originating_transaction_id = {0x00, 0x00, 0x00, 0x11};
    begin.dialogue_portion = tcap_core::encode_dialogue_portion_request(aarq);
    begin.components.push_back(tcap_core::encode_invoke(invoke));

    ss7_core::SccpUdt udt;
    udt.protocol_class = ss7_core::dictionary::ProtocolClass::kClass0;
    udt.called_party.ssn_present = true;
    udt.called_party.ssn = ss7_core::dictionary::SubsystemNumber::kHlr;
    udt.calling_party.ssn_present = true;
    udt.calling_party.ssn = ss7_core::dictionary::SubsystemNumber::kVlr;
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
    std::vector<std::uint8_t> body;
    ss7_core::encode_m3ua_tlv(body, pd_tlv);
    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kTransfer,
              ss7_core::dictionary::TransferMessageType::kData,
              body);

    const auto bytes = sock.receive();
    if (bytes.empty()) {
        return std::nullopt;
    }
    std::size_t offset = 0;
    std::uint32_t payload_length = 0;
    const auto header = ss7_core::decode_m3ua_header(bytes, offset, payload_length);
    if (!header.has_value()) {
        return std::nullopt;
    }
    std::vector<std::uint8_t> payload(bytes.begin() + static_cast<std::ptrdiff_t>(offset),
                                      bytes.end());
    const auto tlvs = ss7_core::decode_m3ua_tlvs(payload);
    if (!tlvs.has_value()) {
        return std::nullopt;
    }
    const auto* resp_pd =
        ss7_core::find_m3ua_tlv(*tlvs, ss7_core::dictionary::ParamTag::kProtocolData);
    if (resp_pd == nullptr) {
        return std::nullopt;
    }
    const auto proto = ss7_core::decode_m3ua_protocol_data(resp_pd->value);
    if (!proto.has_value()) {
        return std::nullopt;
    }
    const auto resp_udt = ss7_core::decode_sccp_udt(proto->user_protocol_data);
    if (!resp_udt.has_value()) {
        return std::nullopt;
    }
    const auto end = tcap_core::decode_tc_end(resp_udt->data);
    if (!end.has_value() || end->components.empty()) {
        return std::nullopt;
    }
    return tcap_core::decode_component(end->components.front());
}

bool activate(ss7_core::SctpSocket& sock) {
    send_m3ua(
        sock,
        ss7_core::dictionary::MessageClass::kAspsm,
        ss7_core::dictionary::AspsmMessageType::kAspUp,
        ss7_core::encode_asp_state_message(ss7_core::dictionary::AspsmMessageType::kAspUp, {}));
    if (sock.receive().empty()) {
        return false;
    }
    ss7_core::AspTrafficMessage active;
    active.traffic_mode_type = ss7_core::dictionary::TrafficModeType::kOverride;
    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kAsptm,
              ss7_core::dictionary::AsptmMessageType::kAspActive,
              ss7_core::encode_asp_traffic_message(
                  ss7_core::dictionary::AsptmMessageType::kAspActive, active));
    return !sock.receive().empty();
}

} // namespace

TEST(UdmMapServer, UpdateLocationIsAnsweredAndPurgeMsDropsTheRecord) {
    ::setenv("UDM_MAP_SERVER_PORT", std::to_string(kMapServerPort).c_str(), 1);
    ::setenv("UDM_HLR_NUMBER", kHlrNumber, 1);
    nf_test::SpawnedProcess nrf(NRF_PATH);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ::unsetenv("UDM_MAP_SERVER_PORT");
    ::unsetenv("UDM_HLR_NUMBER");
    ASSERT_GT(nrf.pid(), 0);
    ASSERT_GT(udm.pid(), 0);

    // The MAP listener binds during startup; retry the association until it is up.
    ss7_core::SctpSocket sock;
    bool connected = false;
    for (int attempt = 0; attempt < 100 && !connected; ++attempt) {
        try {
            ss7_core::SctpSocket fresh;
            fresh.connect("127.0.0.1", kMapServerPort);
            sock = std::move(fresh);
            connected = true;
        } catch (const std::exception&) {
            std::this_thread::sleep_for(100ms);
        }
    }
    ASSERT_TRUE(connected) << "udm's MAP server never accepted an association";
    ASSERT_TRUE(activate(sock)) << "M3UA activation failed";

    // --- updateLocation ---
    map_core::UpdateLocationArg ul;
    ul.imsi = tbcd_core::encode_tbcd(kImsiDigits);
    ul.msc_number = tbcd_core::encode_tbcd("99970000002");
    ul.vlr_number = tbcd_core::encode_tbcd("99970000003");
    const auto ul_comp = run_dialogue(sock,
                                      map_core::Opcode::kUpdateLocation,
                                      map_core::kNetworkLocUpContextV3Oid,
                                      map_core::encode_update_location_arg(ul));
    ASSERT_TRUE(ul_comp.has_value()) << "no decodable answer to updateLocation";
    ASSERT_TRUE(ul_comp->return_result_last.has_value())
        << "updateLocation must be answered with a ReturnResultLast";
    ASSERT_TRUE(ul_comp->return_result_last->result.has_value());
    const auto hlr =
        map_core::decode_update_location_res(ul_comp->return_result_last->result->parameter);
    ASSERT_TRUE(hlr.has_value()) << "UpdateLocationRes carried no decodable hlr-Number";
    EXPECT_EQ(tbcd_core::decode_tbcd(*hlr), kHlrNumber)
        << "UDM answered with an hlr-Number that is not its own configured one";

    // The subscriber must really be recorded -- checked over SBI, not by trusting the MAP answer.
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client client(std::move(tls));
    const std::string reg_url = "https://127.0.0.1:7780/nudm-uecm/v1/imsi-" +
                                std::string(kImsiDigits) + "/registrations/amf-3gpp-access";
    sbi_core::http2::ClientRequest get;
    get.method = "GET";
    get.url = reg_url;
    auto get_resp = client.send(get);
    ASSERT_TRUE(get_resp.has_value());
    EXPECT_EQ(get_resp->status, 200)
        << "the MAP updateLocation did not actually store a serving-node record";
    if (get_resp->status == 200) {
        const auto stored = json::parse(get_resp->body);
        EXPECT_EQ(stored.value("vlrNumber", ""), "99970000003");
    }

    // --- an unimplemented opcode must be a real ReturnError, never silence ---
    const auto sai_comp = run_dialogue(sock,
                                       map_core::Opcode::kSendAuthenticationInfo,
                                       map_core::kNetworkLocUpContextV3Oid,
                                       std::vector<std::uint8_t>{});
    ASSERT_TRUE(sai_comp.has_value())
        << "sendAuthenticationInfo got NO answer -- a VLR cannot tell that from a dead HLR";
    EXPECT_TRUE(sai_comp->return_error.has_value())
        << "an unimplemented operation must come back as a ReturnError";

    // --- purgeMS drops the record ---
    map_core::PurgeMsArg purge;
    purge.imsi = tbcd_core::encode_tbcd(kImsiDigits);
    purge.vlr_number = tbcd_core::encode_tbcd("99970000003");
    const auto purge_comp = run_dialogue(sock,
                                         map_core::Opcode::kPurgeMs,
                                         map_core::kMsPurgingContextV3Oid,
                                         map_core::encode_purge_ms_arg(purge));
    ASSERT_TRUE(purge_comp.has_value()) << "no decodable answer to purgeMS";
    EXPECT_TRUE(purge_comp->return_result_last.has_value());

    auto after = client.send(get);
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->status, 404) << "purgeMS answered but did not drop the serving-node record";
}
