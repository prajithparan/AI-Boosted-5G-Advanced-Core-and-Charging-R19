#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

// ADR-0264: a real gNB, for tests.
//
// Why this exists. Every NGAP procedure this project has built -- the whole N2 handover chain
// across ADR-0090/0095/0096/0248/0249/0258/0261 -- has been verified by construction and review,
// never end to end, because UERANSIM (ADR-0016, the vendored RAN simulator) implements NO handover
// procedure in its gNB at all: `HandoverRequired` appears in its generated ASN.1 and never once in
// `src/gnb/`. That is a gap in verification, not in the simulator's honesty, and it is why
// `SctpSocket::connect()` was added back at ADR-0090 for "a real hand-crafted NGAP test client"
// that was never committed. This is that client, committed.
//
// What it is: a real SCTP association to AMF's real N2 listener, carrying real Aligned-PER NGAP
// PDUs built with this project's own `ngap_core` codec and `ngap_generated` ASN.1 -- the same
// codec AMF itself uses. It is NOT a simulated or stubbed transport.
//
// Deliberate design choice: this header exposes no ASN.1 type. Tests deal in byte vectors and the
// small set of questions below, so a test including this does not pull the generated NGAP headers
// into its own translation unit, and the ASN.1 lifetime rules stay inside the .cpp where they can
// be got right once.

namespace nf_test {

class NgapTestGnb {
public:
    NgapTestGnb();
    ~NgapTestGnb();

    NgapTestGnb(const NgapTestGnb&) = delete;
    NgapTestGnb& operator=(const NgapTestGnb&) = delete;

    // Establishes the SCTP association. Retries because AMF's NGAP listener comes up on its own
    // thread after the process starts. Returns false if it never became connectable.
    bool connect(const std::string& address, std::uint16_t port, int max_attempts = 80);

    // Real NGSetupRequest carrying a real GlobalRANNodeID/globalGNB-ID (PLMNIdentity + a 32-bit
    // GNB-ID bit string), then awaits the response. Returns true only on a real NGSetupResponse.
    // `gnb_id` distinguishes source from target when a test runs two of these.
    bool ng_setup(std::uint32_t gnb_id);

    void send_raw(const std::vector<std::uint8_t>& pdu_bytes);

    // Blocking receive of one PDU. Empty vector means the peer closed the association.
    std::vector<std::uint8_t> receive_raw();

    // --- PDU builders. Each returns real PER bytes, or empty on an encode failure. ---

    // Mandatory IE set per HandoverRequiredIEs (specs/NGAP/ngap-17.9.asn): AMF-UE-NGAP-ID(10),
    // RAN-UE-NGAP-ID(85), HandoverType(29), Cause(15), TargetID(105),
    // PDUSessionResourceListHORqd(61), SourceToTarget-TransparentContainer(101). `target_gnb_id`
    // names the gNB this handover is towards, in the same GlobalGNB-ID form ng_setup registers.
    std::vector<std::uint8_t> build_handover_required(std::uint64_t amf_ue_id,
                                                      std::uint32_t ran_ue_id,
                                                      std::uint32_t target_gnb_id,
                                                      std::uint8_t pdu_session_id);

    // Mandatory IE set per HandoverCancelIEs: AMF-UE-NGAP-ID(10), RAN-UE-NGAP-ID(85), Cause(15).
    std::vector<std::uint8_t> build_handover_cancel(std::uint64_t amf_ue_id,
                                                    std::uint32_t ran_ue_id);

    // --- Target-gNB side of the relay (ADR-0269). ---

    // What a target gNB needs out of the HandoverRequest AMF sends it: the AMF-UE-NGAP-ID it must
    // echo, and the PDU sessions AMF actually managed to prepare. Both are read from the real
    // message rather than assumed, so an acknowledge always answers about the sessions AMF asked
    // for -- AMF SKIPS any session SMF could not answer for (ADR-0258), so the list is a real
    // answer to a real question, not a constant.
    struct HandoverRequestInfo {
        std::uint64_t amf_ue_id = 0;
        std::vector<std::uint8_t> pdu_session_ids;
    };
    static bool parse_handover_request(const std::vector<std::uint8_t>& pdu_bytes,
                                       HandoverRequestInfo& out);

    // Real HandoverRequestAcknowledge (TS 38.413 §9.2.3.2). Mandatory IE set per
    // HandoverRequestAcknowledgeIEs: AMF-UE-NGAP-ID(10), RAN-UE-NGAP-ID(85),
    // PDUSessionResourceAdmittedList(53), TargetToSource-TransparentContainer(106).
    //
    // `ran_ue_id` is the TARGET's own newly allocated RAN-UE-NGAP-ID, not the source's -- the
    // target gNB assigns it, which is the whole reason the IE is in this message.
    //
    // Each admitted session carries a REAL PER-encoded HandoverRequestAcknowledgeTransfer (the
    // target's own DL N3 tunnel plus the QoS flows it accepted), not an opaque marker: the ASN.1
    // makes it `OCTET STRING (CONTAINING HandoverRequestAcknowledgeTransfer)`, a structure with a
    // real consumer, unlike the transparent containers which are opaque by design.
    std::vector<std::uint8_t>
    build_handover_request_acknowledge(std::uint64_t amf_ue_id,
                                       std::uint32_t ran_ue_id,
                                       const std::vector<std::uint8_t>& pdu_session_ids);

    // Pulls the PDU session IDs out of a HandoverCommand's PDUSessionResourceHandoverList. That
    // IE is OPTIONAL in the ASN.1 and AMF only fills it for sessions SMF confirmed it had really
    // switched to the target's downlink tunnel (ADR-0270) -- so an empty result is the real
    // signal that no session's downlink moved, not a parse failure. Returns false only if the PDU
    // is not a HandoverCommand at all.
    static bool parse_handover_command(const std::vector<std::uint8_t>& pdu_bytes,
                                       std::vector<std::uint8_t>& switched_pdu_session_ids);

    // Real HandoverFailure (TS 38.413 §9.2.3.3) -- the target gNB REJECTING a handover it cannot
    // admit. Mandatory IE set per HandoverFailureIEs: AMF-UE-NGAP-ID(10), Cause(15). AMF relays
    // that cause to the source gNB inside its own HandoverPreparationFailure, so the cause a test
    // sends here is the one it should see come back out.
    std::vector<std::uint8_t> build_handover_failure(std::uint64_t amf_ue_id);

    // The radioNetwork Cause build_handover_failure sends, so a test can assert AMF relayed the
    // TARGET's cause rather than substituting one of its own. Value read from the generated
    // enum (CauseRadioNetwork_no_radio_resources_available_in_target_cell = 13), not remembered.
    static constexpr long kCauseRadioNoResourcesInTargetCell = 13;

    // Pulls the Cause out of a HandoverPreparationFailure. Returns false if the PDU is not one,
    // or carries no Cause.
    static bool parse_handover_preparation_failure_cause(const std::vector<std::uint8_t>& pdu_bytes,
                                                         long& radio_network_cause);

    // Real HandoverNotify (TS 38.413 §9.2.3.5) -- what the TARGET gNB sends once the UE has
    // actually arrived on it, opening the execution phase (TS 23.502 §4.9.1.3.3). Mandatory IE
    // set per HandoverNotifyIEs: AMF-UE-NGAP-ID(10), RAN-UE-NGAP-ID(85),
    // UserLocationInformation(121). `ran_ue_id` is the target's own, the one it allocated in its
    // HandoverRequestAcknowledge.
    std::vector<std::uint8_t> build_handover_notify(std::uint64_t amf_ue_id,
                                                    std::uint32_t ran_ue_id);

    // --- UE-associated signalling (ADR-0265), for procedures that need a registered UE. ---

    // Real InitialUEMessage carrying `nas_pdu`: id-RAN-UE-NGAP-ID(85), id-NAS-PDU(38), plus the
    // mandatory UserLocationInformation and RRCEstablishmentCause.
    std::vector<std::uint8_t> build_initial_ue_message(std::uint32_t ran_ue_id,
                                                       const std::vector<std::uint8_t>& nas_pdu);

    // Real UplinkNASTransport: id-AMF-UE-NGAP-ID(10), id-RAN-UE-NGAP-ID(85), id-NAS-PDU(38),
    // plus UserLocationInformation.
    std::vector<std::uint8_t> build_uplink_nas_transport(std::uint64_t amf_ue_id,
                                                         std::uint32_t ran_ue_id,
                                                         const std::vector<std::uint8_t>& nas_pdu);

    // Pulls the NAS-PDU and the AMF-UE-NGAP-ID out of a DownlinkNASTransport. Returns false if
    // the PDU is not one, or is missing either IE.
    struct DownlinkNas {
        std::uint64_t amf_ue_id = 0;
        std::vector<std::uint8_t> nas_pdu;
    };
    static bool extract_downlink_nas(const std::vector<std::uint8_t>& pdu_bytes, DownlinkNas& out);

    // ADR-0393: real UEContextReleaseComplete (TS 38.413 §9.2.1.11), the gNB's own confirmation of
    // a UEContextReleaseCommand -- needed for the Deregistration test, which drives a real
    // AMF-INITIATED release for the first time (every earlier NGAP test that reaches
    // UEContextReleaseCommand never sent a Complete back, so handle_ue_context_release_complete
    // and its ADR-0393 cleanup were never exercised end to end). Mandatory IE set per
    // UEContextReleaseComplete-IEs: AMF-UE-NGAP-ID(10), RAN-UE-NGAP-ID(85) -- both mandatory,
    // everything else in the real IE set is optional and not sent here.
    std::vector<std::uint8_t> build_ue_context_release_complete(std::uint64_t amf_ue_id,
                                                                std::uint32_t ran_ue_id);

    // ADR-0393: pulls the NAS Cause value out of a received UEContextReleaseCommand -- lets a
    // test distinguish the two real causes this AMF now sends (CauseNas_normal_release=0 for the
    // pre-existing RAN-initiated round trip, CauseNas_deregister=2 for the new AMF-INITIATED one)
    // rather than only checking that some UEContextReleaseCommand arrived. Returns false if the
    // PDU is not one, or its Cause is not the nas group.
    static bool
    parse_ue_context_release_command_nas_cause(const std::vector<std::uint8_t>& pdu_bytes,
                                               long& nas_cause);

    // --- Questions a test can ask about a received PDU, without touching ASN.1 itself. ---

    enum class Outcome { Initiating, Successful, Unsuccessful, Undecodable };

    struct PduSummary {
        Outcome outcome = Outcome::Undecodable;
        long procedure_code = -1;
    };

    static PduSummary summarize(const std::vector<std::uint8_t>& pdu_bytes);

    // NGAP procedure codes, read from specs/NGAP/ngap-17.9.asn rather than remembered.
    static constexpr long kProcDownlinkNasTransport = 4;
    static constexpr long kProcHandoverCancel = 10;
    static constexpr long kProcHandoverNotification = 11;
    static constexpr long kProcHandoverPreparation = 12;
    static constexpr long kProcHandoverResourceAllocation = 13;
    static constexpr long kProcInitialUeMessage = 15;
    static constexpr long kProcNgSetup = 21;
    static constexpr long kProcUeContextRelease = 41;
    static constexpr long kProcUplinkNasTransport = 46;

private:
    struct Impl;
    Impl* impl_;
};

} // namespace nf_test
