#include "ngap_test_gnb.hpp"

#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#include "ngap_core/ngap_codec.hpp"
#include "ngap_core/sctp_socket.hpp"

extern "C" {
#include <AMF-UE-NGAP-ID.h>
#include <Cause.h>
#include <DownlinkNASTransport.h>
#include <GTPTunnel.h>
#include <GlobalGNB-ID.h>
#include <GlobalRANNodeID.h>
#include <HandoverCancel.h>
#include <HandoverCommand.h>
#include <HandoverFailure.h>
#include <HandoverNotify.h>
#include <HandoverPreparationFailure.h>
#include <HandoverRequest.h>
#include <HandoverRequestAcknowledge.h>
#include <HandoverRequestAcknowledgeTransfer.h>
#include <HandoverRequired.h>
#include <HandoverType.h>
#include <InitialUEMessage.h>
#include <InitiatingMessage.h>
#include <NAS-PDU.h>
#include <NGAP-PDU.h>
#include <NGSetupRequest.h>
#include <NR-CGI.h>
#include <PDUSessionResourceAdmittedItem.h>
#include <PDUSessionResourceAdmittedList.h>
#include <PDUSessionResourceHandoverItem.h>
#include <PDUSessionResourceHandoverList.h>
#include <PDUSessionResourceItemHORqd.h>
#include <PDUSessionResourceListHORqd.h>
#include <PDUSessionResourceSetupItemHOReq.h>
#include <PDUSessionResourceSetupListHOReq.h>
#include <PLMNIdentity.h>
#include <QosFlowItemWithDataForwarding.h>
#include <QosFlowListWithDataForwarding.h>
#include <RAN-UE-NGAP-ID.h>
#include <RRCEstablishmentCause.h>
#include <SourceToTarget-TransparentContainer.h>
#include <SuccessfulOutcome.h>
#include <TargetID.h>
#include <TargetRANNodeID.h>
#include <TargetToSource-TransparentContainer.h>
#include <UEContextReleaseCommand.h>
#include <UEContextReleaseComplete.h>
#include <UPTransportLayerInformation.h>
#include <UnsuccessfulOutcome.h>
#include <UplinkNASTransport.h>
#include <UserLocationInformation.h>
#include <UserLocationInformationNR.h>
}

namespace nf_test {

namespace {

// This lab's fixed test PLMN, matching nfs/amf/src/ngap_task.cpp's own kMcc/kMnc exactly. A gNB
// whose PLMN did not match would still get an NGSetupResponse (AMF logs and continues), but its
// GlobalGNB-ID key would differ from the one a HandoverRequired's TargetID resolves to, and the
// relay would silently never find the target -- so this must agree, not merely be plausible.
constexpr const char* kMcc = "999";
constexpr const char* kMnc = "70";
constexpr std::size_t kGnbIdBits = 32;

OCTET_STRING_t make_octet_string(const std::uint8_t* data, std::size_t len) {
    OCTET_STRING_t s{};
    s.buf = static_cast<std::uint8_t*>(std::malloc(len));
    s.size = static_cast<int>(len);
    std::memcpy(s.buf, data, len);
    return s;
}

// PLMN identity per TS 24.008/38.413 -- half-octet BCD, filler 0xF for a 2-digit MNC's third
// digit. Mirrors AMF's own encode_plmn_identity; the two must produce identical bytes or the
// GlobalGNB-ID registry key will not match.
void encode_plmn_identity(const char* mcc, const char* mnc, std::uint8_t out[3]) {
    const int mcc1 = mcc[0] - '0';
    const int mcc2 = mcc[1] - '0';
    const int mcc3 = mcc[2] - '0';
    const bool mnc_is_3_digit = std::strlen(mnc) == 3;
    const int mnc1 = mnc[0] - '0';
    const int mnc2 = mnc[1] - '0';
    const int mnc3 = mnc_is_3_digit ? (mnc[2] - '0') : 0xF;
    out[0] = static_cast<std::uint8_t>((mcc2 << 4) | mcc1);
    out[1] = static_cast<std::uint8_t>((mnc3 << 4) | mcc3);
    out[2] = static_cast<std::uint8_t>((mnc2 << 4) | mnc1);
}

BIT_STRING_t make_bit_string_from_uint(unsigned long value, std::size_t bits) {
    const std::size_t bytes = (bits + 7) / 8;
    BIT_STRING_t s{};
    s.buf = static_cast<std::uint8_t*>(std::calloc(bytes, 1));
    s.size = static_cast<int>(bytes);
    s.bits_unused = static_cast<int>(bytes * 8 - bits);
    const unsigned long shifted = value << s.bits_unused;
    for (std::size_t i = 0; i < bytes; ++i) {
        s.buf[bytes - 1 - i] = static_cast<std::uint8_t>((shifted >> (8 * i)) & 0xFF);
    }
    return s;
}

// Fills a real GlobalGNB-ID (PLMNIdentity + gNB-ID bit string) -- the identity AMF re-encodes and
// uses as its GnbAssociationRegistry key.
void fill_global_gnb_id(GlobalGNB_ID_t& out, std::uint32_t gnb_id) {
    std::uint8_t plmn[3];
    encode_plmn_identity(kMcc, kMnc, plmn);
    out.pLMNIdentity = make_octet_string(plmn, 3);
    out.gNB_ID.present = GNB_ID_PR_gNB_ID;
    out.gNB_ID.choice.gNB_ID = make_bit_string_from_uint(gnb_id, kGnbIdBits);
}

} // namespace

struct NgapTestGnb::Impl {
    ngap_core::SctpSocket socket;
};

NgapTestGnb::NgapTestGnb() : impl_(new Impl()) {}
NgapTestGnb::~NgapTestGnb() {
    delete impl_;
}

bool NgapTestGnb::connect(const std::string& address, std::uint16_t port, int max_attempts) {
    for (int attempt = 0; attempt < max_attempts; ++attempt) {
        try {
            impl_->socket.connect(address, port);
            // Every receive in these tests is bounded, so a peer that never answers fails the
            // test with its own assertion message instead of hanging until the CI job is killed
            // -- a hang there is indistinguishable from the runner dying mid-job. Generous
            // relative to what the exchanges actually take (the widest, the PDU session round
            // trip through SMF and PCF, is ~250ms locally): this is a deadlock guard, not a
            // performance assertion.
            impl_->socket.set_receive_timeout(std::chrono::seconds(30));
            return true;
        } catch (const std::exception&) {
            // AMF's NGAP listener starts on its own thread after the process does; retry.
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return false;
}

bool NgapTestGnb::ng_setup(std::uint32_t gnb_id) {
    NGSetupRequest_t request{};

    GlobalRANNodeID_t ran_node_id{};
    ran_node_id.present = GlobalRANNodeID_PR_globalGNB_ID;
    ran_node_id.choice.globalGNB_ID =
        static_cast<GlobalGNB_ID_t*>(std::calloc(1, sizeof(GlobalGNB_ID_t)));
    fill_global_gnb_id(*ran_node_id.choice.globalGNB_ID, gnb_id);
    ::ngap::add_ie(request.protocolIEs,
                   ::ngap::make_ie(27 /* id-GlobalRANNodeID */,
                                   Criticality_reject,
                                   &asn_DEF_GlobalRANNodeID,
                                   &ran_node_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_GlobalRANNodeID, &ran_node_id);

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = kProcNgSetup;
    pdu.choice.initiatingMessage->criticality = Criticality_reject;
    pdu.choice.initiatingMessage->value.present = InitiatingMessage__value_PR_NGSetupRequest;
    pdu.choice.initiatingMessage->value.choice.NGSetupRequest = request;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    if (bytes.empty()) {
        return false;
    }

    impl_->socket.send(bytes);
    const auto reply = impl_->socket.receive();
    const auto summary = summarize(reply);
    return summary.outcome == Outcome::Successful && summary.procedure_code == kProcNgSetup;
}

void NgapTestGnb::send_raw(const std::vector<std::uint8_t>& pdu_bytes) {
    impl_->socket.send(pdu_bytes);
}

std::vector<std::uint8_t> NgapTestGnb::receive_raw() {
    return impl_->socket.receive();
}

std::vector<std::uint8_t> NgapTestGnb::build_handover_required(std::uint64_t amf_ue_id,
                                                               std::uint32_t ran_ue_id,
                                                               std::uint32_t target_gnb_id,
                                                               std::uint8_t pdu_session_id) {
    HandoverRequired_t required{};

    AMF_UE_NGAP_ID_t amf_id{};
    asn_ulong2INTEGER(&amf_id, static_cast<unsigned long>(amf_ue_id));
    ::ngap::add_ie(
        required.protocolIEs,
        ::ngap::make_ie(
            10 /* id-AMF-UE-NGAP-ID */, Criticality_reject, &asn_DEF_AMF_UE_NGAP_ID, &amf_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_id);

    RAN_UE_NGAP_ID_t ran_id = static_cast<RAN_UE_NGAP_ID_t>(ran_ue_id);
    ::ngap::add_ie(
        required.protocolIEs,
        ::ngap::make_ie(
            85 /* id-RAN-UE-NGAP-ID */, Criticality_reject, &asn_DEF_RAN_UE_NGAP_ID, &ran_id));

    // intra5gs: a real 5GS-to-5GS N2 handover, which is the case this project implements.
    HandoverType_t ho_type = HandoverType_intra5gs;
    ::ngap::add_ie(
        required.protocolIEs,
        ::ngap::make_ie(
            29 /* id-HandoverType */, Criticality_reject, &asn_DEF_HandoverType, &ho_type));

    // handover-desirable-for-radio-reason: the real, standard cause a source gNB gives for a
    // radio-triggered handover.
    Cause_t cause{};
    cause.present = Cause_PR_radioNetwork;
    cause.choice.radioNetwork = CauseRadioNetwork_handover_desirable_for_radio_reason;
    ::ngap::add_ie(required.protocolIEs,
                   ::ngap::make_ie(15 /* id-Cause */, Criticality_ignore, &asn_DEF_Cause, &cause));

    TargetID_t target_id{};
    target_id.present = TargetID_PR_targetRANNodeID;
    target_id.choice.targetRANNodeID =
        static_cast<TargetRANNodeID_t*>(std::calloc(1, sizeof(TargetRANNodeID_t)));
    auto& target_node = *target_id.choice.targetRANNodeID;
    target_node.globalRANNodeID.present = GlobalRANNodeID_PR_globalGNB_ID;
    target_node.globalRANNodeID.choice.globalGNB_ID =
        static_cast<GlobalGNB_ID_t*>(std::calloc(1, sizeof(GlobalGNB_ID_t)));
    fill_global_gnb_id(*target_node.globalRANNodeID.choice.globalGNB_ID, target_gnb_id);
    std::uint8_t plmn[3];
    encode_plmn_identity(kMcc, kMnc, plmn);
    target_node.selectedTAI.pLMNIdentity = make_octet_string(plmn, 3);
    const std::uint8_t tac[3] = {0x00, 0x00, 0x01};
    target_node.selectedTAI.tAC = make_octet_string(tac, 3);
    ::ngap::add_ie(
        required.protocolIEs,
        ::ngap::make_ie(105 /* id-TargetID */, Criticality_reject, &asn_DEF_TargetID, &target_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_TargetID, &target_id);

    PDUSessionResourceListHORqd_t pdu_list{};
    auto* item = static_cast<PDUSessionResourceItemHORqd_t*>(
        std::calloc(1, sizeof(PDUSessionResourceItemHORqd_t)));
    item->pDUSessionID = pdu_session_id;
    // The transparent container is opaque to AMF -- it relays it to the target gNB verbatim. A
    // short, non-empty marker is honest here: this driver is not a real RRC stack and does not
    // pretend the bytes are a real HandoverPreparationInformation.
    const std::uint8_t opaque[4] = {0xDE, 0xAD, 0xBE, 0xEF};
    item->handoverRequiredTransfer = make_octet_string(opaque, 4);
    ASN_SEQUENCE_ADD(&pdu_list.list, item);
    ::ngap::add_ie(required.protocolIEs,
                   ::ngap::make_ie(61 /* id-PDUSessionResourceListHORqd */,
                                   Criticality_reject,
                                   &asn_DEF_PDUSessionResourceListHORqd,
                                   &pdu_list));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PDUSessionResourceListHORqd, &pdu_list);

    SourceToTarget_TransparentContainer_t s2t = make_octet_string(opaque, 4);
    ::ngap::add_ie(required.protocolIEs,
                   ::ngap::make_ie(101 /* id-SourceToTarget-TransparentContainer */,
                                   Criticality_reject,
                                   &asn_DEF_SourceToTarget_TransparentContainer,
                                   &s2t));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_SourceToTarget_TransparentContainer, &s2t);

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = kProcHandoverPreparation;
    pdu.choice.initiatingMessage->criticality = Criticality_reject;
    pdu.choice.initiatingMessage->value.present = InitiatingMessage__value_PR_HandoverRequired;
    pdu.choice.initiatingMessage->value.choice.HandoverRequired = required;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

// --- ADR-0269: the target gNB's half of Handover Resource Allocation (TS 38.413 §8.4.3) -------

namespace {

// This target gNB's own DL N3 tunnel -- the endpoint it tells the core to send downlink user
// plane to once the UE arrives. Deliberately distinct from UPF's own N3 address so a transfer
// that ends up carrying the wrong side's tunnel is visible rather than plausible.
constexpr std::uint32_t kTargetGnbDlTeid = 0x0000BEEF;
constexpr std::uint8_t kTargetGnbIpv4[4] = {127, 0, 0, 22};

// The QoS flow this lab's SM policy establishes (PCF's own default 5QI flow, QFI 1) -- the flow a
// target gNB would be admitting here. Nothing in the current relay decodes it (see ADR-0269's own
// disclosure that AMF does not yet call SMF with HANDOVER_REQ_ACK), so this is honesty about what
// the message says, not a value any assertion depends on.
constexpr long kAdmittedQfi = 1;

// A real gTPTunnel CHOICE for this gNB's own DL endpoint, heap-allocated the way the generated
// codec expects so ASN_STRUCT_FREE_CONTENTS_ONLY owns it. Same shape as
// tests/integration/test_smf_n2sminfo_dispatch.cpp's own fill_gtp_tunnel, for the same reason.
void fill_target_dl_tunnel(UPTransportLayerInformation_t& tnl) {
    tnl.present = UPTransportLayerInformation_PR_gTPTunnel;
    auto* tunnel = static_cast<GTPTunnel_t*>(std::calloc(1, sizeof(GTPTunnel_t)));
    const std::uint8_t teid[4] = {static_cast<std::uint8_t>((kTargetGnbDlTeid >> 24) & 0xFF),
                                  static_cast<std::uint8_t>((kTargetGnbDlTeid >> 16) & 0xFF),
                                  static_cast<std::uint8_t>((kTargetGnbDlTeid >> 8) & 0xFF),
                                  static_cast<std::uint8_t>(kTargetGnbDlTeid & 0xFF)};
    tunnel->gTP_TEID = make_octet_string(teid, 4);
    tunnel->transportLayerAddress.buf = static_cast<std::uint8_t*>(std::malloc(4));
    std::memcpy(tunnel->transportLayerAddress.buf, kTargetGnbIpv4, 4);
    tunnel->transportLayerAddress.size = 4;
    tunnel->transportLayerAddress.bits_unused = 0;
    tnl.choice.gTPTunnel = tunnel;
}

// The real inner structure PDUSessionResourceAdmittedItem carries. Both fields written here are
// MANDATORY in the ASN.1 (specs/NGAP/ngap-17.9.asn:6454) -- dL-NGU-UP-TNLInformation and
// qosFlowSetupResponseList; everything else in it is OPTIONAL and omitted.
std::vector<std::uint8_t> encode_handover_request_acknowledge_transfer() {
    HandoverRequestAcknowledgeTransfer_t transfer{};
    fill_target_dl_tunnel(transfer.dL_NGU_UP_TNLInformation);
    auto* flow = static_cast<QosFlowItemWithDataForwarding_t*>(
        std::calloc(1, sizeof(QosFlowItemWithDataForwarding_t)));
    flow->qosFlowIdentifier = kAdmittedQfi;
    ASN_SEQUENCE_ADD(&transfer.qosFlowSetupResponseList.list, flow);
    const auto bytes = ::ngap::encode_value(&asn_DEF_HandoverRequestAcknowledgeTransfer, &transfer);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_HandoverRequestAcknowledgeTransfer, &transfer);
    return bytes;
}

} // namespace

bool NgapTestGnb::parse_handover_request(const std::vector<std::uint8_t>& pdu_bytes,
                                         HandoverRequestInfo& out) {
    NGAP_PDU_t* pdu = ::ngap::decode_pdu(pdu_bytes);
    if (pdu == nullptr) {
        return false;
    }
    if (pdu->present != NGAP_PDU_PR_initiatingMessage ||
        pdu->choice.initiatingMessage->procedureCode != kProcHandoverResourceAllocation) {
        ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
        return false;
    }
    const auto& container = pdu->choice.initiatingMessage->value.choice.HandoverRequest.protocolIEs;

    bool ok = false;
    const auto* amf_id_ie = ::ngap::find_ie(container, 10 /* id-AMF-UE-NGAP-ID */);
    if (amf_id_ie != nullptr) {
        auto* amf_id = static_cast<AMF_UE_NGAP_ID_t*>(
            ::ngap::decode_ie_value(&asn_DEF_AMF_UE_NGAP_ID, *amf_id_ie));
        if (amf_id != nullptr) {
            unsigned long value = 0;
            if (asn_INTEGER2ulong(amf_id, &value) == 0) {
                out.amf_ue_id = value;
                ok = true;
            }
            ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, amf_id);
        }
    }

    // The sessions AMF really prepared. An empty list is not an error here -- it is a real answer
    // (AMF sends no list at all when it could prepare none), and the caller's own assertion is a
    // better place to say so than a silent false.
    out.pdu_session_ids.clear();
    const auto* setup_ie = ::ngap::find_ie(container, 73 /* id-PDUSessionResourceSetupListHOReq */);
    if (setup_ie != nullptr) {
        auto* setup_list = static_cast<PDUSessionResourceSetupListHOReq_t*>(
            ::ngap::decode_ie_value(&asn_DEF_PDUSessionResourceSetupListHOReq, *setup_ie));
        if (setup_list != nullptr) {
            for (int i = 0; i < setup_list->list.count; ++i) {
                out.pdu_session_ids.push_back(
                    static_cast<std::uint8_t>(setup_list->list.array[i]->pDUSessionID));
            }
            ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceSetupListHOReq, setup_list);
        }
    }

    ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
    return ok;
}

std::vector<std::uint8_t>
NgapTestGnb::build_handover_request_acknowledge(std::uint64_t amf_ue_id,
                                                std::uint32_t ran_ue_id,
                                                const std::vector<std::uint8_t>& pdu_session_ids) {
    HandoverRequestAcknowledge_t ack{};

    AMF_UE_NGAP_ID_t amf_id{};
    asn_ulong2INTEGER(&amf_id, static_cast<unsigned long>(amf_ue_id));
    ::ngap::add_ie(
        ack.protocolIEs,
        ::ngap::make_ie(
            10 /* id-AMF-UE-NGAP-ID */, Criticality_ignore, &asn_DEF_AMF_UE_NGAP_ID, &amf_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_id);

    RAN_UE_NGAP_ID_t ran_id = static_cast<RAN_UE_NGAP_ID_t>(ran_ue_id);
    ::ngap::add_ie(
        ack.protocolIEs,
        ::ngap::make_ie(
            85 /* id-RAN-UE-NGAP-ID */, Criticality_ignore, &asn_DEF_RAN_UE_NGAP_ID, &ran_id));

    // PDUSessionResourceAdmittedList is MANDATORY and SIZE(1..maxnoofPDUSessions) -- a zero-length
    // list would not be a legal encoding, so a caller with nothing to admit must send a
    // HandoverFailure instead. Returning empty bytes says that here rather than emitting an
    // out-of-constraint PDU that AMF would then fail to decode for a confusing reason.
    if (pdu_session_ids.empty()) {
        ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_HandoverRequestAcknowledge, &ack);
        return {};
    }
    const auto transfer = encode_handover_request_acknowledge_transfer();
    if (transfer.empty()) {
        ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_HandoverRequestAcknowledge, &ack);
        return {};
    }
    PDUSessionResourceAdmittedList_t admitted{};
    for (const std::uint8_t session_id : pdu_session_ids) {
        auto* item = static_cast<PDUSessionResourceAdmittedItem_t*>(
            std::calloc(1, sizeof(PDUSessionResourceAdmittedItem_t)));
        item->pDUSessionID = session_id;
        item->handoverRequestAcknowledgeTransfer =
            make_octet_string(transfer.data(), transfer.size());
        ASN_SEQUENCE_ADD(&admitted.list, item);
    }
    ::ngap::add_ie(ack.protocolIEs,
                   ::ngap::make_ie(53 /* id-PDUSessionResourceAdmittedList */,
                                   Criticality_ignore,
                                   &asn_DEF_PDUSessionResourceAdmittedList,
                                   &admitted));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PDUSessionResourceAdmittedList, &admitted);

    // Opaque to AMF by design (TS 38.413 §9.3.1.32) -- it relays these bytes to the source gNB
    // without decoding them, so a short marker is honest: this driver is not a real RRC stack and
    // does not pretend to produce a real HandoverCommand RRC container.
    const std::uint8_t opaque[4] = {0xCA, 0xFE, 0xBA, 0xBE};
    TargetToSource_TransparentContainer_t t2s = make_octet_string(opaque, 4);
    ::ngap::add_ie(ack.protocolIEs,
                   ::ngap::make_ie(106 /* id-TargetToSource-TransparentContainer */,
                                   Criticality_reject,
                                   &asn_DEF_TargetToSource_TransparentContainer,
                                   &t2s));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_TargetToSource_TransparentContainer, &t2s);

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_successfulOutcome;
    pdu.choice.successfulOutcome =
        static_cast<SuccessfulOutcome_t*>(std::calloc(1, sizeof(SuccessfulOutcome_t)));
    pdu.choice.successfulOutcome->procedureCode = kProcHandoverResourceAllocation;
    pdu.choice.successfulOutcome->criticality = Criticality_reject;
    pdu.choice.successfulOutcome->value.present =
        SuccessfulOutcome__value_PR_HandoverRequestAcknowledge;
    pdu.choice.successfulOutcome->value.choice.HandoverRequestAcknowledge = ack;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

std::vector<std::uint8_t> NgapTestGnb::build_ue_context_release_complete(std::uint64_t amf_ue_id,
                                                                         std::uint32_t ran_ue_id) {
    UEContextReleaseComplete_t complete{};

    AMF_UE_NGAP_ID_t amf_id{};
    asn_ulong2INTEGER(&amf_id, static_cast<unsigned long>(amf_ue_id));
    ::ngap::add_ie(
        complete.protocolIEs,
        ::ngap::make_ie(
            10 /* id-AMF-UE-NGAP-ID */, Criticality_ignore, &asn_DEF_AMF_UE_NGAP_ID, &amf_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_id);

    RAN_UE_NGAP_ID_t ran_id = static_cast<RAN_UE_NGAP_ID_t>(ran_ue_id);
    ::ngap::add_ie(
        complete.protocolIEs,
        ::ngap::make_ie(
            85 /* id-RAN-UE-NGAP-ID */, Criticality_ignore, &asn_DEF_RAN_UE_NGAP_ID, &ran_id));

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_successfulOutcome;
    pdu.choice.successfulOutcome =
        static_cast<SuccessfulOutcome_t*>(std::calloc(1, sizeof(SuccessfulOutcome_t)));
    pdu.choice.successfulOutcome->procedureCode = kProcUeContextRelease;
    pdu.choice.successfulOutcome->criticality = Criticality_reject;
    pdu.choice.successfulOutcome->value.present =
        SuccessfulOutcome__value_PR_UEContextReleaseComplete;
    pdu.choice.successfulOutcome->value.choice.UEContextReleaseComplete = complete;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

bool NgapTestGnb::parse_handover_command(const std::vector<std::uint8_t>& pdu_bytes,
                                         std::vector<std::uint8_t>& switched_pdu_session_ids) {
    switched_pdu_session_ids.clear();
    NGAP_PDU_t* pdu = ::ngap::decode_pdu(pdu_bytes);
    if (pdu == nullptr) {
        return false;
    }
    if (pdu->present != NGAP_PDU_PR_successfulOutcome ||
        pdu->choice.successfulOutcome->procedureCode != kProcHandoverPreparation) {
        ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
        return false;
    }
    const auto& container = pdu->choice.successfulOutcome->value.choice.HandoverCommand.protocolIEs;
    const auto* list_ie = ::ngap::find_ie(container, 59 /* id-PDUSessionResourceHandoverList */);
    if (list_ie != nullptr) {
        auto* list = static_cast<PDUSessionResourceHandoverList_t*>(
            ::ngap::decode_ie_value(&asn_DEF_PDUSessionResourceHandoverList, *list_ie));
        if (list != nullptr) {
            for (int i = 0; i < list->list.count; ++i) {
                switched_pdu_session_ids.push_back(
                    static_cast<std::uint8_t>(list->list.array[i]->pDUSessionID));
            }
            ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceHandoverList, list);
        }
    }
    ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
    return true;
}

std::vector<std::uint8_t> NgapTestGnb::build_handover_cancel(std::uint64_t amf_ue_id,
                                                             std::uint32_t ran_ue_id) {
    HandoverCancel_t cancel{};

    AMF_UE_NGAP_ID_t amf_id{};
    asn_ulong2INTEGER(&amf_id, static_cast<unsigned long>(amf_ue_id));
    ::ngap::add_ie(
        cancel.protocolIEs,
        ::ngap::make_ie(
            10 /* id-AMF-UE-NGAP-ID */, Criticality_reject, &asn_DEF_AMF_UE_NGAP_ID, &amf_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_id);

    RAN_UE_NGAP_ID_t ran_id = static_cast<RAN_UE_NGAP_ID_t>(ran_ue_id);
    ::ngap::add_ie(
        cancel.protocolIEs,
        ::ngap::make_ie(
            85 /* id-RAN-UE-NGAP-ID */, Criticality_reject, &asn_DEF_RAN_UE_NGAP_ID, &ran_id));

    // handover-cancelled: the real, spec-defined cause for a source gNB abandoning a handover.
    Cause_t cause{};
    cause.present = Cause_PR_radioNetwork;
    cause.choice.radioNetwork = CauseRadioNetwork_handover_cancelled;
    ::ngap::add_ie(cancel.protocolIEs,
                   ::ngap::make_ie(15 /* id-Cause */, Criticality_ignore, &asn_DEF_Cause, &cause));

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = kProcHandoverCancel;
    pdu.choice.initiatingMessage->criticality = Criticality_reject;
    pdu.choice.initiatingMessage->value.present = InitiatingMessage__value_PR_HandoverCancel;
    pdu.choice.initiatingMessage->value.choice.HandoverCancel = cancel;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

namespace {

// Mandatory on both InitialUEMessage and UplinkNASTransport. A real, structurally-valid NR CGI +
// TAI in this lab's own PLMN -- AMF logs it and does not act on it, so a fixed cell is honest.
void fill_user_location(UserLocationInformation_t& uli, std::uint64_t nr_cell_id = 1) {
    uli.present = UserLocationInformation_PR_userLocationInformationNR;
    auto* nr = static_cast<UserLocationInformationNR_t*>(
        std::calloc(1, sizeof(UserLocationInformationNR_t)));
    std::uint8_t plmn[3];
    encode_plmn_identity(kMcc, kMnc, plmn);
    nr->nR_CGI.pLMNIdentity = make_octet_string(plmn, 3);
    nr->nR_CGI.nRCellIdentity = make_bit_string_from_uint(nr_cell_id, 36);
    nr->tAI.pLMNIdentity = make_octet_string(plmn, 3);
    const std::uint8_t tac[3] = {0x00, 0x00, 0x01};
    nr->tAI.tAC = make_octet_string(tac, 3);
    uli.choice.userLocationInformationNR = nr;
}

} // namespace

std::vector<std::uint8_t>
NgapTestGnb::build_initial_ue_message(std::uint32_t ran_ue_id,
                                      const std::vector<std::uint8_t>& nas_pdu) {
    InitialUEMessage_t msg{};

    RAN_UE_NGAP_ID_t ran_id = static_cast<RAN_UE_NGAP_ID_t>(ran_ue_id);
    ::ngap::add_ie(
        msg.protocolIEs,
        ::ngap::make_ie(
            85 /* id-RAN-UE-NGAP-ID */, Criticality_reject, &asn_DEF_RAN_UE_NGAP_ID, &ran_id));

    NAS_PDU_t nas = make_octet_string(nas_pdu.data(), nas_pdu.size());
    ::ngap::add_ie(
        msg.protocolIEs,
        ::ngap::make_ie(38 /* id-NAS-PDU */, Criticality_reject, &asn_DEF_NAS_PDU, &nas));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NAS_PDU, &nas);

    UserLocationInformation_t uli{};
    fill_user_location(uli);
    ::ngap::add_ie(msg.protocolIEs,
                   ::ngap::make_ie(121 /* id-UserLocationInformation */,
                                   Criticality_reject,
                                   &asn_DEF_UserLocationInformation,
                                   &uli));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_UserLocationInformation, &uli);

    RRCEstablishmentCause_t cause = RRCEstablishmentCause_mo_Signalling;
    ::ngap::add_ie(msg.protocolIEs,
                   ::ngap::make_ie(90 /* id-RRCEstablishmentCause */,
                                   Criticality_ignore,
                                   &asn_DEF_RRCEstablishmentCause,
                                   &cause));

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = kProcInitialUeMessage;
    pdu.choice.initiatingMessage->criticality = Criticality_ignore;
    pdu.choice.initiatingMessage->value.present = InitiatingMessage__value_PR_InitialUEMessage;
    pdu.choice.initiatingMessage->value.choice.InitialUEMessage = msg;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

std::vector<std::uint8_t> NgapTestGnb::build_handover_failure(std::uint64_t amf_ue_id) {
    HandoverFailure_t failure{};

    AMF_UE_NGAP_ID_t amf_id{};
    asn_ulong2INTEGER(&amf_id, static_cast<unsigned long>(amf_ue_id));
    ::ngap::add_ie(
        failure.protocolIEs,
        ::ngap::make_ie(
            10 /* id-AMF-UE-NGAP-ID */, Criticality_ignore, &asn_DEF_AMF_UE_NGAP_ID, &amf_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_id);

    // A real reason a target gNB refuses: it has no radio resources left in the target cell.
    Cause_t cause{};
    cause.present = Cause_PR_radioNetwork;
    cause.choice.radioNetwork = CauseRadioNetwork_no_radio_resources_available_in_target_cell;
    ::ngap::add_ie(failure.protocolIEs,
                   ::ngap::make_ie(15 /* id-Cause */, Criticality_ignore, &asn_DEF_Cause, &cause));

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_unsuccessfulOutcome;
    pdu.choice.unsuccessfulOutcome =
        static_cast<UnsuccessfulOutcome_t*>(std::calloc(1, sizeof(UnsuccessfulOutcome_t)));
    pdu.choice.unsuccessfulOutcome->procedureCode = kProcHandoverResourceAllocation;
    pdu.choice.unsuccessfulOutcome->criticality = Criticality_reject;
    pdu.choice.unsuccessfulOutcome->value.present = UnsuccessfulOutcome__value_PR_HandoverFailure;
    pdu.choice.unsuccessfulOutcome->value.choice.HandoverFailure = failure;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

bool NgapTestGnb::parse_handover_preparation_failure_cause(
    const std::vector<std::uint8_t>& pdu_bytes, long& radio_network_cause) {
    NGAP_PDU_t* pdu = ::ngap::decode_pdu(pdu_bytes);
    if (pdu == nullptr) {
        return false;
    }
    if (pdu->present != NGAP_PDU_PR_unsuccessfulOutcome ||
        pdu->choice.unsuccessfulOutcome->procedureCode != kProcHandoverPreparation) {
        ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
        return false;
    }
    const auto& container =
        pdu->choice.unsuccessfulOutcome->value.choice.HandoverPreparationFailure.protocolIEs;
    bool ok = false;
    const auto* cause_ie = ::ngap::find_ie(container, 15 /* id-Cause */);
    if (cause_ie != nullptr) {
        auto* cause = static_cast<Cause_t*>(::ngap::decode_ie_value(&asn_DEF_Cause, *cause_ie));
        if (cause != nullptr) {
            if (cause->present == Cause_PR_radioNetwork) {
                radio_network_cause = cause->choice.radioNetwork;
                ok = true;
            }
            ASN_STRUCT_FREE(asn_DEF_Cause, cause);
        }
    }
    ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
    return ok;
}

bool NgapTestGnb::parse_ue_context_release_command_nas_cause(
    const std::vector<std::uint8_t>& pdu_bytes, long& nas_cause) {
    NGAP_PDU_t* pdu = ::ngap::decode_pdu(pdu_bytes);
    if (pdu == nullptr) {
        return false;
    }
    if (pdu->present != NGAP_PDU_PR_initiatingMessage ||
        pdu->choice.initiatingMessage->procedureCode != kProcUeContextRelease) {
        ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
        return false;
    }
    const auto& container =
        pdu->choice.initiatingMessage->value.choice.UEContextReleaseCommand.protocolIEs;
    bool ok = false;
    const auto* cause_ie = ::ngap::find_ie(container, 15 /* id-Cause */);
    if (cause_ie != nullptr) {
        auto* cause = static_cast<Cause_t*>(::ngap::decode_ie_value(&asn_DEF_Cause, *cause_ie));
        if (cause != nullptr) {
            if (cause->present == Cause_PR_nas) {
                nas_cause = cause->choice.nas;
                ok = true;
            }
            ASN_STRUCT_FREE(asn_DEF_Cause, cause);
        }
    }
    ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
    return ok;
}

std::vector<std::uint8_t> NgapTestGnb::build_handover_notify(std::uint64_t amf_ue_id,
                                                             std::uint32_t ran_ue_id,
                                                             std::uint64_t nr_cell_id) {
    HandoverNotify_t notify{};

    AMF_UE_NGAP_ID_t amf_id{};
    asn_ulong2INTEGER(&amf_id, static_cast<unsigned long>(amf_ue_id));
    ::ngap::add_ie(
        notify.protocolIEs,
        ::ngap::make_ie(
            10 /* id-AMF-UE-NGAP-ID */, Criticality_reject, &asn_DEF_AMF_UE_NGAP_ID, &amf_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_id);

    RAN_UE_NGAP_ID_t ran_id = static_cast<RAN_UE_NGAP_ID_t>(ran_ue_id);
    ::ngap::add_ie(
        notify.protocolIEs,
        ::ngap::make_ie(
            85 /* id-RAN-UE-NGAP-ID */, Criticality_reject, &asn_DEF_RAN_UE_NGAP_ID, &ran_id));

    UserLocationInformation_t uli{};
    fill_user_location(uli, nr_cell_id);
    ::ngap::add_ie(notify.protocolIEs,
                   ::ngap::make_ie(121 /* id-UserLocationInformation */,
                                   Criticality_ignore,
                                   &asn_DEF_UserLocationInformation,
                                   &uli));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_UserLocationInformation, &uli);

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = kProcHandoverNotification;
    pdu.choice.initiatingMessage->criticality = Criticality_ignore;
    pdu.choice.initiatingMessage->value.present = InitiatingMessage__value_PR_HandoverNotify;
    pdu.choice.initiatingMessage->value.choice.HandoverNotify = notify;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

std::vector<std::uint8_t> NgapTestGnb::build_uplink_nas_transport(
    std::uint64_t amf_ue_id, std::uint32_t ran_ue_id, const std::vector<std::uint8_t>& nas_pdu) {
    UplinkNASTransport_t msg{};

    AMF_UE_NGAP_ID_t amf_id{};
    asn_ulong2INTEGER(&amf_id, static_cast<unsigned long>(amf_ue_id));
    ::ngap::add_ie(
        msg.protocolIEs,
        ::ngap::make_ie(
            10 /* id-AMF-UE-NGAP-ID */, Criticality_reject, &asn_DEF_AMF_UE_NGAP_ID, &amf_id));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_id);

    RAN_UE_NGAP_ID_t ran_id = static_cast<RAN_UE_NGAP_ID_t>(ran_ue_id);
    ::ngap::add_ie(
        msg.protocolIEs,
        ::ngap::make_ie(
            85 /* id-RAN-UE-NGAP-ID */, Criticality_reject, &asn_DEF_RAN_UE_NGAP_ID, &ran_id));

    NAS_PDU_t nas = make_octet_string(nas_pdu.data(), nas_pdu.size());
    ::ngap::add_ie(
        msg.protocolIEs,
        ::ngap::make_ie(38 /* id-NAS-PDU */, Criticality_reject, &asn_DEF_NAS_PDU, &nas));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NAS_PDU, &nas);

    UserLocationInformation_t uli{};
    fill_user_location(uli);
    ::ngap::add_ie(msg.protocolIEs,
                   ::ngap::make_ie(121 /* id-UserLocationInformation */,
                                   Criticality_ignore,
                                   &asn_DEF_UserLocationInformation,
                                   &uli));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_UserLocationInformation, &uli);

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = kProcUplinkNasTransport;
    pdu.choice.initiatingMessage->criticality = Criticality_ignore;
    pdu.choice.initiatingMessage->value.present = InitiatingMessage__value_PR_UplinkNASTransport;
    pdu.choice.initiatingMessage->value.choice.UplinkNASTransport = msg;
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    return bytes;
}

bool NgapTestGnb::extract_downlink_nas(const std::vector<std::uint8_t>& pdu_bytes,
                                       DownlinkNas& out) {
    NGAP_PDU_t* pdu = ::ngap::decode_pdu(pdu_bytes);
    if (pdu == nullptr) {
        return false;
    }
    bool ok = false;
    if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
        pdu->choice.initiatingMessage->procedureCode == kProcDownlinkNasTransport) {
        const auto& container =
            pdu->choice.initiatingMessage->value.choice.DownlinkNASTransport.protocolIEs;
        const auto* amf_id_ie = ::ngap::find_ie(container, 10 /* id-AMF-UE-NGAP-ID */);
        const auto* nas_ie = ::ngap::find_ie(container, 38 /* id-NAS-PDU */);
        if (amf_id_ie != nullptr && nas_ie != nullptr) {
            auto* amf_id = static_cast<AMF_UE_NGAP_ID_t*>(
                ::ngap::decode_ie_value(&asn_DEF_AMF_UE_NGAP_ID, *amf_id_ie));
            auto* nas = static_cast<NAS_PDU_t*>(::ngap::decode_ie_value(&asn_DEF_NAS_PDU, *nas_ie));
            if (amf_id != nullptr && nas != nullptr) {
                unsigned long value = 0;
                asn_INTEGER2ulong(amf_id, &value);
                out.amf_ue_id = value;
                out.nas_pdu.assign(nas->buf, nas->buf + nas->size);
                ok = true;
            }
            if (amf_id != nullptr) {
                ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, amf_id);
            }
            if (nas != nullptr) {
                ASN_STRUCT_FREE(asn_DEF_NAS_PDU, nas);
            }
        }
    }
    ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
    return ok;
}

NgapTestGnb::PduSummary NgapTestGnb::summarize(const std::vector<std::uint8_t>& pdu_bytes) {
    PduSummary summary;
    if (pdu_bytes.empty()) {
        return summary;
    }
    NGAP_PDU_t* pdu = ::ngap::decode_pdu(pdu_bytes);
    if (pdu == nullptr) {
        return summary;
    }
    switch (pdu->present) {
        case NGAP_PDU_PR_initiatingMessage:
            summary.outcome = Outcome::Initiating;
            summary.procedure_code = pdu->choice.initiatingMessage->procedureCode;
            break;
        case NGAP_PDU_PR_successfulOutcome:
            summary.outcome = Outcome::Successful;
            summary.procedure_code = pdu->choice.successfulOutcome->procedureCode;
            break;
        case NGAP_PDU_PR_unsuccessfulOutcome:
            summary.outcome = Outcome::Unsuccessful;
            summary.procedure_code = pdu->choice.unsuccessfulOutcome->procedureCode;
            break;
        default:
            break;
    }
    ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
    return summary;
}

} // namespace nf_test
