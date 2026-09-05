#include "cap_server.hpp"

#include <spdlog/spdlog.h>

#include "cap_core/cap_dictionary.hpp"
#include "cap_core/cap_operations.hpp"
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

namespace chf {

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

// Real M3UA ASPSM/ASPTM activation handshake, responder role (this side receives ASP Up/ASP
// Active from the peer and acknowledges -- RFC 4666 §3.5/§3.7).
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
    AspTrafficMessage ack_msg;
    ack_msg.traffic_mode_type = dictionary::TrafficModeType::kOverride;
    send_m3ua(sock,
              dictionary::MessageClass::kAsptm,
              dictionary::AsptmMessageType::kAspActiveAck,
              encode_asp_traffic_message(dictionary::AsptmMessageType::kAspActiveAck, ack_msg));
    return true;
}

// Unwraps an M3UA DATA message down to the real SCCP UDT it carries, or std::nullopt on any
// malformed layer.
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

// Wraps a real TCAP message (already-encoded bytes) into a real SCCP UDT (addressed
// calling=gsmSCF/HLR-analogue role reversed for the response: calling=SSF's own called SSN,
// called=SSF's own calling SSN -- real SCCP UDT response convention, addresses swapped) then a
// real M3UA DATA message, and sends it.
void send_tcap(ss7_core::SctpSocket& sock, const std::vector<std::uint8_t>& tcap_bytes) {
    ss7_core::SccpUdt udt;
    udt.protocol_class = ss7_core::dictionary::ProtocolClass::kClass0;
    udt.called_party.ssn_present = true;
    udt.called_party.ssn = ss7_core::dictionary::SubsystemNumber::kMsc;
    udt.calling_party.ssn_present = true;
    udt.calling_party.ssn = ss7_core::dictionary::SubsystemNumber::kHlr; // gsmSCF role, reusing
                                                                         // the real HLR SSN slot
                                                                         // (no dedicated gsmSCF
                                                                         // SSN constant exists in
                                                                         // sccp_dictionary.hpp)
    udt.data = tcap_bytes;
    const auto sccp_bytes = ss7_core::encode_sccp_udt(udt);

    ss7_core::M3uaProtocolData proto_data;
    proto_data.opc = 2;
    proto_data.dpc = 1;
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
}

} // namespace

CapServer::CapServer(std::uint16_t port,
                     sbi_core::http2::TlsConfig client_tls,
                     ChargingDataStore& charging_data_store,
                     CdrWriter& cdr_writer,
                     RatingDecisionStore& rating_decision_store,
                     opentelemetry::metrics::Counter<std::uint64_t>* grant_counter,
                     opentelemetry::metrics::Counter<std::uint64_t>* reserve_rejected_counter,
                     opentelemetry::metrics::Counter<std::uint64_t>* initial_dp_counter,
                     opentelemetry::metrics::Counter<std::uint64_t>* apply_charging_counter)
    : client_tls_(std::move(client_tls)), charging_data_store_(charging_data_store),
      cdr_writer_(cdr_writer), rating_decision_store_(rating_decision_store),
      grant_counter_(grant_counter), reserve_rejected_counter_(reserve_rejected_counter),
      initial_dp_counter_(initial_dp_counter), apply_charging_counter_(apply_charging_counter) {
    listener_.bind_and_listen("0.0.0.0", port);
    accept_thread_ = std::thread(&CapServer::accept_loop, this);
}

CapServer::~CapServer() {
    stop_ = true;
    listener_.close();
    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }
}

void CapServer::set_tps_limit(double sustained_tps, double burst_capacity) {
    if (sustained_tps <= 0.0) {
        rate_limit_.store(nullptr, std::memory_order_release);
        rate_limit_owner_.reset();
        return;
    }
    rate_limit_owner_ = std::make_unique<sbi_core::TokenBucket>(sustained_tps, burst_capacity);
    // Release/acquire, not relaxed: a connection thread that sees this pointer must also see the
    // fully-constructed bucket behind it.
    rate_limit_.store(rate_limit_owner_.get(), std::memory_order_release);
}

void CapServer::accept_loop() {
    while (!stop_) {
        try {
            auto conn = listener_.accept();
            std::thread(&CapServer::handle_connection, this, std::move(conn)).detach();
        } catch (const std::exception& e) {
            if (!stop_) {
                spdlog::warn("chf: CAP accept() failed: {}", e.what());
            }
        }
    }
}

void CapServer::handle_connection(ss7_core::SctpSocket socket) {
    spdlog::info("chf: real CAP (gsmSSF) peer connected");

    if (!do_asp_handshake_responder(socket)) {
        spdlog::warn("chf: CAP peer failed the real M3UA ASPSM/ASPTM handshake");
        return;
    }

    sbi_core::http2::Client catalog_client(client_tls_);
    sbi_core::http2::Client balance_client(client_tls_);

    // Real dialogue state, persisted across this association's own real multi-message CAP
    // dialogue (InitialDP -> ... -> ApplyChargingReport) -- the same thread-per-association shape
    // as everything else here, just remembering the charging ref/SUPI/peer transaction id between
    // loop iterations instead of only within one iteration.
    std::optional<std::string> current_ref;
    std::optional<std::string> current_supi;
    // ADR-0298: remembered so a mid-call re-authorization rates against the SAME rating group the
    // InitialDP did, rather than re-deriving it from a serviceKey no later message carries.
    std::optional<std::int64_t> current_rating_group;
    std::vector<std::uint8_t> peer_transaction_id;

    while (!stop_) {
        const auto msg = receive_m3ua(socket);
        if (!msg.has_value()) {
            spdlog::info("chf: CAP peer association closed");
            return;
        }
        // P15 (ADR-0288): the SS7/M3UA front door's ceiling -- the third and final protocol, after
        // SBI (ADR-0280) and Diameter (ADR-0285). Checked on a received M3UA message, before the
        // SCCP unwrap and the TCAP/CAP decode below, because those are where the real work is.
        //
        // A shed here DROPS the message rather than answering. That is a deliberate difference
        // from the SBI and Diameter ceilings, and the reason is protocol shape, not convenience: a
        // TCAP answer is not a status line, it is a TC-Abort inside a correctly-addressed SCCP/
        // M3UA envelope quoting the peer's own transaction id -- which means decoding the very
        // message being shed and building a reply nearly as expensive as serving it. Dropping is
        // also what a real SS7 node does under congestion: TCAP dialogues are protected by the
        // peer's own invoke timers, which is the mechanism that exists for exactly this.
        if (auto* limiter = rate_limit_.load(std::memory_order_acquire);
            limiter != nullptr && !limiter->try_acquire()) {
            spdlog::warn("chf: CAP message dropped at the configured TPS ceiling -- the peer's own "
                         "TCAP invoke timer is what recovers this dialogue");
            continue;
        }

        const auto udt = unwrap_to_sccp(*msg);
        if (!udt.has_value()) {
            spdlog::warn("chf: CAP peer sent a malformed M3UA/SCCP message, ignoring");
            continue;
        }
        const auto tag = tcap_core::peek_tc_message_tag(udt->data);

        if (tag.has_value() && *tag == tcap_core::MessageTag::kContinue) {
            const auto cont = tcap_core::decode_tc_continue(udt->data);
            if (!cont.has_value()) {
                spdlog::warn("chf: CAP peer sent a malformed TC-Continue, ignoring");
                continue;
            }
            for (const auto& comp_tlv : cont->components) {
                const auto comp = tcap_core::decode_component(comp_tlv);
                if (!comp.has_value() || !comp->invoke.has_value() ||
                    !comp->invoke->operation_code.local.has_value()) {
                    continue;
                }
                const auto& evt_invoke = *comp->invoke;
                const auto opcode = *evt_invoke.operation_code.local;

                if (opcode == cap_core::Opcode::kEventReportBcsm) {
                    // Real, correctly-scoped no-op: EventReportBCSM (Class 4, "ALWAYS RESPONDS
                    // FALSE" per TS 29.078 clause 6.1.1) has no real defined response -- logging
                    // the real event is the whole real obligation here.
                    const auto evt = cap_core::decode_event_report_bcsm_arg(evt_invoke.parameter);
                    spdlog::info("chf: real CAP EventReportBCSM received (eventTypeBCSM={})",
                                 evt.has_value() ? evt->event_type_bcsm : -1);
                } else if (opcode == cap_core::Opcode::kApplyChargingReport) {
                    const auto report =
                        cap_core::decode_apply_charging_report_arg(evt_invoke.parameter);
                    if (!report.has_value() || !current_ref.has_value() ||
                        !current_supi.has_value()) {
                        spdlog::warn("chf: real CAP ApplyChargingReport received but no decodable "
                                     "report or no open InitialDP dialogue, ignoring");
                        continue;
                    }
                    const auto elapsed_seconds = report->elapsed_hundred_ms_units / 10;
                    spdlog::info("chf: real CAP ApplyChargingReport received (SUPI={}, "
                                 "elapsedSeconds={}, legActive={})",
                                 *current_supi,
                                 elapsed_seconds,
                                 report->leg_active);

                    // ADR-0298: mid-call re-authorization. `legActive` (CAMEL-CallResult's own
                    // real [2] BOOLEAN DEFAULT TRUE) is what separates a PERIODIC report -- the
                    // call is still up and maxCallPeriodDuration just expired -- from the FINAL
                    // one. Before this, every report was treated as call end, so a call longer
                    // than one charging period was finalized while it was still running and then
                    // carried on unmonitored and unbilled.
                    if (report->leg_active && current_rating_group.has_value()) {
                        // Rate a fresh period against the same rating group, exactly as the
                        // InitialDP did: the same charge_one_usage shared code path, so the
                        // reservation, CDR row and RatingDecision audit all happen for the renewal
                        // too rather than only for the first period.
                        sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging renewal{};
                        renewal.ratingGroup = static_cast<sbi_gen::Uint32>(*current_rating_group);
                        const auto renewed = chf::charge_one_usage(catalog_client,
                                                                   balance_client,
                                                                   cdr_writer_,
                                                                   rating_decision_store_,
                                                                   charging_data_store_,
                                                                   grant_counter_,
                                                                   reserve_rejected_counter_,
                                                                   *current_ref,
                                                                   "Update",
                                                                   *current_supi,
                                                                   "CHF",
                                                                   "",
                                                                   evt_invoke.invoke_id,
                                                                   renewal);

                        std::int32_t renewed_duration = 0;
                        if (renewed.reserved && renewed.rating.grant.has_value() &&
                            renewed.rating.grant->time.has_value()) {
                            renewed_duration =
                                static_cast<std::int32_t>(*renewed.rating.grant->time) * 10;
                        }

                        if (!renewed.reserved) {
                            // Out of money mid-call. The honest action is to stop granting, not to
                            // keep the call up on credit: no further ApplyCharging is sent, and
                            // `releaseIfDurationExceeded` on the period already granted is what
                            // makes the gsmSSF tear the call down. Disclosed rather than papered
                            // over -- a real gsmSCF would typically also send ReleaseCall here,
                            // which this build does not implement (cap_operations has the encoder,
                            // nothing calls it).
                            spdlog::warn("chf: CAP re-authorization REFUSED for SUPI={} (no "
                                         "balance) -- no further charging period granted",
                                         *current_supi);
                            continue;
                        }

                        cap_core::ApplyChargingArg renewed_ac;
                        renewed_ac.max_call_period_duration = renewed_duration;
                        renewed_ac.release_if_duration_exceeded = true;

                        tcap_core::Invoke renewed_invoke;
                        renewed_invoke.invoke_id = evt_invoke.invoke_id + 1;
                        renewed_invoke.operation_code.local = cap_core::Opcode::kApplyCharging;
                        renewed_invoke.parameter = cap_core::encode_apply_charging_arg(renewed_ac);

                        tcap_core::TcContinue renewal_cont;
                        renewal_cont.originating_transaction_id = {0x00, 0x00, 0x00, 0x01};
                        renewal_cont.destination_transaction_id = peer_transaction_id;
                        renewal_cont.components.push_back(tcap_core::encode_invoke(renewed_invoke));
                        send_tcap(socket, tcap_core::encode_tc_continue(renewal_cont));
                        if (apply_charging_counter_ != nullptr) {
                            apply_charging_counter_->Add(1);
                        }
                        spdlog::info("chf: CAP re-authorization granted -- another "
                                     "ApplyCharging sent (maxCallPeriodDuration={} = {}s)",
                                     renewed_duration,
                                     renewed_duration / 10);
                        // The dialogue stays open: current_ref/current_supi are deliberately NOT
                        // cleared, so the next report lands on the same session.
                        continue;
                    }

                    // ADR-0304 closes what ADR-0297 had to disclose here. That entry said CAP
                    // could not charge proportionally because the rating engine granted volume or
                    // service units while ApplyChargingReport reports elapsed TIME, and no
                    // seconds-to-octets conversion exists to bridge them. The fix was never a
                    // conversion -- it was making duration a real grant dimension, which C2 did.
                    //
                    // So: when this session holds a TIME grant, the elapsed seconds the gsmSSF
                    // actually reported proportion the debit, exactly as octets do on the HTTP
                    // path. When it does not (a volume-priced offering charged over CAP, which is
                    // an operator configuration choice this code does not prevent), the dimension mismatch is
                    // real and unchanged, and the full reservation is still finalized -- so the
                    // honest behaviour is preserved for exactly the case that used to be the only
                    // case.
                    const auto reserved_total =
                        charging_data_store_.get_reserved_total(*current_ref);
                    const auto granted_time = charging_data_store_.get_granted_time(*current_ref);
                    const auto granted_volume =
                        charging_data_store_.get_granted_volume(*current_ref);
                    const auto granted_units =
                        charging_data_store_.get_granted_service_units(*current_ref);
                    const auto amount_to_debit =
                        granted_time > 0.0
                            ? chf::proportional_debit(reserved_total,
                                                      /*granted_volume=*/0.0,
                                                      /*used_volume=*/0.0,
                                                      /*granted_service_units=*/0.0,
                                                      /*used_service_units=*/0.0,
                                                      granted_time,
                                                      static_cast<double>(elapsed_seconds))
                            : reserved_total;
                    (void)granted_volume;
                    (void)granted_units;
                    charging_data_store_.release(*current_ref);
                    spdlog::info("chf: CAP finalize for {} -- reserved {}, debiting {} (used {}s "
                                 "of {}s granted)",
                                 *current_ref,
                                 reserved_total,
                                 amount_to_debit,
                                 elapsed_seconds,
                                 granted_time);
                    chf::finalize_subscriber_balance(balance_client,
                                                     *current_supi,
                                                     reserved_total,
                                                     amount_to_debit,
                                                     "CAP-gsmSSF ApplyChargingReport " +
                                                         *current_ref);

                    // Real Q.773/TS 29.078 fact: applyChargingReport's real operation definition
                    // is "RESULT FALSE" (Class 2, only ERROR is defined) -- there is no real
                    // successful RESULT payload to send back, so the dialogue closes with an
                    // empty real TC-End, not a ReturnResultLast.
                    tcap_core::TcEnd end;
                    end.destination_transaction_id = peer_transaction_id;
                    send_tcap(socket, tcap_core::encode_tc_end(end));

                    current_ref.reset();
                    current_supi.reset();
                    current_rating_group.reset();
                } else {
                    spdlog::info("chf: CAP peer's TC-Continue carried opcode {} (not implemented), "
                                 "ignoring",
                                 opcode);
                }
            }
            continue;
        }

        if (!tag.has_value() || *tag != tcap_core::MessageTag::kBegin) {
            spdlog::info("chf: CAP peer sent an unexpected message (tag={}), ignoring",
                         tag.value_or(0));
            continue;
        }

        const auto begin = tcap_core::decode_tc_begin(udt->data);
        if (!begin.has_value() || begin->components.empty()) {
            spdlog::warn("chf: CAP peer sent a malformed TC-Begin, ignoring");
            continue;
        }
        const auto component = tcap_core::decode_component(begin->components[0]);
        if (!component.has_value() || !component->invoke.has_value() ||
            !component->invoke->operation_code.local.has_value()) {
            spdlog::warn("chf: CAP peer's TC-Begin did not carry a decodable Invoke, ignoring");
            continue;
        }
        // Real, disclosed leniency, symmetric with UDM's own MAP client
        // (nfs/udm/src/map_client.cpp): logged when present, not required -- some real gsmSSF peers
        // may not negotiate a structured dialogue portion at all.
        if (begin->dialogue_portion.has_value()) {
            const auto aarq = tcap_core::decode_dialogue_portion_request(*begin->dialogue_portion);
            if (aarq.has_value()) {
                spdlog::info("chf: real CAP peer opened the dialogue with a structured AARQ "
                             "(applicationContextName has {} arcs)",
                             aarq->application_context_name.size());
            }
        }

        const auto& invoke = *component->invoke;
        if (*invoke.operation_code.local != cap_core::Opcode::kInitialDp) {
            spdlog::info("chf: CAP peer's TC-Begin carried opcode {} (only InitialDP=0 is "
                         "implemented), ignoring",
                         *invoke.operation_code.local);
            continue;
        }

        if (initial_dp_counter_ != nullptr) {
            initial_dp_counter_->Add(1);
        }

        const auto arg = cap_core::decode_initial_dp_arg(invoke.parameter);
        if (!arg.has_value() || !arg->imsi.has_value()) {
            spdlog::warn("chf: real InitialDP missing a decodable arg or IMSI -- real "
                         "missingParameter ReturnError");
            tcap_core::ReturnError re;
            re.invoke_id = invoke.invoke_id;
            re.error_code.local = cap_core::ErrorCode::kMissingParameter;
            tcap_core::TcEnd end;
            end.destination_transaction_id = begin->originating_transaction_id;
            end.components.push_back(tcap_core::encode_return_error(re));
            send_tcap(socket, tcap_core::encode_tc_end(end));
            continue;
        }

        const auto imsi_digits = tbcd_core::decode_tbcd(*arg->imsi);
        const std::string supi = "imsi-" + imsi_digits;
        spdlog::info(
            "chf: real CAP InitialDP received (SUPI={}, serviceKey={})", supi, arg->service_key);

        peer_transaction_id = begin->originating_transaction_id;
        const auto ref = charging_data_store_.create(supi);
        current_ref = ref;
        current_supi = supi;
        current_rating_group = static_cast<std::int64_t>(arg->service_key);
        sbi_gen::MultipleUnitUsage_Nchf_ConvergedCharging usage{};
        usage.ratingGroup = static_cast<sbi_gen::Uint32>(arg->service_key);
        const auto charged = chf::charge_one_usage(catalog_client,
                                                   balance_client,
                                                   cdr_writer_,
                                                   rating_decision_store_,
                                                   charging_data_store_,
                                                   grant_counter_,
                                                   reserve_rejected_counter_,
                                                   ref,
                                                   "Create",
                                                   supi,
                                                   "CAP-gsmSSF",
                                                   // Real, disclosed: "CAP-gsmSSF" is this
                                                   // project's own protocol-identity label, not a
                                                   // real TS 32.298 NetworkFunctionality value, so
                                                   // encode_chf_cdr already returns an empty blob
                                                   // for this whole real call path regardless --
                                                   // recording_network_function_id has no live
                                                   // effect here, not threaded through CapServer's
                                                   // own constructor for that reason.
                                                   "",
                                                   invoke.invoke_id,
                                                   usage);

        std::int32_t max_call_period_duration = 0; // 100ms units, real ApplyChargingArg field
        if (charged.reserved && charged.rating.grant.has_value() &&
            charged.rating.grant->time.has_value()) {
            max_call_period_duration = static_cast<std::int32_t>(*charged.rating.grant->time) * 10;
        }

        cap_core::RequestReportBcsmEventArg rrbe;
        cap_core::BcsmEvent answer_evt;
        answer_evt.event_type_bcsm = cap_core::EventTypeBcsm::kOAnswer;
        answer_evt.monitor_mode = cap_core::MonitorMode::kNotifyAndContinue;
        cap_core::BcsmEvent disconnect_evt;
        disconnect_evt.event_type_bcsm = cap_core::EventTypeBcsm::kODisconnect;
        disconnect_evt.monitor_mode = cap_core::MonitorMode::kNotifyAndContinue;
        rrbe.bcsm_events = {answer_evt, disconnect_evt};

        tcap_core::Invoke rrbe_invoke;
        rrbe_invoke.invoke_id = 2;
        rrbe_invoke.operation_code.local = cap_core::Opcode::kRequestReportBcsmEvent;
        rrbe_invoke.parameter = cap_core::encode_request_report_bcsm_event_arg(rrbe);

        cap_core::ApplyChargingArg ac;
        ac.max_call_period_duration = max_call_period_duration;
        ac.release_if_duration_exceeded = true;

        tcap_core::Invoke ac_invoke;
        ac_invoke.invoke_id = 3;
        ac_invoke.operation_code.local = cap_core::Opcode::kApplyCharging;
        ac_invoke.parameter = cap_core::encode_apply_charging_arg(ac);

        // Real AARE (dialogue response), sent only when the gsmSSF peer opened with a real,
        // structured AARQ -- ACSE only expects a dialogue response when a dialogue was actually
        // proposed. Echoes back the real gsmSSF-scfGenericAC OID the peer proposed (real X.227
        // semantics: accepting the SAME application context the AARQ named, not inventing a
        // different one) with ResultType::kAccepted and a real
        // DialogServiceUserType::kNoReasonGiven diagnostic (no real reason needed for an accept).
        tcap_core::TcContinue cont;
        cont.originating_transaction_id = {0x00, 0x00, 0x00, 0x01};
        cont.destination_transaction_id = begin->originating_transaction_id;
        if (begin->dialogue_portion.has_value()) {
            tcap_core::DialogueResponse aare;
            aare.application_context_name = cap_core::kGsmssfScfGenericAcOid;
            aare.result = tcap_core::ResultType::kAccepted;
            aare.diagnostic.is_user_type = true;
            aare.diagnostic.value = tcap_core::DialogServiceUserType::kNoReasonGiven;
            cont.dialogue_portion = tcap_core::encode_dialogue_portion_response(aare);
        }
        cont.components.push_back(tcap_core::encode_invoke(rrbe_invoke));
        cont.components.push_back(tcap_core::encode_invoke(ac_invoke));

        send_tcap(socket, tcap_core::encode_tc_continue(cont));
        if (apply_charging_counter_ != nullptr) {
            apply_charging_counter_->Add(1);
        }
        spdlog::info("chf: real CAP RequestReportBCSMEvent+ApplyCharging sent "
                     "(maxCallPeriodDuration={} = {}s)",
                     max_call_period_duration,
                     max_call_period_duration / 10);
    }
}

} // namespace chf
