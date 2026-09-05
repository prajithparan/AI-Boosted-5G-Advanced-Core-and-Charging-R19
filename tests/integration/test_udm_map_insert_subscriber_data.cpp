// ADR-0293: UDM's MAP client is reachable -- proved by a real M3UA/SCTP peer receiving what it
// sends.
//
// The client has existed since ADR-0061 and nothing ever called it: `grep map_client` over UDM's
// main.cpp returned nothing. The capability was real and unreachable, which by this project's own
// standard ("a server with no consumer is not done") meant MAP was not done.
//
// This test is a minimal VLR: it listens on SCTP, answers the M3UA ASPSM/ASPTM handshake, and
// asserts a real insertSubscriberData Invoke arrives carrying the TBCD-encoded IMSI of the UE that
// just registered over Nudm_UECM. It does NOT re-implement the MAP decode -- it uses the project's
// own map_core/tcap_core, the same libraries the client encodes with, so a change that broke the
// encoding would break this test rather than being mirrored by it.

#include "sbi_core/http2_client.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
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

constexpr std::uint16_t kVlrPort = 12905;
constexpr const char* kTestSupi = "imsi-999700000000901";

sbi_core::http2::Client make_client() {
    sbi_core::http2::TlsConfig tls{
        .cert_path = CERTS_DIR "/hello-nf/cert.pem",
        .key_path = CERTS_DIR "/hello-nf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    return sbi_core::http2::Client(std::move(tls));
}

bool wait_reachable(sbi_core::http2::Client& client, const std::string& url, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        sbi_core::http2::ClientRequest req;
        req.method = "GET";
        req.url = url;
        if (client.send(req).has_value()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    return false;
}

std::string fetch_token(sbi_core::http2::Client& client) {
    sbi_core::http2::ClientRequest req;
    req.method = "POST";
    req.url = "https://127.0.0.1:7777/oauth2/token";
    req.headers.emplace("content-type", "application/x-www-form-urlencoded");
    req.body = "grant_type=client_credentials&nfInstanceId=test-client&scope=nudm-uecm&"
               "targetNfType=UDM";
    auto resp = client.send(req);
    if (!resp.has_value() || resp->status != 200) {
        return "";
    }
    return json::parse(resp->body).at("access_token").get<std::string>();
}

} // namespace

TEST(UdmMapInsertSubscriberData, RegistrationPushesTheSubscriberToAVlrOverRealM3ua) {
    // The stand-in VLR, started before UDM so the association can be accepted immediately.
    ss7_core::SctpSocket listener;
    listener.bind_and_listen("127.0.0.1", kVlrPort);

    std::atomic<bool> received_imsi_matches{false};
    std::atomic<bool> got_invoke{false};
    std::atomic<bool> answered{false};
    std::thread vlr([&] {
        // Mirrors nfs/chf/src/cap_server.cpp's own receive_m3ua/send_m3ua/handshake helpers,
        // using the same ss7_core entry points -- so if the M3UA layer changes, this breaks rather
        // than silently agreeing with a stale copy.
        struct Received {
            ss7_core::M3uaHeader header;
            std::vector<std::uint8_t> payload;
        };
        auto receive = [](ss7_core::SctpSocket& sock) -> std::optional<Received> {
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
            return Received{*header, std::move(payload)};
        };
        auto send = [](ss7_core::SctpSocket& sock,
                       std::uint8_t cls,
                       std::uint8_t type,
                       const std::vector<std::uint8_t>& body) {
            auto msg =
                ss7_core::encode_m3ua_header({cls, type}, static_cast<std::uint32_t>(body.size()));
            msg.insert(msg.end(), body.begin(), body.end());
            sock.send(msg);
        };

        try {
            auto conn = listener.accept();

            const auto up = receive(conn);
            if (!up.has_value() ||
                up->header.message_class != ss7_core::dictionary::MessageClass::kAspsm ||
                up->header.message_type != ss7_core::dictionary::AspsmMessageType::kAspUp) {
                return;
            }
            send(conn,
                 ss7_core::dictionary::MessageClass::kAspsm,
                 ss7_core::dictionary::AspsmMessageType::kAspUpAck,
                 ss7_core::encode_asp_state_message(
                     ss7_core::dictionary::AspsmMessageType::kAspUpAck, {}));

            const auto active = receive(conn);
            if (!active.has_value() ||
                active->header.message_class != ss7_core::dictionary::MessageClass::kAsptm ||
                active->header.message_type != ss7_core::dictionary::AsptmMessageType::kAspActive) {
                return;
            }
            ss7_core::AspTrafficMessage ack;
            ack.traffic_mode_type = ss7_core::dictionary::TrafficModeType::kOverride;
            send(conn,
                 ss7_core::dictionary::MessageClass::kAsptm,
                 ss7_core::dictionary::AsptmMessageType::kAspActiveAck,
                 ss7_core::encode_asp_traffic_message(
                     ss7_core::dictionary::AsptmMessageType::kAspActiveAck, ack));

            const auto data = receive(conn);
            if (!data.has_value() ||
                data->header.message_class != ss7_core::dictionary::MessageClass::kTransfer ||
                data->header.message_type != ss7_core::dictionary::TransferMessageType::kData) {
                return;
            }
            const auto tlvs = ss7_core::decode_m3ua_tlvs(data->payload);
            if (!tlvs.has_value()) {
                return;
            }
            const auto* pd_tlv =
                ss7_core::find_m3ua_tlv(*tlvs, ss7_core::dictionary::ParamTag::kProtocolData);
            if (pd_tlv == nullptr) {
                return;
            }
            const auto proto_data = ss7_core::decode_m3ua_protocol_data(pd_tlv->value);
            if (!proto_data.has_value()) {
                return;
            }
            const auto udt = ss7_core::decode_sccp_udt(proto_data->user_protocol_data);
            if (!udt.has_value()) {
                return;
            }
            const auto begin = tcap_core::decode_tc_begin(udt->data);
            if (!begin.has_value() || begin->components.empty()) {
                return;
            }
            const auto comp = tcap_core::decode_component(begin->components.front());
            if (!comp.has_value() || !comp->invoke.has_value()) {
                return;
            }
            got_invoke = true;
            const auto arg = map_core::decode_insert_subscriber_data_arg(comp->invoke->parameter);
            if (arg.has_value() && arg->imsi.has_value()) {
                received_imsi_matches =
                    tbcd_core::decode_tbcd(*arg->imsi) == std::string(kTestSupi).substr(5);
            }

            // Answer the dialogue for real. Without this the client's whole response path --
            // AARE handling, TC-END decode, ReturnResultLast interpretation -- would never
            // execute, and that is exactly where a decode bug would hide. Mirrors
            // nfs/chf/src/cap_server.cpp's own reply construction (addresses swapped, same
            // application context the AARQ named rather than a different one).
            tcap_core::ReturnResultLast rrl;
            rrl.invoke_id = comp->invoke->invoke_id;
            rrl.result = tcap_core::ReturnResult::Result{
                comp->invoke->operation_code, map_core::encode_insert_subscriber_data_res()};

            tcap_core::TcEnd end;
            end.destination_transaction_id = begin->originating_transaction_id;
            if (begin->dialogue_portion.has_value()) {
                tcap_core::DialogueResponse aare;
                aare.application_context_name = map_core::kSubscriberDataMngtContextV3Oid;
                aare.result = tcap_core::ResultType::kAccepted;
                aare.diagnostic.is_user_type = true;
                aare.diagnostic.value = tcap_core::DialogServiceUserType::kNoReasonGiven;
                end.dialogue_portion = tcap_core::encode_dialogue_portion_response(aare);
            }
            end.components.push_back(tcap_core::encode_return_result(rrl, true));

            ss7_core::SccpUdt reply_udt;
            reply_udt.protocol_class = ss7_core::dictionary::ProtocolClass::kClass0;
            reply_udt.called_party.ssn_present = true;
            reply_udt.called_party.ssn = ss7_core::dictionary::SubsystemNumber::kHlr;
            reply_udt.calling_party.ssn_present = true;
            reply_udt.calling_party.ssn = ss7_core::dictionary::SubsystemNumber::kVlr;
            reply_udt.data = tcap_core::encode_tc_end(end);

            ss7_core::M3uaProtocolData reply_pd;
            reply_pd.opc = 2;
            reply_pd.dpc = 1;
            reply_pd.si = ss7_core::dictionary::ServiceIndicator::kSccp;
            reply_pd.ni = 2;
            reply_pd.sls = 0;
            reply_pd.user_protocol_data = ss7_core::encode_sccp_udt(reply_udt);

            ss7_core::M3uaTlv reply_tlv;
            reply_tlv.tag = ss7_core::dictionary::ParamTag::kProtocolData;
            reply_tlv.value = ss7_core::encode_m3ua_protocol_data(reply_pd);
            std::vector<std::uint8_t> reply_bytes;
            ss7_core::encode_m3ua_tlv(reply_bytes, reply_tlv);
            send(conn,
                 ss7_core::dictionary::MessageClass::kTransfer,
                 ss7_core::dictionary::TransferMessageType::kData,
                 reply_bytes);
            answered = true;
        } catch (const std::exception&) {
            // Flags stay false; the assertions report it.
        }
    });

    nf_test::SpawnedProcess nrf(NRF_PATH);
    ASSERT_GT(nrf.pid(), 0);
    // The VLR peer is configured through the environment so no checked-in config carries a test
    // value, the same pattern the TPS ceiling test uses.
    ::setenv("UDM_VLR_PEER_ADDRESS", "127.0.0.1", 1);
    ::setenv("UDM_VLR_PEER_PORT", std::to_string(kVlrPort).c_str(), 1);
    nf_test::SpawnedProcess udm(UDM_PATH);
    ASSERT_GT(udm.pid(), 0);
    ::unsetenv("UDM_VLR_PEER_ADDRESS");
    ::unsetenv("UDM_VLR_PEER_PORT");

    auto client = make_client();
    const std::string base =
        "https://127.0.0.1:7780/nudm-uecm/v1/" + std::string(kTestSupi) + "/registrations";
    ASSERT_TRUE(wait_reachable(client, base + "/amf-3gpp-access", 200)) << "udm never reachable";
    const std::string token = fetch_token(client);
    ASSERT_FALSE(token.empty());

    // A real Nudm_UECM AMF registration -- the trigger.
    sbi_core::http2::ClientRequest reg;
    reg.method = "PUT";
    reg.url = base + "/amf-3gpp-access";
    reg.headers.emplace("content-type", "application/json");
    reg.headers.emplace("authorization", "Bearer " + token);
    reg.body =
        json{
            {"amfInstanceId", "00000000-0000-4000-8000-0000000000aa"},
            {"deregCallbackUri", "https://example.com/dereg"},
            {"guami", json{{"plmnId", json{{"mcc", "999"}, {"mnc", "70"}}}, {"amfId", "ABCDEF"}}},
            {"ratType", "NR"},
        }
            .dump();
    auto resp = client.send(reg);
    ASSERT_TRUE(resp.has_value());
    EXPECT_TRUE(resp->status == 201 || resp->status == 200 || resp->status == 204) << resp->body;

    if (vlr.joinable()) {
        vlr.join();
    }

    ASSERT_TRUE(got_invoke.load())
        << "no MAP Invoke reached the VLR -- UDM's map_client is still unreachable, which is the "
           "exact state ADR-0293 exists to fix";
    EXPECT_TRUE(answered.load()) << "the VLR never got far enough to answer the dialogue";
    EXPECT_TRUE(received_imsi_matches.load())
        << "the insertSubscriberData carried a different IMSI than the UE that registered";
}
