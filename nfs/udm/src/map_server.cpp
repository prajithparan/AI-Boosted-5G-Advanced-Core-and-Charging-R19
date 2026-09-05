#include "map_server.hpp"

#include <spdlog/spdlog.h>

#include <optional>

#include "map_core/map_dictionary.hpp"
#include "map_core/map_operations.hpp"
#include "ss7_core/m3ua_asp.hpp"
#include "ss7_core/m3ua_dictionary.hpp"
#include "ss7_core/m3ua_header.hpp"
#include "ss7_core/m3ua_protocol_data.hpp"
#include "ss7_core/m3ua_tlv.hpp"
#include "ss7_core/sccp_dictionary.hpp"
#include "ss7_core/sccp_udt.hpp"
#include "tbcd_core/tbcd.hpp"
#include "tcap_core/component.hpp"
#include "tcap_core/dialogue_portion.hpp"
#include "tcap_core/message.hpp"

namespace udm {
namespace {

void send_m3ua(ss7_core::SctpSocket& sock,
               std::uint8_t message_class,
               std::uint8_t message_type,
               const std::vector<std::uint8_t>& body) {
    auto msg = ss7_core::encode_m3ua_header({message_class, message_type},
                                            static_cast<std::uint32_t>(body.size()));
    msg.insert(msg.end(), body.begin(), body.end());
    sock.send(msg);
}

struct ReceivedM3ua {
    ss7_core::M3uaHeader header;
    std::vector<std::uint8_t> payload;
};

std::optional<ReceivedM3ua> receive_m3ua(ss7_core::SctpSocket& sock) {
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
    return ReceivedM3ua{*header, std::move(payload)};
}

// Real M3UA ASPSM/ASPTM activation, responder role (RFC 4666 §3.5/§3.7) -- the same exchange
// nfs/chf/src/cap_server.cpp performs, from the same ss7_core entry points.
bool do_asp_handshake_responder(ss7_core::SctpSocket& sock) {
    using namespace ss7_core;
    const auto up = receive_m3ua(sock);
    if (!up.has_value() || up->header.message_class != dictionary::MessageClass::kAspsm ||
        up->header.message_type != dictionary::AspsmMessageType::kAspUp) {
        return false;
    }
    send_m3ua(sock,
              dictionary::MessageClass::kAspsm,
              dictionary::AspsmMessageType::kAspUpAck,
              encode_asp_state_message(dictionary::AspsmMessageType::kAspUpAck, {}));

    const auto active = receive_m3ua(sock);
    if (!active.has_value() || active->header.message_class != dictionary::MessageClass::kAsptm ||
        active->header.message_type != dictionary::AsptmMessageType::kAspActive) {
        return false;
    }
    AspTrafficMessage ack;
    ack.traffic_mode_type = dictionary::TrafficModeType::kOverride;
    send_m3ua(sock,
              dictionary::MessageClass::kAsptm,
              dictionary::AsptmMessageType::kAspActiveAck,
              encode_asp_traffic_message(dictionary::AsptmMessageType::kAspActiveAck, ack));
    return true;
}

std::optional<ss7_core::SccpUdt> unwrap_to_sccp(const ReceivedM3ua& msg) {
    if (msg.header.message_class != ss7_core::dictionary::MessageClass::kTransfer ||
        msg.header.message_type != ss7_core::dictionary::TransferMessageType::kData) {
        return std::nullopt;
    }
    const auto tlvs = ss7_core::decode_m3ua_tlvs(msg.payload);
    if (!tlvs.has_value()) {
        return std::nullopt;
    }
    const auto* pd_tlv =
        ss7_core::find_m3ua_tlv(*tlvs, ss7_core::dictionary::ParamTag::kProtocolData);
    if (pd_tlv == nullptr) {
        return std::nullopt;
    }
    const auto proto_data = ss7_core::decode_m3ua_protocol_data(pd_tlv->value);
    if (!proto_data.has_value()) {
        return std::nullopt;
    }
    return ss7_core::decode_sccp_udt(proto_data->user_protocol_data);
}

// Wraps already-encoded TCAP bytes into a real SCCP UDT and M3UA DATA message. Addresses are the
// reverse of the request's: this side is the HLR, the peer is the VLR.
void send_tcap(ss7_core::SctpSocket& sock, const std::vector<std::uint8_t>& tcap_bytes) {
    ss7_core::SccpUdt udt;
    udt.protocol_class = ss7_core::dictionary::ProtocolClass::kClass0;
    udt.called_party.ssn_present = true;
    udt.called_party.ssn = ss7_core::dictionary::SubsystemNumber::kVlr;
    udt.calling_party.ssn_present = true;
    udt.calling_party.ssn = ss7_core::dictionary::SubsystemNumber::kHlr;
    udt.data = tcap_bytes;

    ss7_core::M3uaProtocolData pd;
    pd.opc = 2;
    pd.dpc = 1;
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
}

} // namespace

MapServer::MapServer(std::uint16_t port,
                     std::vector<std::uint8_t> hlr_number,
                     AmfRegistrationStore& amf_registrations)
    : hlr_number_(std::move(hlr_number)), amf_registrations_(amf_registrations) {
    listener_.bind_and_listen("0.0.0.0", port);
    accept_thread_ = std::thread(&MapServer::accept_loop, this);
}

MapServer::~MapServer() {
    stop_ = true;
    listener_.close();
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void MapServer::accept_loop() {
    while (!stop_) {
        try {
            auto conn = listener_.accept();
            std::thread(&MapServer::handle_connection, this, std::move(conn)).detach();
        } catch (const std::exception& e) {
            if (!stop_) {
                spdlog::warn("udm: MAP accept() failed: {}", e.what());
            }
        }
    }
}

void MapServer::handle_connection(ss7_core::SctpSocket socket) {
    spdlog::info("udm: real MAP (VLR) peer connected");
    if (!do_asp_handshake_responder(socket)) {
        spdlog::warn("udm: MAP peer failed the real M3UA ASPSM/ASPTM handshake");
        return;
    }

    while (!stop_) {
        const auto msg = receive_m3ua(socket);
        if (!msg.has_value()) {
            spdlog::info("udm: MAP peer association closed");
            return;
        }
        const auto udt = unwrap_to_sccp(*msg);
        if (!udt.has_value()) {
            spdlog::warn("udm: MAP peer sent a malformed M3UA/SCCP message, ignoring");
            continue;
        }
        const auto tag = tcap_core::peek_tc_message_tag(udt->data);
        if (!tag.has_value() || *tag != tcap_core::MessageTag::kBegin) {
            spdlog::info("udm: MAP peer sent an unexpected TCAP message (tag={}), ignoring",
                         tag.value_or(0));
            continue;
        }
        const auto begin = tcap_core::decode_tc_begin(udt->data);
        if (!begin.has_value() || begin->components.empty()) {
            spdlog::warn("udm: MAP peer sent a TC-Begin with no components, ignoring");
            continue;
        }
        const auto comp = tcap_core::decode_component(begin->components.front());
        if (!comp.has_value() || !comp->invoke.has_value() ||
            !comp->invoke->operation_code.local.has_value()) {
            spdlog::warn("udm: MAP peer's TC-Begin carried no decodable Invoke, ignoring");
            continue;
        }
        const auto& invoke = *comp->invoke;
        const auto opcode = *invoke.operation_code.local;

        std::vector<std::uint32_t> answer_context;
        std::optional<std::vector<std::uint8_t>> result_parameter;
        std::optional<std::int32_t> error_code;

        if (opcode == map_core::Opcode::kUpdateLocation) {
            const auto arg = map_core::decode_update_location_arg(invoke.parameter);
            if (!arg.has_value()) {
                spdlog::warn("udm: real MAP updateLocation had an undecodable argument");
                continue;
            }
            const auto ue_id = "imsi-" + tbcd_core::decode_tbcd(arg->imsi);
            // Record the VLR as this subscriber's serving node, in the SAME store Nudm_UECM's
            // AMF registration writes to -- one subscriber, one serving-node fact. This is also
            // what gives a later cancelLocation (ADR-0296) something real to cancel.
            nlohmann::json registration;
            registration["servingNode"] = "MAP-VLR";
            registration["vlrNumber"] = tbcd_core::decode_tbcd(arg->vlr_number);
            registration["mscNumber"] = tbcd_core::decode_tbcd(arg->msc_number);
            amf_registrations_.put(ue_id, registration);
            spdlog::info("udm: real MAP updateLocation accepted for {} (VLR={})",
                         ue_id,
                         registration["vlrNumber"].get<std::string>());
            answer_context = map_core::kNetworkLocUpContextV3Oid;
            result_parameter = map_core::encode_update_location_res(hlr_number_);
        } else if (opcode == map_core::Opcode::kPurgeMs) {
            const auto arg = map_core::decode_purge_ms_arg(invoke.parameter);
            if (!arg.has_value()) {
                spdlog::warn("udm: real MAP purgeMS had an undecodable argument");
                continue;
            }
            const auto ue_id = "imsi-" + tbcd_core::decode_tbcd(arg->imsi);
            const bool existed = amf_registrations_.remove(ue_id);
            spdlog::info("udm: real MAP purgeMS for {} ({})",
                         ue_id,
                         existed ? "record dropped" : "no record held");
            answer_context = map_core::kMsPurgingContextV3Oid;
            // freezeTMSI/freezeP-TMSI are deliberately NOT set -- see map_server.hpp.
            result_parameter = map_core::encode_purge_ms_res();
        } else {
            // Including sendAuthenticationInfo: a real ReturnError, never silence. A VLR that gets
            // no answer waits out its invoke timer and cannot tell "unsupported" from "dead".
            spdlog::info("udm: MAP opcode {} is not implemented -- answering ReturnError", opcode);
            error_code = opcode;
        }

        tcap_core::TcEnd end;
        end.destination_transaction_id = begin->originating_transaction_id;
        if (begin->dialogue_portion.has_value() && !answer_context.empty()) {
            tcap_core::DialogueResponse aare;
            aare.application_context_name = answer_context;
            aare.result = tcap_core::ResultType::kAccepted;
            aare.diagnostic.is_user_type = true;
            aare.diagnostic.value = tcap_core::DialogServiceUserType::kNoReasonGiven;
            end.dialogue_portion = tcap_core::encode_dialogue_portion_response(aare);
        }

        if (result_parameter.has_value()) {
            tcap_core::ReturnResultLast rrl;
            rrl.invoke_id = invoke.invoke_id;
            rrl.result = tcap_core::ReturnResult::Result{invoke.operation_code, *result_parameter};
            end.components.push_back(tcap_core::encode_return_result(rrl, true));
        } else if (error_code.has_value()) {
            tcap_core::ReturnError re;
            re.invoke_id = invoke.invoke_id;
            // TS 29.002's own real numeric MAP-Errors codes were not located during this
            // increment's research (map_dictionary.hpp records the same gap for
            // insertSubscriberData's errors), so no invented error number is sent -- the component
            // is a real ReturnError whose error code carries the unimplemented opcode, and that
            // narrowing is stated rather than dressed up as a real MAP error value.
            re.error_code.local = *error_code;
            end.components.push_back(tcap_core::encode_return_error(re));
        }
        send_tcap(socket, tcap_core::encode_tc_end(end));
    }
}

} // namespace udm
