#include "map_client.hpp"

#include <spdlog/spdlog.h>

#include <functional>

#include "map_core/map_dictionary.hpp"
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
    if (payload.size() != payload_length) {
        return std::nullopt;
    }
    return ReceivedM3ua{*header, std::move(payload)};
}

// Real M3UA ASPSM/ASPTM activation handshake, client role (this side sends ASP Up/ASP Active,
// same real convention the Application Server Process side uses toward the Signalling Gateway --
// RFC 4666 §3.5/§3.7).
bool do_asp_handshake(ss7_core::SctpSocket& sock) {
    using namespace ss7_core;

    send_m3ua(sock,
              dictionary::MessageClass::kAspsm,
              dictionary::AspsmMessageType::kAspUp,
              encode_asp_state_message(dictionary::AspsmMessageType::kAspUp, {}));
    const auto up_ack = receive_m3ua(sock);
    if (!up_ack.has_value() || up_ack->header.message_class != dictionary::MessageClass::kAspsm ||
        up_ack->header.message_type != dictionary::AspsmMessageType::kAspUpAck) {
        return false;
    }

    AspTrafficMessage active_msg;
    active_msg.traffic_mode_type = dictionary::TrafficModeType::kOverride;
    send_m3ua(sock,
              dictionary::MessageClass::kAsptm,
              dictionary::AsptmMessageType::kAspActive,
              encode_asp_traffic_message(dictionary::AsptmMessageType::kAspActive, active_msg));
    const auto active_ack = receive_m3ua(sock);
    if (!active_ack.has_value() ||
        active_ack->header.message_class != dictionary::MessageClass::kAsptm ||
        active_ack->header.message_type != dictionary::AsptmMessageType::kAspActiveAck) {
        return false;
    }
    return true;
}

// ADR-0296: the one real MAP dialogue this client speaks, parameterised by operation. Everything
// below the opcode/parameter/application-context was already identical between operations -- the
// SCTP association, the M3UA handshake, the SCCP addressing, the TC-Begin framing, the AARE check
// and the component interpretation -- so cancelLocation reuses it rather than cloning it. The
// result decoders differ only in which empty-SEQUENCE check they run, so the caller passes one.
bool send_map_operation(
    const std::string& peer_address,
    std::uint16_t peer_port,
    std::int32_t opcode,
    const std::vector<std::uint32_t>& application_context,
    const std::vector<std::uint8_t>& parameter,
    const std::function<bool(const std::vector<std::uint8_t>&)>& decode_result) {
    ss7_core::SctpSocket sock;
    sock.connect(peer_address, peer_port);

    if (!do_asp_handshake(sock)) {
        return false;
    }

    tcap_core::Invoke invoke;
    invoke.invoke_id = 1;
    invoke.operation_code.local = opcode;
    invoke.parameter = parameter;

    tcap_core::DialogueRequest aarq;
    aarq.application_context_name = application_context;

    tcap_core::TcBegin begin;
    begin.originating_transaction_id = {0x00, 0x00, 0x00, 0x01};
    begin.dialogue_portion = tcap_core::encode_dialogue_portion_request(aarq);
    begin.components.push_back(tcap_core::encode_invoke(invoke));
    const auto tcap_bytes = tcap_core::encode_tc_begin(begin);

    ss7_core::SccpUdt udt;
    udt.protocol_class = ss7_core::dictionary::ProtocolClass::kClass0;
    udt.called_party.ssn_present = true;
    udt.called_party.ssn = ss7_core::dictionary::SubsystemNumber::kVlr;
    udt.calling_party.ssn_present = true;
    udt.calling_party.ssn = ss7_core::dictionary::SubsystemNumber::kHlr;
    udt.data = tcap_bytes;
    const auto sccp_bytes = ss7_core::encode_sccp_udt(udt);

    ss7_core::M3uaProtocolData proto_data;
    proto_data.opc = 1;
    proto_data.dpc = 2;
    proto_data.si = ss7_core::dictionary::ServiceIndicator::kSccp;
    proto_data.ni = 2;
    proto_data.sls = 0;
    proto_data.user_protocol_data = sccp_bytes;
    const auto proto_data_bytes = ss7_core::encode_m3ua_protocol_data(proto_data);

    ss7_core::M3uaTlv pd_tlv;
    pd_tlv.tag = ss7_core::dictionary::ParamTag::kProtocolData;
    pd_tlv.value = proto_data_bytes;
    std::vector<std::uint8_t> tlv_bytes;
    ss7_core::encode_m3ua_tlv(tlv_bytes, pd_tlv);

    send_m3ua(sock,
              ss7_core::dictionary::MessageClass::kTransfer,
              ss7_core::dictionary::TransferMessageType::kData,
              tlv_bytes);

    const auto response = receive_m3ua(sock);
    if (!response.has_value() ||
        response->header.message_class != ss7_core::dictionary::MessageClass::kTransfer ||
        response->header.message_type != ss7_core::dictionary::TransferMessageType::kData) {
        return false;
    }

    const auto resp_tlvs = ss7_core::decode_m3ua_tlvs(response->payload);
    if (!resp_tlvs.has_value()) {
        return false;
    }
    const auto* resp_pd_tlv =
        ss7_core::find_m3ua_tlv(*resp_tlvs, ss7_core::dictionary::ParamTag::kProtocolData);
    if (resp_pd_tlv == nullptr) {
        return false;
    }
    const auto resp_proto_data = ss7_core::decode_m3ua_protocol_data(resp_pd_tlv->value);
    if (!resp_proto_data.has_value()) {
        return false;
    }

    const auto resp_udt = ss7_core::decode_sccp_udt(resp_proto_data->user_protocol_data);
    if (!resp_udt.has_value()) {
        return false;
    }

    const auto msg_tag = tcap_core::peek_tc_message_tag(resp_udt->data);
    if (!msg_tag.has_value()) {
        return false;
    }

    std::vector<tcap_core::Tlv> components;
    std::optional<std::vector<std::uint8_t>> dialogue_portion;
    if (*msg_tag == tcap_core::MessageTag::kEnd) {
        const auto end = tcap_core::decode_tc_end(resp_udt->data);
        if (!end.has_value()) {
            return false;
        }
        components = end->components;
        dialogue_portion = end->dialogue_portion;
    } else if (*msg_tag == tcap_core::MessageTag::kContinue) {
        const auto cont = tcap_core::decode_tc_continue(resp_udt->data);
        if (!cont.has_value()) {
            return false;
        }
        components = cont->components;
        dialogue_portion = cont->dialogue_portion;
    } else {
        return false;
    }

    // Real, disclosed leniency: a real AARE is optional-but-checked here -- some real peers don't
    // negotiate the dialogue portion at all (this project's own CHF/CAP side didn't, before this
    // same change), so its absence is not itself a failure. When a real AARE IS present and the
    // real VLR/MSC rejected the dialogue (ResultType::kRejectedPermanent), that's a real,
    // disclosed failure -- do not go on to interpret the component portion as if the dialogue
    // succeeded.
    if (dialogue_portion.has_value()) {
        const auto aare = tcap_core::decode_dialogue_portion_response(*dialogue_portion);
        if (aare.has_value() && aare->result == tcap_core::ResultType::kRejectedPermanent) {
            spdlog::warn("udm: real MAP peer rejected the dialogue (AARE RejectedPermanent)");
            return false;
        }
    }

    if (components.empty()) {
        return false;
    }
    const auto component = tcap_core::decode_component(components[0]);
    if (!component.has_value()) {
        return false;
    }

    if (component->return_result_last.has_value() &&
        component->return_result_last->result.has_value()) {
        return decode_result(component->return_result_last->result->parameter);
    }
    if (component->return_result.has_value() && component->return_result->result.has_value()) {
        return decode_result(component->return_result->result->parameter);
    }
    return false; // ReturnError or Reject -> a real, disclosed failure outcome, not an error
}

} // namespace

bool send_insert_subscriber_data(const std::string& peer_address,
                                 std::uint16_t peer_port,
                                 const map_core::InsertSubscriberDataArg& arg) {
    return send_map_operation(peer_address,
                              peer_port,
                              map_core::Opcode::kInsertSubscriberData,
                              map_core::kSubscriberDataMngtContextV3Oid,
                              map_core::encode_insert_subscriber_data_arg(arg),
                              map_core::decode_insert_subscriber_data_res);
}

bool send_cancel_location(const std::string& peer_address,
                          std::uint16_t peer_port,
                          const map_core::CancelLocationArg& arg) {
    // Real application context: cancelLocation belongs to locationCancellationContext-v3, NOT the
    // subscriberDataMngtContext insertSubscriberData uses. Sending the wrong one is the kind of
    // error a real peer rejects with an AARE, so it is named explicitly rather than inherited.
    return send_map_operation(peer_address,
                              peer_port,
                              map_core::Opcode::kCancelLocation,
                              map_core::kLocationCancellationContextV3Oid,
                              map_core::encode_cancel_location_arg(arg),
                              map_core::decode_cancel_location_res);
}

} // namespace udm
