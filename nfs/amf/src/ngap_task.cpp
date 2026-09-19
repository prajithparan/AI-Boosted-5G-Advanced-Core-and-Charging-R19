#include "ngap_task.hpp"

#include "sbi_core/http2_client.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/multipart.hpp"
#include "sbi_core/oauth2_client.hpp"
#include "sbi_core/tls_config.hpp"

#include <nlohmann/json.hpp>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "TS26510_CommonData_grp.hpp"
#include "TS29509_Nausf_UEAuthentication.hpp"
#include "aka_crypto/hex.hpp"
#include "aka_crypto/kdf.hpp"
#include "amf_ue_id_index_store.hpp"
#include "gnb_association_registry.hpp"
#include "li_poi.hpp"
#include "nas_codec.hpp"
#include "ngap_core/ngap_codec.hpp"
#include "ngap_core/sctp_socket.hpp"
#include "ngap_handover.hpp"

extern "C" {
#include <AMF-UE-NGAP-ID.h>
#include <AMFName.h>
#include <AMFPointer.h>
#include <AMFRegionID.h>
#include <AMFSetID.h>
#include <AllocationAndRetentionPriority.h>
#include <AllowedNSSAI-Item.h>
#include <AllowedNSSAI.h>
#include <Cause.h>
#include <CauseNas.h>
#include <CauseProtocol.h>
#include <CauseRadioNetwork.h>
#include <DownlinkNASTransport.h>
#include <ErrorIndication.h>
#include <FiveQI.h>
#include <GNB-ID.h>
#include <GTP-TEID.h>
#include <GTPTunnel.h>
#include <GUAMI.h>
#include <GlobalGNB-ID.h>
#include <GlobalRANNodeID.h>
#include <HandoverCommand.h>
#include <HandoverFailure.h>
#include <HandoverNotify.h>
#include <HandoverPreparationFailure.h>
#include <HandoverRequest.h>
#include <HandoverRequestAcknowledge.h>
#include <HandoverRequestAcknowledgeTransfer.h>
#include <HandoverRequired.h>
#include <HandoverRequiredTransfer.h>
#include <HandoverType.h>
#include <InitialUEMessage.h>
#include <InitiatingMessage.h>
#include <NAS-PDU.h>
#include <NGAP-PDU.h>
#include <NGSetupRequest.h>
#include <NGSetupResponse.h>
#include <NonDynamic5QIDescriptor.h>
#include <NotifySourceNGRANNode.h>
#include <PDUSessionResourceAdmittedItem.h>
#include <PDUSessionResourceAdmittedList.h>
#include <PDUSessionResourceItemHORqd.h>
#include <PDUSessionResourceListHORqd.h>
#include <PDUSessionResourceSetupItemHOReq.h>
#include <PDUSessionResourceSetupListHOReq.h>
#include <PDUSessionResourceSetupRequestTransfer.h>
#include <PDUSessionResourceSwitchedItem.h>
#include <PDUSessionResourceSwitchedList.h>
#include <PDUSessionResourceToBeSwitchedDLItem.h>
#include <PDUSessionResourceToBeSwitchedDLList.h>
#include <PDUSessionType.h>
#include <PLMNIdentity.h>
#include <PLMNSupportItem.h>
#include <PLMNSupportList.h>
#include <PathSwitchRequest.h>
#include <PathSwitchRequestAcknowledge.h>
#include <PathSwitchRequestAcknowledgeTransfer.h>
#include <PathSwitchRequestFailure.h>
#include <ProcedureCode.h>
#include <QosCharacteristics.h>
#include <QosFlowIdentifier.h>
#include <QosFlowLevelQosParameters.h>
#include <QosFlowSetupRequestItem.h>
#include <QosFlowSetupRequestList.h>
#include <RAN-UE-NGAP-ID.h>
#include <RelativeAMFCapacity.h>
#include <S-NSSAI.h>
#include <SD.h>
#include <SST.h>
#include <SecurityContext.h>
#include <ServedGUAMIItem.h>
#include <ServedGUAMIList.h>
#include <SliceSupportItem.h>
#include <SliceSupportList.h>
#include <SourceToTarget-TransparentContainer.h>
#include <SuccessfulOutcome.h>
#include <TargetID.h>
#include <TargetRANNodeID.h>
#include <TargetToSource-TransparentContainer.h>
#include <UE-NGAP-IDs.h>
#include <UEAggregateMaximumBitRate.h>
#include <UEContextReleaseCommand.h>
#include <UEContextReleaseComplete.h>
#include <UEContextReleaseRequest.h>
#include <UESecurityCapabilities.h>
#include <UPTransportLayerInformation.h>
#include <UnsuccessfulOutcome.h>
#include <UplinkNASTransport.h>
#include <UserLocationInformation.h>
}

namespace amf::ngap {

namespace {

// This lab's fixed test PLMN/AMF identity, matching simulators/ransim/config/gnb.yaml's
// mcc=999/mnc=70/tac=1/sst=1/sd=1 (see docs/DECISIONS.md ADR-0016) -- not invented, the same
// values already used throughout this project's certs/UDM/PCF test configuration. AMF Region ID/
// Set ID/Pointer (TS 23.003 clause 2.10.1's 8+10+6-bit AMF Identifier split) are set to all-zero
// as an arbitrary, unambiguous lab placeholder -- disclosed, not sourced from any real deployment
// plan, chosen specifically to sidestep bit-packing direction ambiguity for this first stage
// (an all-zero bit pattern is unambiguous regardless of MSB/LSB-first packing convention).
constexpr const char* kMcc = "999";
constexpr const char* kMnc = "70";
constexpr std::uint8_t kSst = 1;
constexpr std::uint32_t kSd = 1;
constexpr const char* kAmfName = "5gc-r19-amf";

// Task #109 (ADR-0273): AUSF's, PCF's and SMF's base URLs, and this AMF's own advertised base,
// were four compile-time literals here (https://127.0.0.1:7782/7783/7779/7778). They now arrive
// in `peers` (amf::ngap::PeerEndpoints), loaded from config/amf.json in main() -- the standing
// rule is that a deployment endpoint never lives in a .cpp literal. The API roots they are
// concatenated with (/nausf-auth/v1, /npcf-am-policy-control/v1, /nsmf-pdusession/v1) stay here:
// those are the peers' 3GPP-defined service paths, not deployment configuration, and reaching
// into another NF's headers for them would break CLAUDE.md's "no NF includes another NF's private
// headers" rule.
//
// peers.self_base feeds PolicyAssociationRequest.notificationUri, a mandatory field PCF's schema
// requires even though nothing here implements a receiver for it yet -- the same disclosed,
// deliberately-deferred gap nfs/pcf/src/main.cpp's own file header states for both directions of
// this callback. Unchanged by the retrofit.

// TS 23.003 §28.3.2.5 serving network name format ("5G:mnc<3-digit>.mcc<3-digit>.
// 3gppnetwork.org"), zero-padded MNC even for this lab's 2-digit mnc=70 -- same string this
// project's own AUSF integration tests already use (tests/integration/test_ausf_ue_authentication.
// cpp), not invented here.
constexpr const char* kServingNetworkName = "5G:mnc070.mcc999.3gppnetwork.org";

// This lab's single-UE-at-a-time scope (see docs/DECISIONS.md ADR-0031) makes a simple
// monotonically increasing counter a correct, unambiguous AMF-UE-NGAP-ID allocator -- a real AMF
// serving many concurrent UEs would need a real allocation/reuse scheme.
std::atomic<unsigned long> g_next_amf_ue_ngap_id{1};

// The AMF's LI IRI-POI, set once at run_ngap_lifecycle entry (nullptr when LI is disabled). A
// file-scope pointer rather than a parameter threaded through the five-deep handler call chain
// (run_ngap_lifecycle -> run_association_thread -> handle_association -> handle_uplink_nas_transport
// -> handle_uplink_nas_transport_smc_complete), matching this file's existing g_next_amf_ue_ngap_id
// pattern: the POI is process-lifetime singleton state, exactly like that allocator. Read only on
// the NGAP task thread(s); set before any association is accepted.
LiPoi* g_li_poi = nullptr;

// Minimal per-association state carried from Stage 2's InitialUEMessage handler to Stage 3's
// UplinkNASTransport handler -- both are separate, otherwise-stateless per-message handlers, but
// Stage 3's AUSF confirmation call needs Stage 2's SUPI and the auth-context confirmation URL
// AUSF returned when it created the authentication context. This lab's single-UE-at-a-time scope
// (ADR-0031) makes one flat struct in handle_association's own stack frame correct and
// unambiguous -- a real AMF tracking many concurrent UEs would need this keyed by
// RAN-UE-NGAP-ID/AMF-UE-NGAP-ID in a real UE context store instead.
struct UeAuthState {
    std::string supi;
    // Full path AUSF returned in UEAuthenticationCtx._links["5g-aka"]["href"] (already includes
    // the auth context id) -- not reconstructed from parts, since AUSF owns that ID's format.
    std::string confirmation_path;
    // Raw UE Security Capability TLV value bytes captured from the RegistrationRequest (Stage 2),
    // replayed verbatim in Stage 4's SecurityModeCommand -- see
    // amf::nas::RegistrationRequestInfo::ue_security_capability's own comment for why.
    std::vector<std::uint8_t> ue_security_capability;
    // Allocated in Stage 2 (handle_initial_ue_message) and reused for every later
    // DownlinkNASTransport this association sends (Stage 4's SecurityModeCommand, and beyond) --
    // this lab's single-UE-at-a-time scope (ADR-0031) makes storing them here, once, correct.
    unsigned long amf_ue_id = 0;
    unsigned long ran_ue_id = 0;
    // Populated once Stage 4 derives them from KAMF (TS 33.501 Annex A.8) -- see
    // aka_crypto/kdf.hpp's derive_knas_int/derive_knas_enc.
    std::optional<aka_crypto::NasIntKey> knas_int;
    std::optional<aka_crypto::NasEncKey> knas_enc;
    // KAMF itself (TS 33.501 Annex A.6), stored alongside its two derived keys above -- gap-
    // closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075) needs the real root key
    // persisted (UeSecurityContextStore) once registration completes, not just its two derived
    // children, so a later ServiceRequest can re-derive knas_int/knas_enc the same way this
    // struct's own Stage 4 does today.
    std::optional<aka_crypto::Kamf> kamf;
    // The RAND from the most recently sent AuthenticationRequest -- needed if THIS attempt itself
    // gets an AuthenticationFailure+AUTS back (SQN resync, ADR-0037): decoding AUTS only works
    // with the exact RAND the UE used, which the AuthenticationFailure message itself doesn't
    // repeat.
    std::optional<aka_crypto::Key128> last_auth_rand;
    // At most one resync retry per association -- this lab's scope (ADR-0031); a real AMF would
    // still cap retries (TS 33.102 doesn't allow unbounded resync loops either), just possibly
    // higher than 1.
    bool sqn_resync_attempted = false;
    // Which UplinkNASTransport this association is next expecting -- this lab's
    // single-registration-per-association scope (ADR-0031) makes a simple linear phase enum a
    // correct dispatch key; a real AMF would need a full per-UE 5GMM state machine.
    enum class Phase {
        AwaitingAuthenticationResponse,
        AwaitingSecurityModeComplete,
        // UPDATE (gap-closure, docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075): this phase
        // now genuinely exists, where it previously didn't. TS 24.501's real UE behavior
        // (confirmed via source, simulators/ransim/vendor/UERANSIM/src/ue/nas/mm/
        // register.cpp:346-426) sends RegistrationComplete if RegistrationAccept carried a
        // 5G-GUTI, an NSSCI=CHANGED indication, or a configuredNSSAI -- encode_registration_accept
        // now sends a real 5G-GUTI (the ServiceRequest gap-closure's own real prerequisite), so a
        // real UE now genuinely sends RegistrationComplete in response, and this AMF must consume
        // it (real, load-bearing: skipping it would desync the NAS uplink COUNT this UE's every
        // later secured uplink message's own MAC verification depends on, not just a missed log
        // line).
        AwaitingRegistrationComplete,
        AwaitingPduSessionEstablishmentRequest,
        Done,
    };
    Phase phase = Phase::AwaitingAuthenticationResponse;
};

// TS 33.501 Annex A.7's SUPI input for KAMF derivation is the bare identity digits, NOT the
// "imsi-"-prefixed SBI string representation this project otherwise uses for auth_state.supi
// everywhere else (AUSF/PCF/SMF calls all correctly use the full "imsi-..." string -- confirmed
// working, since those calls succeed). Confirmed via real interop, not assumed: a first attempt
// using the full "imsi-..." string produced a KAMF the UE could never converge on
// (SecurityModeCommand MAC verification failed against a real nr-ue), tracked down to
// simulators/ransim/vendor/UERANSIM/src/utils/common_types.cpp's own Supi::Parse stripping the
// "imsi-" prefix before its own KDF calls (e.g. keys.cpp:33's
// `EncodeKdfString(ueConfig.supi->value)` -- `value` is deliberately prefix-free). Only this
// project's own "imsi-" prefix convention needs stripping -- a real SUCI-derived, non-IMSI-format
// SUPI is out of scope (matches decode_registration_request's own existing IMSI-only scope). See
// docs/DECISIONS.md ADR-0037.
std::string strip_imsi_prefix(const std::string& supi) {
    static constexpr char kPrefix[] = "imsi-";
    static constexpr std::size_t kPrefixLen = sizeof(kPrefix) - 1;
    if (supi.compare(0, kPrefixLen, kPrefix) == 0) {
        return supi.substr(kPrefixLen);
    }
    return supi;
}

// Shared by every Stage 2+/4 handler that needs to pull the NAS-PDU out of an UplinkNASTransport
// (previously duplicated inline in handle_uplink_nas_transport; factored out once a second caller
// -- the SecurityModeComplete handler -- needed the identical extraction).
std::optional<std::vector<std::uint8_t>> extract_uplink_nas_pdu(const InitiatingMessage_t& msg) {
    const auto& container = msg.value.choice.UplinkNASTransport.protocolIEs;
    const auto* nas_pdu_ie = ::ngap::find_ie(container, 38 /* id-NAS-PDU */);
    if (nas_pdu_ie == nullptr) {
        spdlog::warn("amf-ngap: UplinkNASTransport missing mandatory NAS-PDU IE, ignoring");
        return std::nullopt;
    }
    auto* nas_pdu = static_cast<NAS_PDU_t*>(::ngap::decode_ie_value(&asn_DEF_NAS_PDU, *nas_pdu_ie));
    if (nas_pdu == nullptr) {
        spdlog::warn("amf-ngap: UplinkNASTransport's NAS-PDU failed to PER-decode, ignoring");
        return std::nullopt;
    }
    std::vector<std::uint8_t> bytes(nas_pdu->buf, nas_pdu->buf + nas_pdu->size);
    ASN_STRUCT_FREE(asn_DEF_NAS_PDU, nas_pdu);
    return bytes;
}

// PLMN identity encoding per TS 24.008/38.413 (half-octet BCD, filler 0xF for a 2-digit MNC's
// third digit) -- the same standard encoding every 3GPP PLMN identity field uses, not specific to
// NGAP.
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

OCTET_STRING_t make_octet_string(const std::uint8_t* data, std::size_t len) {
    OCTET_STRING_t s{};
    s.buf = static_cast<std::uint8_t*>(std::malloc(len));
    s.size = len;
    std::memcpy(s.buf, data, len);
    return s;
}

BIT_STRING_t make_zero_bit_string(std::size_t bits) {
    const std::size_t bytes = (bits + 7) / 8;
    BIT_STRING_t s{};
    s.buf = static_cast<std::uint8_t*>(std::calloc(bytes, 1));
    s.size = bytes;
    s.bits_unused = static_cast<int>(bytes * 8 - bits);
    return s;
}

NGSetupResponse_t build_ng_setup_response() {
    NGSetupResponse_t resp{};

    // id-AMFName (mandatory).
    AMFName_t amf_name{};
    amf_name.buf = reinterpret_cast<std::uint8_t*>(::strdup(kAmfName));
    amf_name.size = std::strlen(kAmfName);
    ::ngap::add_ie(
        resp.protocolIEs,
        ::ngap::make_ie(1 /* id-AMFName */, Criticality_reject, &asn_DEF_AMFName, &amf_name));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMFName, &amf_name);

    // id-ServedGUAMIList (mandatory) -- one GUAMI, this lab's fixed PLMN + all-zero AMF Identifier.
    ServedGUAMIList_t guami_list{};
    auto* guami_item = static_cast<ServedGUAMIItem_t*>(std::calloc(1, sizeof(ServedGUAMIItem_t)));
    std::uint8_t plmn_bytes[3];
    encode_plmn_identity(kMcc, kMnc, plmn_bytes);
    guami_item->gUAMI.pLMNIdentity = make_octet_string(plmn_bytes, 3);
    guami_item->gUAMI.aMFRegionID = make_zero_bit_string(8);
    guami_item->gUAMI.aMFSetID = make_zero_bit_string(10);
    guami_item->gUAMI.aMFPointer = make_zero_bit_string(6);
    ASN_SEQUENCE_ADD(&guami_list.list, guami_item);
    ::ngap::add_ie(resp.protocolIEs,
                   ::ngap::make_ie(96 /* id-ServedGUAMIList */,
                                   Criticality_reject,
                                   &asn_DEF_ServedGUAMIList,
                                   &guami_list));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_ServedGUAMIList, &guami_list);

    // id-RelativeAMFCapacity (mandatory) -- arbitrary mid-range lab value (0..255 per spec range).
    RelativeAMFCapacity_t capacity = 128;
    ::ngap::add_ie(resp.protocolIEs,
                   ::ngap::make_ie(86 /* id-RelativeAMFCapacity */,
                                   Criticality_ignore,
                                   &asn_DEF_RelativeAMFCapacity,
                                   &capacity));

    // id-PLMNSupportList (mandatory) -- this lab's fixed PLMN + S-NSSAI (sst=1, sd=1).
    PLMNSupportList_t plmn_support{};
    auto* plmn_item = static_cast<PLMNSupportItem_t*>(std::calloc(1, sizeof(PLMNSupportItem_t)));
    plmn_item->pLMNIdentity = make_octet_string(plmn_bytes, 3);
    auto* slice_item = static_cast<SliceSupportItem_t*>(std::calloc(1, sizeof(SliceSupportItem_t)));
    slice_item->s_NSSAI.sST = make_octet_string(&kSst, 1);
    auto* sd = static_cast<SD_t*>(std::calloc(1, sizeof(SD_t)));
    const std::uint8_t sd_bytes[3] = {static_cast<std::uint8_t>((kSd >> 16) & 0xff),
                                      static_cast<std::uint8_t>((kSd >> 8) & 0xff),
                                      static_cast<std::uint8_t>(kSd & 0xff)};
    *sd = make_octet_string(sd_bytes, 3);
    slice_item->s_NSSAI.sD = sd;
    ASN_SEQUENCE_ADD(&plmn_item->sliceSupportList.list, slice_item);
    ASN_SEQUENCE_ADD(&plmn_support.list, plmn_item);
    ::ngap::add_ie(resp.protocolIEs,
                   ::ngap::make_ie(80 /* id-PLMNSupportList */,
                                   Criticality_reject,
                                   &asn_DEF_PLMNSupportList,
                                   &plmn_support));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PLMNSupportList, &plmn_support);

    return resp;
}

NGAP_PDU_t wrap_successful_outcome_ng_setup(NGSetupResponse_t response) {
    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_successfulOutcome;
    pdu.choice.successfulOutcome =
        static_cast<SuccessfulOutcome_t*>(std::calloc(1, sizeof(SuccessfulOutcome_t)));
    pdu.choice.successfulOutcome->procedureCode = 21 /* id-NGSetup */;
    pdu.choice.successfulOutcome->criticality = Criticality_reject;
    pdu.choice.successfulOutcome->value.present = SuccessfulOutcome__value_PR_NGSetupResponse;
    pdu.choice.successfulOutcome->value.choice.NGSetupResponse = response;
    return pdu;
}

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0095): real GlobalRANNodeID
// extraction (TS 38.413 §9.3.1.6, id-GlobalRANNodeID=27) -- the real gNB identity
// GnbAssociationRegistry needs to route a later HandoverRequest onto THIS gNB's own live
// association during a real N2 handover. This lab's own real, disclosed scope: only the
// `globalGNB-ID` CHOICE arm (this project's only real RAN node type, gNB via UERANSIM) is
// supported -- `globalNgENB-ID`/`globalN3IWF-ID`/the TNGF/TWIF/W-AGF extension IEs are real ASN.1
// CHOICE alternatives this project has no access network for, and are rejected (nullopt), not
// silently misparsed as a gNB. Returns the real PER-encoded GlobalGNB-ID bytes (PLMNIdentity +
// GNB-ID bit string) as a stable, spec-derived identity key -- not an invented label.
std::optional<std::vector<std::uint8_t>> extract_global_gnb_id(const InitiatingMessage_t& msg) {
    const auto& container = msg.value.choice.NGSetupRequest.protocolIEs;
    const auto* id_ie = ::ngap::find_ie(container, 27 /* id-GlobalRANNodeID */);
    if (id_ie == nullptr) {
        spdlog::warn("amf-ngap: NGSetupRequest missing mandatory GlobalRANNodeID IE");
        return std::nullopt;
    }
    auto* global_id =
        static_cast<GlobalRANNodeID_t*>(::ngap::decode_ie_value(&asn_DEF_GlobalRANNodeID, *id_ie));
    if (global_id == nullptr) {
        spdlog::warn("amf-ngap: NGSetupRequest's GlobalRANNodeID failed to PER-decode");
        return std::nullopt;
    }
    if (global_id->present != GlobalRANNodeID_PR_globalGNB_ID) {
        spdlog::warn("amf-ngap: NGSetupRequest's GlobalRANNodeID is not a real globalGNB-ID "
                     "(present={}) -- this lab has no other real RAN node type, rejecting",
                     static_cast<int>(global_id->present));
        ASN_STRUCT_FREE(asn_DEF_GlobalRANNodeID, global_id);
        return std::nullopt;
    }
    const auto gnb_id_bytes =
        ::ngap::encode_value(&asn_DEF_GlobalGNB_ID, global_id->choice.globalGNB_ID);
    ASN_STRUCT_FREE(asn_DEF_GlobalRANNodeID, global_id);
    if (gnb_id_bytes.empty()) {
        spdlog::warn("amf-ngap: failed to re-encode GlobalGNB-ID as a registry key");
        return std::nullopt;
    }
    return gnb_id_bytes;
}

// Returns this gNB's real GlobalGNB-ID bytes on success (for GnbAssociationRegistry
// registration), or nullopt if the mandatory IE was missing/unparseable/not a real gNB -- the
// NGSetupResponse is still sent either way (this lab's own established "log and continue" style
// for optional-to-this-lab data, matching every other handler's mandatory-but-log-only IEs), but
// a gNB whose identity couldn't be captured cannot later be a real HandoverRequest target.
std::optional<std::vector<std::uint8_t>> handle_ng_setup_request(ngap_core::SctpSocket& assoc,
                                                                 const InitiatingMessage_t& msg) {
    spdlog::info("amf-ngap: received NGSetupRequest, sending NGSetupResponse");
    auto gnb_id = extract_global_gnb_id(msg);

    NGSetupResponse_t response = build_ng_setup_response();
    NGAP_PDU_t pdu = wrap_successful_outcome_ng_setup(response);
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);

    if (bytes.empty()) {
        spdlog::error("amf-ngap: failed to PER-encode NGSetupResponse");
        return std::nullopt;
    }
    {
        std::string hex;
        for (auto b : bytes) {
            char buf[4];
            std::snprintf(buf, sizeof(buf), "%02x", b);
            hex += buf;
        }
        spdlog::info("amf-ngap: NGSetupResponse hex: {}", hex);
    }
    assoc.send(bytes);
    spdlog::info("amf-ngap: sent NGSetupResponse ({} bytes)", bytes.size());
    return gnb_id;
}

NGAP_PDU_t wrap_initiating_downlink_nas_transport(DownlinkNASTransport_t msg) {
    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = 4 /* id-DownlinkNASTransport */;
    pdu.choice.initiatingMessage->criticality = Criticality_ignore;
    pdu.choice.initiatingMessage->value.present = InitiatingMessage__value_PR_DownlinkNASTransport;
    pdu.choice.initiatingMessage->value.choice.DownlinkNASTransport = msg;
    return pdu;
}

// Shared DownlinkNASTransport send path -- Stage 2's AuthenticationRequest and Stage 4's
// SecurityModeCommand both just wrap a NAS-PDU byte string with the same two UE identifiers and
// send it, previously duplicated inline (see this function's call sites).
void send_downlink_nas_transport(ngap_core::SctpSocket& assoc,
                                 unsigned long amf_ue_id,
                                 unsigned long ran_ue_id,
                                 const std::vector<std::uint8_t>& nas_bytes) {
    NAS_PDU_t out_nas_pdu = make_octet_string(nas_bytes.data(), nas_bytes.size());
    AMF_UE_NGAP_ID_t amf_ue_ngap_id{};
    asn_ulong2INTEGER(&amf_ue_ngap_id, amf_ue_id);
    RAN_UE_NGAP_ID_t out_ran_ue_id = ran_ue_id;

    DownlinkNASTransport_t dl{};
    ::ngap::add_ie(dl.protocolIEs,
                   ::ngap::make_ie(10 /* id-AMF-UE-NGAP-ID */,
                                   Criticality_reject,
                                   &asn_DEF_AMF_UE_NGAP_ID,
                                   &amf_ue_ngap_id));
    ::ngap::add_ie(dl.protocolIEs,
                   ::ngap::make_ie(85 /* id-RAN-UE-NGAP-ID */,
                                   Criticality_reject,
                                   &asn_DEF_RAN_UE_NGAP_ID,
                                   &out_ran_ue_id));
    ::ngap::add_ie(
        dl.protocolIEs,
        ::ngap::make_ie(38 /* id-NAS-PDU */, Criticality_reject, &asn_DEF_NAS_PDU, &out_nas_pdu));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AMF_UE_NGAP_ID, &amf_ue_ngap_id);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NAS_PDU, &out_nas_pdu);

    NGAP_PDU_t pdu = wrap_initiating_downlink_nas_transport(dl);
    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);

    if (bytes.empty()) {
        spdlog::error("amf-ngap: failed to PER-encode DownlinkNASTransport");
        return;
    }
    assoc.send(bytes);
}

// Calls real AUSF's Nausf_UEAuthentication (POST .../ue-authentications), optionally carrying
// `resync_info` (TS 33.102 §6.3.3 SQN resynchronisation, ADR-0037 -- set when this call is a
// resync retry after an AuthenticationFailure, nullopt for a first attempt) and, on success,
// sends the resulting NAS AuthenticationRequest to the UE via DownlinkNASTransport. Shared by
// handle_initial_ue_message (the first attempt) and handle_uplink_nas_transport (the resync
// retry) -- the two call sites differ only in resync_info, everything else about initiating
// 5G-AKA is identical. Requires auth_state.supi/amf_ue_id/ran_ue_id already set. Returns false
// (having already logged why) on any failure; auth_state.confirmation_path/last_auth_rand are
// only updated on success.
bool initiate_5g_aka_authentication(
    const PeerEndpoints& peers,
    ngap_core::SctpSocket& assoc,
    sbi_core::http2::Client& ausf_client,
    sbi_core::OAuth2Client& ausf_oauth,
    UeAuthState& auth_state,
    const std::optional<sbi_gen::ResynchronizationInfo_Nudm_UEAU>& resync_info) {
    auto token = ausf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::error("amf-ngap: could not obtain AUSF bearer token: {}", token.error());
        return false;
    }

    sbi_gen::AuthenticationInfo req{};
    req.supiOrSuci = auth_state.supi;
    req.servingNetworkName = kServingNetworkName;
    req.resynchronizationInfo = resync_info;

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "POST";
    http_req.url = peers.ausf_base + "/nausf-auth/v1/ue-authentications";
    http_req.headers.emplace("content-type", "application/json");
    http_req.headers.emplace("authorization", "Bearer " + *token);
    http_req.body = nlohmann::json(req).dump();

    auto resp = ausf_client.send(http_req);
    if (!resp.has_value()) {
        spdlog::error("amf-ngap: AUSF call failed: {}", resp.error());
        return false;
    }
    if (resp->status != 201) {
        spdlog::error("amf-ngap: AUSF returned unexpected status {} for SUPI {}",
                      resp->status,
                      auth_state.supi);
        return false;
    }

    sbi_gen::UEAuthenticationCtx ctx;
    try {
        ctx = nlohmann::json::parse(resp->body).get<sbi_gen::UEAuthenticationCtx>();
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("amf-ngap: AUSF returned a malformed UEAuthenticationCtx: {}", e.what());
        return false;
    }
    if (ctx.authType.value != sbi_gen::AuthType_Nausf_UEAuthentication::V5G_AKA) {
        spdlog::warn("amf-ngap: AUSF returned auth method '{}', only 5G_AKA is supported yet, "
                     "ignoring",
                     ctx.authType.value);
        return false;
    }

    // Stage 3 needs this to confirm the eventual AuthenticationResponse/Failure -- AUSF's own
    // response shape (_links["5g-aka"]["href"], nfs/ausf/src/main.cpp) is the source of truth for
    // the confirmation URL, not reconstructed from the auth context id ourselves.
    try {
        auth_state.confirmation_path = ctx._links.at("5g-aka").at("href").get<std::string>();
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("amf-ngap: AUSF's _links is missing the 5g-aka confirmation href: {}",
                      e.what());
        return false;
    }

    sbi_gen::Av5gAka av;
    try {
        av = ctx.n5gAuthData.get<sbi_gen::Av5gAka>();
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("amf-ngap: AUSF's n5gAuthData is not a valid Av5gAka: {}", e.what());
        return false;
    }
    const auto rand = aka_crypto::from_hex<16>(av.rand);
    const auto autn = aka_crypto::from_hex<16>(av.autn);
    if (!rand.has_value() || !autn.has_value()) {
        spdlog::error("amf-ngap: AUSF returned malformed hex RAND/AUTN");
        return false;
    }
    auth_state.last_auth_rand = *rand; // needed if THIS AuthenticationRequest itself later resyncs

    const std::vector<std::uint8_t> auth_req_nas =
        amf::nas::encode_authentication_request(*rand, *autn, /*ngksi=*/0);
    send_downlink_nas_transport(assoc, auth_state.amf_ue_id, auth_state.ran_ue_id, auth_req_nas);
    spdlog::info("amf-ngap: sent DownlinkNASTransport with AuthenticationRequest ({} bytes), "
                 "AMF-UE-NGAP-ID={}{}",
                 auth_req_nas.size(),
                 auth_state.amf_ue_id,
                 resync_info.has_value() ? " (SQN resync retry)" : "");
    return true;
}

// Stage 2 (see docs/DECISIONS.md's staged NGAP/NAS plan): decodes the NAS RegistrationRequest
// carried in InitialUEMessage's NAS-PDU, calls real AUSF's Nausf_UEAuthentication
// (POST .../ue-authentications) with the extracted SUPI, and sends back a real NAS
// AuthenticationRequest (RAND/AUTN from AUSF's 5G-AKA vector) wrapped in DownlinkNASTransport.
// Stage 3 picks up from there (decoding the UE's AuthenticationResponse, confirming with AUSF,
// deriving KAMF) -- not handled here.
// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075): real ServiceRequest handling
// (TS 24.501 §5.6.1) -- a UE reconnecting on a FRESH NG association (this function is only ever
// reached from handle_initial_ue_message, i.e. a NEW association, exactly the real scenario
// ServiceRequest exists for), reusing an EXISTING, persisted NAS security context instead of
// running the full authentication procedure again. Real, disclosed scope boundary: this closes
// the CM-IDLE->CM-CONNECTED transition itself; it does NOT drive real N2 PDU Session Resource
// Setup for any PDU session `uplinkDataStatus` reports as having pending data -- that's SMF's own
// `UpdateSMContext` real N2SmInfo dispatch, a separate, already-tracked gap
// (docs/CAPABILITY_GAP_ANALYSIS.md task #101), not fabricated here just because this function
// could technically send SOMETHING.
void handle_service_request(ngap_core::SctpSocket& assoc,
                            UeSecurityContextStore& ue_security_contexts,
                            NgapUeRegistry& ue_ngap_registry,
                            UeAuthState& auth_state,
                            unsigned long ran_ue_id,
                            const std::vector<std::uint8_t>& nas_pdu_bytes) {
    const auto tmsi = amf::nas::peek_service_request_tmsi(nas_pdu_bytes);
    if (!tmsi.has_value()) {
        spdlog::warn("amf-ngap: InitialUEMessage's NAS-PDU is neither a supported "
                     "RegistrationRequest nor a ServiceRequest, ignoring");
        return;
    }

    const auto ctx = ue_security_contexts.get(*tmsi);
    const auto fresh_amf_ue_id = g_next_amf_ue_ngap_id.fetch_add(1);
    if (!ctx.has_value()) {
        spdlog::warn("amf-ngap: ServiceRequest for unknown tmsi={:08x} (no persisted security "
                     "context -- unknown UE, or this AMF restarted since it was issued), "
                     "rejecting",
                     *tmsi);
        const auto reject_nas = amf::nas::encode_service_reject_plain(
            0x09 /* TS 24.501 5GMM cause: UE_IDENTITY_CANNOT_BE_DERIVED_FROM_NETWORK */);
        send_downlink_nas_transport(assoc, fresh_amf_ue_id, ran_ue_id, reject_nas);
        return;
    }

    const auto knas_int =
        aka_crypto::derive_knas_int(ctx->kamf, aka_crypto::kNia2AlgorithmIdentity);
    const auto knas_enc =
        aka_crypto::derive_knas_enc(ctx->kamf, aka_crypto::kNea2AlgorithmIdentity);
    const auto uplink_count = ue_security_contexts.next_uplink_count(*tmsi);
    const auto info = amf::nas::decode_service_request(knas_int, uplink_count, nas_pdu_bytes);
    if (!info.has_value()) {
        spdlog::warn("amf-ngap: NAS-PDU matched ServiceRequest's own envelope shape but failed to "
                     "parse for tmsi={:08x} (SUPI {}), ignoring",
                     *tmsi,
                     ctx->supi);
        return;
    }
    if (!info->mac_valid) {
        spdlog::warn("amf-ngap: ServiceRequest MAC verification FAILED for SUPI {} -- wrong keys, "
                     "a tampered/replayed message, or a NAS COUNT desync",
                     ctx->supi);
        return;
    }
    if (info->ngksi != ctx->ngksi) {
        spdlog::warn("amf-ngap: ServiceRequest ngKSI mismatch for SUPI {} (got {}, persisted "
                     "context has {}) -- real TS 24.501 security-context-desync case, rejecting",
                     ctx->supi,
                     info->ngksi,
                     ctx->ngksi);
        return;
    }

    spdlog::info("amf-ngap: ServiceRequest verified OK for SUPI {} (serviceType={}), CM-IDLE -> "
                 "CM-CONNECTED",
                 ctx->supi,
                 info->service_type);

    auth_state.amf_ue_id = fresh_amf_ue_id;
    auth_state.ran_ue_id = ran_ue_id;
    auth_state.supi = ctx->supi;
    auth_state.knas_int = knas_int;
    auth_state.knas_enc = knas_enc;
    auth_state.kamf = ctx->kamf;
    auth_state.ue_security_capability = ctx->ue_security_capability;

    const auto downlink_count = ue_security_contexts.next_downlink_count(*tmsi);
    const auto accept_nas = amf::nas::encode_service_accept(
        knas_int, knas_enc, downlink_count, info->pdu_session_status.value_or(0));
    send_downlink_nas_transport(assoc, auth_state.amf_ue_id, auth_state.ran_ue_id, accept_nas);
    spdlog::info(
        "amf-ngap: sent DownlinkNASTransport with ServiceAccept ({} bytes), AMF-UE-NGAP-ID={}",
        accept_nas.size(),
        auth_state.amf_ue_id);

    // Re-register this UE's now-live (new) association -- the old one's own registry entry is
    // long gone (unregister_ue fires when an association's socket closes, see handle_association's
    // own disconnect path), so without this a UE that reconnects via ServiceRequest would be
    // silently invisible to any later Namf_Communication N1N2MessageTransfer call, same real
    // mechanism the original registration flow's own registry entry exists for (ADR-0038).
    ue_ngap_registry.register_ue(
        auth_state.supi,
        NgapUeRegistry::Entry{&assoc,
                              static_cast<std::uint32_t>(auth_state.amf_ue_id),
                              static_cast<std::uint32_t>(auth_state.ran_ue_id),
                              knas_int,
                              knas_enc,
                              downlink_count + 1});

    auth_state.phase = UeAuthState::Phase::Done;

    if (info->uplink_data_status.has_value() && *info->uplink_data_status != 0) {
        spdlog::warn("amf-ngap: ServiceRequest for SUPI {} reports uplinkDataStatus={:#06x} (PDU "
                     "session(s) with pending uplink data) -- real N2 PDU Session Resource Setup "
                     "triggered by this is a separate, disclosed gap (SMF's own real N2SmInfo "
                     "dispatch, docs/CAPABILITY_GAP_ANALYSIS.md task #101), not implemented here",
                     ctx->supi,
                     *info->uplink_data_status);
    }
}

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075, part 2/ADR-0078): NGAP
// UEContextRelease{Request,Command,Complete} (TS 38.413 §8.3.3/§9.2.1.9-11) -- found blocking a
// live-interop attempt to trigger a real ServiceRequest via UERANSIM's own
// `nr-cli <gnb> --exec 'ue-release <id>'`, which sends a real, previously entirely undecoded
// UEContextReleaseRequest ("failed to decode NGAP PDU ... ignoring"). RAN-initiated: the gNB
// sends UEContextReleaseRequest (a real cause -- O&M intervention, radio-link failure, etc.);
// this AMF replies with UEContextReleaseCommand (Cause=nas/normal-release, this lab's own
// network-triggered-release choice, not a value taken from the request's own cause); the gNB
// then confirms with UEContextReleaseComplete, decoded by handle_ue_context_release_complete
// below to actually clean up. Real, disclosed scope boundary: this does NOT implement the
// AMF-INITIATED direction (an AMF that decides on its own, e.g. after a Deregistration, to send
// UEContextReleaseCommand unprompted) -- this lab has no such trigger yet; only the RAN-initiated
// request/command/complete round trip a real gNB actually exercises today.
void handle_ue_context_release_request(ngap_core::SctpSocket& assoc,
                                       UeAuthState& auth_state,
                                       const InitiatingMessage_t& msg) {
    const auto& container = msg.value.choice.UEContextReleaseRequest.protocolIEs;

    const auto* amf_ue_id_ie = ::ngap::find_ie(container, 10 /* id-AMF-UE-NGAP-ID */);
    const auto* ran_ue_id_ie = ::ngap::find_ie(container, 85 /* id-RAN-UE-NGAP-ID */);
    const auto* cause_ie = ::ngap::find_ie(container, 15 /* id-Cause */);
    if (amf_ue_id_ie == nullptr || ran_ue_id_ie == nullptr) {
        spdlog::warn("amf-ngap: UEContextReleaseRequest missing mandatory AMF-UE-NGAP-ID/"
                     "RAN-UE-NGAP-ID IE, ignoring");
        return;
    }

    auto* amf_ue_id = static_cast<AMF_UE_NGAP_ID_t*>(
        ::ngap::decode_ie_value(&asn_DEF_AMF_UE_NGAP_ID, *amf_ue_id_ie));
    auto* ran_ue_id = static_cast<RAN_UE_NGAP_ID_t*>(
        ::ngap::decode_ie_value(&asn_DEF_RAN_UE_NGAP_ID, *ran_ue_id_ie));
    if (amf_ue_id == nullptr || ran_ue_id == nullptr) {
        spdlog::warn("amf-ngap: UEContextReleaseRequest's AMF-UE-NGAP-ID/RAN-UE-NGAP-ID failed to "
                     "PER-decode, ignoring");
        if (amf_ue_id != nullptr)
            ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, amf_ue_id);
        if (ran_ue_id != nullptr)
            ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, ran_ue_id);
        return;
    }
    long amf_ue_id_value = 0;
    asn_INTEGER2long(amf_ue_id, &amf_ue_id_value);

    // Cause is real and mandatory per spec, but this build only logs it -- a full real
    // cause-driven response (e.g. skipping the release for a transport-only cause) is out of
    // scope. Best-effort decode, not fatal if it fails to parse.
    if (cause_ie != nullptr) {
        auto* cause = static_cast<Cause_t*>(::ngap::decode_ie_value(&asn_DEF_Cause, *cause_ie));
        if (cause != nullptr) {
            spdlog::info("amf-ngap: UEContextReleaseRequest for AMF-UE-NGAP-ID={}, "
                         "RAN-UE-NGAP-ID={}, cause group={}",
                         amf_ue_id_value,
                         *ran_ue_id,
                         static_cast<int>(cause->present));
            ASN_STRUCT_FREE(asn_DEF_Cause, cause);
        }
    } else {
        spdlog::info("amf-ngap: UEContextReleaseRequest for AMF-UE-NGAP-ID={}, RAN-UE-NGAP-ID={} "
                     "(no Cause IE)",
                     amf_ue_id_value,
                     *ran_ue_id);
    }
    ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, amf_ue_id);
    ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, ran_ue_id);

    UEContextReleaseCommand_t cmd{};
    UE_NGAP_IDs_t ue_ngap_ids{};
    ue_ngap_ids.present = UE_NGAP_IDs_PR_aMF_UE_NGAP_ID;
    asn_ulong2INTEGER(&ue_ngap_ids.choice.aMF_UE_NGAP_ID,
                      static_cast<unsigned long>(auth_state.amf_ue_id));
    ::ngap::add_ie(
        cmd.protocolIEs,
        ::ngap::make_ie(
            114 /* id-UE-NGAP-IDs */, Criticality_reject, &asn_DEF_UE_NGAP_IDs, &ue_ngap_ids));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_UE_NGAP_IDs, &ue_ngap_ids);

    Cause_t cause_out{};
    cause_out.present = Cause_PR_nas;
    cause_out.choice.nas = CauseNas_normal_release;
    ::ngap::add_ie(
        cmd.protocolIEs,
        ::ngap::make_ie(15 /* id-Cause */, Criticality_ignore, &asn_DEF_Cause, &cause_out));

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_initiatingMessage;
    pdu.choice.initiatingMessage =
        static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
    pdu.choice.initiatingMessage->procedureCode = 41 /* id-UEContextRelease */;
    pdu.choice.initiatingMessage->criticality = Criticality_reject;
    pdu.choice.initiatingMessage->value.present =
        InitiatingMessage__value_PR_UEContextReleaseCommand;
    pdu.choice.initiatingMessage->value.choice.UEContextReleaseCommand = cmd;

    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    if (bytes.empty()) {
        spdlog::error("amf-ngap: failed to PER-encode UEContextReleaseCommand");
        return;
    }
    assoc.send(bytes);
    spdlog::info("amf-ngap: sent UEContextReleaseCommand ({} bytes), AMF-UE-NGAP-ID={}, "
                 "Cause=nas/normal-release",
                 bytes.size(),
                 auth_state.amf_ue_id);
}

// SuccessfulOutcome for id-UEContextRelease (TS 38.413 §9.2.1.11) -- confirms the release this
// AMF itself requested via UEContextReleaseCommand above. Real, disclosed behavior: resets
// auth_state to its default value so this same SCTP association can serve a brand-new UE context
// afterward, matching a real gNB's own behavior of keeping one association open across many UE
// contexts rather than tearing down the whole association per release -- this lab's "one UE at a
// time per association" scope (ADR-0031) becomes "one at a time, but the association itself now
// survives a release" as of this fix.
void handle_ue_context_release_complete(NgapUeRegistry& ue_ngap_registry,
                                        UeAuthState& auth_state,
                                        const SuccessfulOutcome_t& msg) {
    const auto& container = msg.value.choice.UEContextReleaseComplete.protocolIEs;
    const auto* amf_ue_id_ie = ::ngap::find_ie(container, 10 /* id-AMF-UE-NGAP-ID */);
    long amf_ue_id_value = static_cast<long>(auth_state.amf_ue_id);
    if (amf_ue_id_ie != nullptr) {
        auto* amf_ue_id = static_cast<AMF_UE_NGAP_ID_t*>(
            ::ngap::decode_ie_value(&asn_DEF_AMF_UE_NGAP_ID, *amf_ue_id_ie));
        if (amf_ue_id != nullptr) {
            asn_INTEGER2long(amf_ue_id, &amf_ue_id_value);
            ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, amf_ue_id);
        }
    }

    spdlog::info("amf-ngap: UEContextReleaseComplete received for AMF-UE-NGAP-ID={}, SUPI={} -- "
                 "UE context released, association ready for a new UE context",
                 amf_ue_id_value,
                 auth_state.supi.empty() ? "(none)" : auth_state.supi);

    if (!auth_state.supi.empty()) {
        ue_ngap_registry.unregister_ue(auth_state.supi);
    }
    auth_state = UeAuthState{};
}

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0090): real NGAP PathSwitchRequest
// (TS 38.413 §8.4.4) -- the AMF-facing tail end of an Xn-based inter-gNB handover (TS 23.502
// §4.9.1.2.2): after the source and target gNBs complete their own direct Xn handshake (entirely
// outside AMF's own visibility -- this project has no gNB-to-gNB Xn simulation either way), the
// TARGET gNB sends PathSwitchRequest to tell the AMF the UE has moved. Arrives on a BRAND NEW SCTP
// association (from the target gNB), with NO prior UeAuthState for it -- see
// amf_ue_id_index_store.hpp's own comment for why a real cross-association
// amf_ue_ngap_id -> tmsi index was a genuine, previously-missing architectural prerequisite this
// needed (every earlier NGAP procedure this project built runs on the SAME association a UE's
// context already lives on).
//
// Real, disclosed scope narrowing: PDUSessionResourceToBeSwitchedDLList is structurally parsed
// (real PDU session IDs, real mandatory-IE presence checks) but NOT acted on -- no SMF/UPF call
// updates real GTP-U forwarding for the new gNB; that is task #101's own explicit, separate,
// not-yet-built scope (SMF UpdateSMContext real N2SmInfo dispatch). The PDUSessionResourceSwitched
// List sent back echoes each PDU session ID with an all-OPTIONAL-fields-empty
// PathSwitchRequestAcknowledgeTransfer -- a real, spec-valid encoding (uL-NGU-UP-TNLInformation is
// genuinely OPTIONAL per the ASN.1 module), not a fabricated tunnel endpoint.
// UserLocationInformation/UESecurityCapabilities are checked for mandatory presence only, not
// decoded -- same "mandatory but log-only" precedent Cause already set in
// handle_ue_context_release_request above. AllowedNSSAI is this lab's own fixed single S-NSSAI
// (sst=kSst, sd=kSd), the same value NG Setup/registration already use everywhere else in this
// file, not derived per-UE.
//
// Real vertical key derivation (TS 33.501 Annex A.9/A.10, aka_crypto::derive_kgnb/derive_nh) for
// the mandatory SecurityContext IE -- see kdf.hpp's own header comment for this project's
// disclosed real scope: since no InitialContextSetup/prior AS security context has ever been
// established for any UE in this project, every call always derives chain position 0
// (NCC=0, SYNC-input=freshly-derived KgNB), never a later position.
//
// Real, load-bearing side effect on success: re-points ue_ngap_registry's entry for this UE's
// SUPI to the NEW association/RAN-UE-NGAP-ID -- without this, a later Namf_Communication
// N1N2MessageTransfer (ADR-0038) would still try to deliver to the stale source-gNB association.
// The stale source association itself is not force-closed (this project has no way to reach into
// a different association's own thread) -- a real, disclosed simplification, not a new regression.
//
// If SourceAMF-UE-NGAP-ID doesn't match any persisted context, replies with a real ErrorIndication
// (TS 38.413's generic, all-optional-fields error procedure, id-ErrorIndication) carrying
// Cause=radioNetwork/unknown-local-UE-NGAP-ID (the real, precise cited value, not PathSwitchRequest
// Failure -- that procedure's own PDUSessionResourceReleasedListPSFail IE is mandatory with a
// SIZE(1..) real ASN.1 constraint this project has no real PDU session IDs to populate for a
// wholly-unrecognized UE).
void handle_path_switch_request(ngap_core::SctpSocket& assoc,
                                UeSecurityContextStore& ue_security_contexts,
                                AmfUeIdIndexStore& amf_ue_id_index,
                                NgapUeRegistry& ue_ngap_registry,
                                const InitiatingMessage_t& msg) {
    const auto& container = msg.value.choice.PathSwitchRequest.protocolIEs;

    const auto* ran_ue_id_ie = ::ngap::find_ie(container, 85 /* id-RAN-UE-NGAP-ID */);
    const auto* source_amf_ue_id_ie = ::ngap::find_ie(container, 100 /* id-SourceAMF-UE-NGAP-ID */);
    const auto* dl_list_ie =
        ::ngap::find_ie(container, 76 /* id-PDUSessionResourceToBeSwitchedDLList */);
    const bool has_ul_info =
        ::ngap::find_ie(container, 121 /* id-UserLocationInformation */) != nullptr;
    const bool has_ue_sec_cap =
        ::ngap::find_ie(container, 119 /* id-UESecurityCapabilities */) != nullptr;
    if (ran_ue_id_ie == nullptr || source_amf_ue_id_ie == nullptr || dl_list_ie == nullptr ||
        !has_ul_info || !has_ue_sec_cap) {
        spdlog::warn("amf-ngap: PathSwitchRequest missing one or more mandatory IEs, ignoring");
        return;
    }

    auto* new_ran_ue_id = static_cast<RAN_UE_NGAP_ID_t*>(
        ::ngap::decode_ie_value(&asn_DEF_RAN_UE_NGAP_ID, *ran_ue_id_ie));
    auto* source_amf_ue_id = static_cast<AMF_UE_NGAP_ID_t*>(
        ::ngap::decode_ie_value(&asn_DEF_AMF_UE_NGAP_ID, *source_amf_ue_id_ie));
    auto* dl_list = static_cast<PDUSessionResourceToBeSwitchedDLList_t*>(
        ::ngap::decode_ie_value(&asn_DEF_PDUSessionResourceToBeSwitchedDLList, *dl_list_ie));
    if (new_ran_ue_id == nullptr || source_amf_ue_id == nullptr || dl_list == nullptr) {
        spdlog::warn("amf-ngap: PathSwitchRequest's mandatory IEs failed to PER-decode, ignoring");
        if (new_ran_ue_id != nullptr)
            ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, new_ran_ue_id);
        if (source_amf_ue_id != nullptr)
            ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, source_amf_ue_id);
        if (dl_list != nullptr)
            ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceToBeSwitchedDLList, dl_list);
        return;
    }

    long source_amf_ue_id_value = 0;
    asn_INTEGER2long(source_amf_ue_id, &source_amf_ue_id_value);
    const unsigned long new_ran_ue_id_value = *new_ran_ue_id;

    spdlog::info("amf-ngap: PathSwitchRequest for SourceAMF-UE-NGAP-ID={}, new RAN-UE-NGAP-ID={}, "
                 "{} PDU session(s) to switch",
                 source_amf_ue_id_value,
                 new_ran_ue_id_value,
                 dl_list->list.count);

    const auto tmsi_opt = amf_ue_id_index.get(static_cast<unsigned long>(source_amf_ue_id_value));
    std::optional<UeSecurityContext> ctx;
    if (tmsi_opt.has_value()) {
        ctx = ue_security_contexts.get(*tmsi_opt);
    }
    if (!ctx.has_value()) {
        spdlog::warn(
            "amf-ngap: PathSwitchRequest referenced an unrecognized SourceAMF-UE-NGAP-ID={} -- "
            "no persisted UE security context, sending ErrorIndication",
            source_amf_ue_id_value);

        ErrorIndication_t err{};
        ::ngap::add_ie(err.protocolIEs,
                       ::ngap::make_ie(85 /* id-RAN-UE-NGAP-ID */,
                                       Criticality_ignore,
                                       &asn_DEF_RAN_UE_NGAP_ID,
                                       new_ran_ue_id));
        Cause_t cause{};
        cause.present = Cause_PR_radioNetwork;
        cause.choice.radioNetwork = CauseRadioNetwork_unknown_local_UE_NGAP_ID;
        ::ngap::add_ie(
            err.protocolIEs,
            ::ngap::make_ie(15 /* id-Cause */, Criticality_ignore, &asn_DEF_Cause, &cause));

        NGAP_PDU_t err_pdu{};
        err_pdu.present = NGAP_PDU_PR_initiatingMessage;
        err_pdu.choice.initiatingMessage =
            static_cast<InitiatingMessage_t*>(std::calloc(1, sizeof(InitiatingMessage_t)));
        err_pdu.choice.initiatingMessage->procedureCode = 9 /* id-ErrorIndication */;
        err_pdu.choice.initiatingMessage->criticality = Criticality_ignore;
        err_pdu.choice.initiatingMessage->value.present =
            InitiatingMessage__value_PR_ErrorIndication;
        err_pdu.choice.initiatingMessage->value.choice.ErrorIndication = err;

        const auto err_bytes = ::ngap::encode_pdu(err_pdu);
        ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &err_pdu);
        if (!err_bytes.empty()) {
            assoc.send(err_bytes);
            spdlog::info("amf-ngap: sent ErrorIndication ({} bytes) for unrecognized "
                         "SourceAMF-UE-NGAP-ID={}",
                         err_bytes.size(),
                         source_amf_ue_id_value);
        }

        ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, new_ran_ue_id);
        ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, source_amf_ue_id);
        ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceToBeSwitchedDLList, dl_list);
        return;
    }

    const auto kgnb =
        aka_crypto::derive_kgnb(ctx->kamf, ctx->uplink_count, aka_crypto::kAccessType3gpp);
    const auto nh = aka_crypto::derive_nh(ctx->kamf, kgnb);

    PathSwitchRequestAcknowledge_t ack{};
    ::ngap::add_ie(ack.protocolIEs,
                   ::ngap::make_ie(10 /* id-AMF-UE-NGAP-ID */,
                                   Criticality_ignore,
                                   &asn_DEF_AMF_UE_NGAP_ID,
                                   source_amf_ue_id));
    ::ngap::add_ie(ack.protocolIEs,
                   ::ngap::make_ie(85 /* id-RAN-UE-NGAP-ID */,
                                   Criticality_ignore,
                                   &asn_DEF_RAN_UE_NGAP_ID,
                                   new_ran_ue_id));

    SecurityContext_t sec_ctx{};
    sec_ctx.nextHopChainingCount = 0;
    sec_ctx.nextHopNH.buf = static_cast<std::uint8_t*>(std::malloc(nh.size()));
    std::memcpy(sec_ctx.nextHopNH.buf, nh.data(), nh.size());
    sec_ctx.nextHopNH.size = nh.size();
    sec_ctx.nextHopNH.bits_unused = 0;
    ::ngap::add_ie(
        ack.protocolIEs,
        ::ngap::make_ie(
            93 /* id-SecurityContext */, Criticality_reject, &asn_DEF_SecurityContext, &sec_ctx));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_SecurityContext, &sec_ctx);

    PDUSessionResourceSwitchedList_t switched_list{};
    const PathSwitchRequestAcknowledgeTransfer_t empty_transfer{};
    const auto empty_transfer_bytes =
        ::ngap::encode_value(&asn_DEF_PathSwitchRequestAcknowledgeTransfer, &empty_transfer);
    for (int i = 0; i < dl_list->list.count; ++i) {
        auto* item = static_cast<PDUSessionResourceSwitchedItem_t*>(
            std::calloc(1, sizeof(PDUSessionResourceSwitchedItem_t)));
        item->pDUSessionID = dl_list->list.array[i]->pDUSessionID;
        item->pathSwitchRequestAcknowledgeTransfer.buf =
            static_cast<std::uint8_t*>(std::malloc(empty_transfer_bytes.size()));
        std::memcpy(item->pathSwitchRequestAcknowledgeTransfer.buf,
                    empty_transfer_bytes.data(),
                    empty_transfer_bytes.size());
        item->pathSwitchRequestAcknowledgeTransfer.size = empty_transfer_bytes.size();
        ASN_SEQUENCE_ADD(&switched_list.list, item);
    }
    ::ngap::add_ie(ack.protocolIEs,
                   ::ngap::make_ie(77 /* id-PDUSessionResourceSwitchedList */,
                                   Criticality_ignore,
                                   &asn_DEF_PDUSessionResourceSwitchedList,
                                   &switched_list));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_PDUSessionResourceSwitchedList, &switched_list);

    AllowedNSSAI_t allowed_nssai{};
    auto* nssai_item =
        static_cast<AllowedNSSAI_Item_t*>(std::calloc(1, sizeof(AllowedNSSAI_Item_t)));
    nssai_item->s_NSSAI.sST = make_octet_string(&kSst, 1);
    auto* sd = static_cast<SD_t*>(std::calloc(1, sizeof(SD_t)));
    const std::uint8_t sd_bytes[3] = {static_cast<std::uint8_t>((kSd >> 16) & 0xff),
                                      static_cast<std::uint8_t>((kSd >> 8) & 0xff),
                                      static_cast<std::uint8_t>(kSd & 0xff)};
    *sd = make_octet_string(sd_bytes, 3);
    nssai_item->s_NSSAI.sD = sd;
    ASN_SEQUENCE_ADD(&allowed_nssai.list, nssai_item);
    ::ngap::add_ie(
        ack.protocolIEs,
        ::ngap::make_ie(
            0 /* id-AllowedNSSAI */, Criticality_reject, &asn_DEF_AllowedNSSAI, &allowed_nssai));
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_AllowedNSSAI, &allowed_nssai);

    NGAP_PDU_t pdu{};
    pdu.present = NGAP_PDU_PR_successfulOutcome;
    pdu.choice.successfulOutcome =
        static_cast<SuccessfulOutcome_t*>(std::calloc(1, sizeof(SuccessfulOutcome_t)));
    pdu.choice.successfulOutcome->procedureCode = 25 /* id-PathSwitchRequest */;
    pdu.choice.successfulOutcome->criticality = Criticality_reject;
    pdu.choice.successfulOutcome->value.present =
        SuccessfulOutcome__value_PR_PathSwitchRequestAcknowledge;
    pdu.choice.successfulOutcome->value.choice.PathSwitchRequestAcknowledge = ack;

    const auto bytes = ::ngap::encode_pdu(pdu);
    ASN_STRUCT_FREE_CONTENTS_ONLY(asn_DEF_NGAP_PDU, &pdu);
    if (bytes.empty()) {
        spdlog::error("amf-ngap: failed to PER-encode PathSwitchRequestAcknowledge");
        ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, new_ran_ue_id);
        ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, source_amf_ue_id);
        ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceToBeSwitchedDLList, dl_list);
        return;
    }
    assoc.send(bytes);
    spdlog::info("amf-ngap: sent PathSwitchRequestAcknowledge ({} bytes), AMF-UE-NGAP-ID={}, new "
                 "RAN-UE-NGAP-ID={}, NCC=0",
                 bytes.size(),
                 source_amf_ue_id_value,
                 new_ran_ue_id_value);

    if (!ctx->supi.empty()) {
        NgapUeRegistry::Entry entry;
        entry.socket = &assoc;
        entry.amf_ue_id = static_cast<std::uint32_t>(source_amf_ue_id_value);
        entry.ran_ue_id = static_cast<std::uint32_t>(new_ran_ue_id_value);
        entry.knas_int = aka_crypto::derive_knas_int(ctx->kamf, aka_crypto::kNia2AlgorithmIdentity);
        entry.knas_enc = aka_crypto::derive_knas_enc(ctx->kamf, aka_crypto::kNea2AlgorithmIdentity);
        entry.next_downlink_count = ctx->downlink_count;
        ue_ngap_registry.register_ue(ctx->supi, entry);
        spdlog::info(
            "amf-ngap: re-pointed NGAP registry entry for SUPI {} to the new association after "
            "PathSwitchRequest",
            ctx->supi);
    }

    ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, new_ran_ue_id);
    ASN_STRUCT_FREE(asn_DEF_AMF_UE_NGAP_ID, source_amf_ue_id);
    ASN_STRUCT_FREE(asn_DEF_PDUSessionResourceToBeSwitchedDLList, dl_list);
}

void handle_initial_ue_message(const PeerEndpoints& peers,
                               ngap_core::SctpSocket& assoc,
                               sbi_core::http2::Client& ausf_client,
                               sbi_core::OAuth2Client& ausf_oauth,
                               UeSecurityContextStore& ue_security_contexts,
                               NgapUeRegistry& ue_ngap_registry,
                               UeAuthState& auth_state,
                               const InitiatingMessage_t& msg) {
    const auto& container = msg.value.choice.InitialUEMessage.protocolIEs;

    const auto* nas_pdu_ie = ::ngap::find_ie(container, 38 /* id-NAS-PDU */);
    const auto* ran_ue_ngap_id_ie = ::ngap::find_ie(container, 85 /* id-RAN-UE-NGAP-ID */);
    if (nas_pdu_ie == nullptr || ran_ue_ngap_id_ie == nullptr) {
        spdlog::warn("amf-ngap: InitialUEMessage missing mandatory NAS-PDU/RAN-UE-NGAP-ID IE, "
                     "ignoring");
        return;
    }

    auto* nas_pdu = static_cast<NAS_PDU_t*>(::ngap::decode_ie_value(&asn_DEF_NAS_PDU, *nas_pdu_ie));
    auto* ran_ue_ngap_id = static_cast<RAN_UE_NGAP_ID_t*>(
        ::ngap::decode_ie_value(&asn_DEF_RAN_UE_NGAP_ID, *ran_ue_ngap_id_ie));
    if (nas_pdu == nullptr || ran_ue_ngap_id == nullptr) {
        spdlog::warn("amf-ngap: InitialUEMessage's NAS-PDU/RAN-UE-NGAP-ID failed to PER-decode, "
                     "ignoring");
        if (nas_pdu != nullptr)
            ASN_STRUCT_FREE(asn_DEF_NAS_PDU, nas_pdu);
        if (ran_ue_ngap_id != nullptr)
            ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, ran_ue_ngap_id);
        return;
    }
    spdlog::info("amf-ngap: InitialUEMessage decoded OK: RAN-UE-NGAP-ID={}, NAS-PDU {} bytes",
                 *ran_ue_ngap_id,
                 nas_pdu->size);

    const std::vector<std::uint8_t> nas_pdu_bytes(nas_pdu->buf, nas_pdu->buf + nas_pdu->size);
    const unsigned long ran_ue_id = *ran_ue_ngap_id;
    ASN_STRUCT_FREE(asn_DEF_NAS_PDU, nas_pdu);
    ASN_STRUCT_FREE(asn_DEF_RAN_UE_NGAP_ID, ran_ue_ngap_id);

    const auto reg_info = amf::nas::decode_registration_request(nas_pdu_bytes);
    if (!reg_info.has_value()) {
        // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075): not every
        // InitialUEMessage is a RegistrationRequest -- a UE resuming from CM-IDLE on a fresh
        // association sends ServiceRequest instead, real, dominant, previously entirely
        // unhandled traffic this project's own gap-analysis sweep found missing.
        handle_service_request(
            assoc, ue_security_contexts, ue_ngap_registry, auth_state, ran_ue_id, nas_pdu_bytes);
        return;
    }
    spdlog::info(
        "amf-ngap: InitialUEMessage from RAN-UE-NGAP-ID={}, SUPI={}", ran_ue_id, reg_info->supi);

    // Stage 4 reuses both IDs to send SecurityModeCommand on this same association once
    // authentication succeeds -- see UeAuthState's own comment.
    auth_state.amf_ue_id = g_next_amf_ue_ngap_id.fetch_add(1);
    auth_state.ran_ue_id = ran_ue_id;
    auth_state.ue_security_capability = reg_info->ue_security_capability;
    auth_state.supi = reg_info->supi;

    initiate_5g_aka_authentication(peers, assoc, ausf_client, ausf_oauth, auth_state, std::nullopt);
}

// Stage 3: decodes the NAS-PDU carried in UplinkNASTransport (the UE's response to Stage 2's
// AuthenticationRequest), and for a real AuthenticationResponse, confirms RES* with real AUSF
// (PUT .../5g-aka-confirmation) and derives KAMF from the returned KSEAF. A real
// AuthenticationFailure (the outcome this project's currently-seeded test subscriber's fixed TS
// 35.207 SQN actually produces against a fresh UE -- see docs/DECISIONS.md ADR-0032) is decoded
// and, for a real SYNCH_FAILURE with AUTS, drives one real SQN resynchronisation retry (TS 33.102
// §6.3.3, ADR-0037): AUTS is forwarded to AUSF/UDM via resynchronizationInfo, and on success a
// fresh AuthenticationRequest is sent using the corrected vector, staying in
// AwaitingAuthenticationResponse to receive its response. Capped at one retry per association --
// see UeAuthState::sqn_resync_attempted's own comment.
void handle_uplink_nas_transport(const PeerEndpoints& peers,
                                 ngap_core::SctpSocket& assoc,
                                 sbi_core::http2::Client& ausf_client,
                                 sbi_core::OAuth2Client& ausf_oauth,
                                 UeAuthState& auth_state,
                                 const InitiatingMessage_t& msg) {
    const auto nas_pdu_bytes_opt = extract_uplink_nas_pdu(msg);
    if (!nas_pdu_bytes_opt.has_value()) {
        return;
    }
    const auto& nas_pdu_bytes = *nas_pdu_bytes_opt;

    const auto outcome = amf::nas::decode_authentication_outcome(nas_pdu_bytes);
    if (!outcome.has_value()) {
        spdlog::warn("amf-ngap: UplinkNASTransport's NAS-PDU is not a supported "
                     "AuthenticationResponse/AuthenticationFailure, ignoring");
        return;
    }

    if (!outcome->success) {
        if (auth_state.sqn_resync_attempted || !outcome->auts.has_value() ||
            !auth_state.last_auth_rand.has_value()) {
            spdlog::warn("amf-ngap: UE sent AuthenticationFailure (mmCause=0x{:02x}{}) for SUPI "
                         "{} -- giving up ({})",
                         outcome->mm_cause,
                         outcome->auts.has_value() ? ", with AUTS" : "",
                         auth_state.supi,
                         auth_state.sqn_resync_attempted
                             ? "resync already attempted once this association"
                         : !outcome->auts.has_value() ? "no AUTS to resync with"
                                                      : "no stored RAND to resync against");
            return;
        }

        spdlog::info("amf-ngap: UE sent AuthenticationFailure (mmCause=0x{:02x}, with AUTS) for "
                     "SUPI {} -- attempting SQN resynchronisation",
                     outcome->mm_cause,
                     auth_state.supi);
        auth_state.sqn_resync_attempted = true;

        sbi_gen::ResynchronizationInfo_Nudm_UEAU resync_info{};
        resync_info.rand = aka_crypto::to_hex(*auth_state.last_auth_rand);
        resync_info.auts = aka_crypto::to_hex(*outcome->auts);

        initiate_5g_aka_authentication(
            peers, assoc, ausf_client, ausf_oauth, auth_state, resync_info);
        return; // stay in AwaitingAuthenticationResponse either way -- success just sent a fresh
                // AuthenticationRequest; failure was already logged by
                // initiate_5g_aka_authentication itself
    }

    if (auth_state.confirmation_path.empty()) {
        spdlog::warn("amf-ngap: received AuthenticationResponse with no pending authentication "
                     "context (out-of-order message or lost association state), ignoring");
        return;
    }

    auto token = ausf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::error("amf-ngap: could not obtain AUSF bearer token: {}", token.error());
        return;
    }

    sbi_gen::ConfirmationData creq{};
    creq.resStar = aka_crypto::to_hex(outcome->res_star);

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "PUT";
    http_req.url = peers.ausf_base + auth_state.confirmation_path;
    http_req.headers.emplace("content-type", "application/json");
    http_req.headers.emplace("authorization", "Bearer " + *token);
    http_req.body = nlohmann::json(creq).dump();

    auto resp = ausf_client.send(http_req);
    if (!resp.has_value()) {
        spdlog::error("amf-ngap: AUSF confirmation call failed: {}", resp.error());
        return;
    }
    if (resp->status != 200) {
        spdlog::error("amf-ngap: AUSF confirmation returned unexpected status {} for SUPI {}",
                      resp->status,
                      auth_state.supi);
        return;
    }

    sbi_gen::ConfirmationDataResponse cresp;
    try {
        cresp = nlohmann::json::parse(resp->body).get<sbi_gen::ConfirmationDataResponse>();
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("amf-ngap: AUSF returned a malformed ConfirmationDataResponse: {}", e.what());
        return;
    }
    if (cresp.authResult.value !=
        sbi_gen::AuthResult_Nausf_UEAuthentication::AUTHENTICATION_SUCCESS) {
        spdlog::warn("amf-ngap: AUSF confirmation reports authentication FAILURE for SUPI {} "
                     "(claimed RES* did not match XRES*)",
                     auth_state.supi);
        return;
    }
    if (!cresp.kseaf.has_value()) {
        spdlog::error("amf-ngap: AUSF confirmed success but returned no kseaf for SUPI {}",
                      auth_state.supi);
        return;
    }
    const auto kseaf = aka_crypto::from_hex<32>(*cresp.kseaf);
    if (!kseaf.has_value()) {
        spdlog::error("amf-ngap: AUSF returned malformed hex kseaf for SUPI {}", auth_state.supi);
        return;
    }

    // Same fixed ABBA=0x0000 this AMF sent in Stage 2's AuthenticationRequest -- KAMF only
    // converges with the UE's own derivation if both sides used the same ABBA value.
    const aka_crypto::Abba abba{0x00, 0x00};
    const auto kamf = aka_crypto::derive_kamf(*kseaf, strip_imsi_prefix(auth_state.supi), abba);
    spdlog::info("amf-ngap: authentication SUCCESSFUL for SUPI {}, KAMF derived", auth_state.supi);
    auth_state.kamf = kamf;

    // Stage 4: activate NAS security. KNASenc/KNASint (TS 33.501 Annex A.8) for this project's
    // only implemented algorithm pair, 128-NEA2/128-NIA2 -- see aka_crypto/nas_security.hpp.
    auth_state.knas_int = aka_crypto::derive_knas_int(kamf, aka_crypto::kNia2AlgorithmIdentity);
    auth_state.knas_enc = aka_crypto::derive_knas_enc(kamf, aka_crypto::kNea2AlgorithmIdentity);

    const auto smc_nas = amf::nas::encode_security_mode_command(*auth_state.knas_int,
                                                                auth_state.ue_security_capability,
                                                                /*downlink_count=*/0);
    send_downlink_nas_transport(assoc, auth_state.amf_ue_id, auth_state.ran_ue_id, smc_nas);
    auth_state.phase = UeAuthState::Phase::AwaitingSecurityModeComplete;
    spdlog::info("amf-ngap: sent DownlinkNASTransport with SecurityModeCommand ({} bytes), "
                 "AMF-UE-NGAP-ID={}",
                 smc_nas.size(),
                 auth_state.amf_ue_id);
}

// Stage 4 continued: decodes+verifies the NAS-PDU carried in the UplinkNASTransport that follows
// a sent SecurityModeCommand, expected to be a SecurityModeComplete (TS 24.501 §8.2.26). This is
// this project's first NAS message secured end-to-end (128-NIA2 MAC verified, 128-NEA2
// deciphered). On success, immediately continues into Stage 5: allocates a real 5G-TMSI, persists
// this UE's NAS security context (UeSecurityContextStore -- gap-closure,
// docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075, the real prerequisite a later ServiceRequest
// needs), and sends RegistrationAccept carrying the real 5G-GUTI (the first message secured with
// the now-confirmed normal context, downlink_count=1). Real, load-bearing consequence of sending a
// GUTI (see UeAuthState::Phase's own updated comment): a real UE now sends RegistrationComplete in
// response, handled by handle_uplink_nas_transport_registration_complete below, NOT here anymore.

// ADR-0279: distinguishes "this slice is full" from "this slice is not mine".
//
// TS 29.536's AcuFailureReason carries both: EXCEED_MAX_* means the quota is exhausted, while
// SLICE_NOT_FOUND means NSACF holds no admission configuration for that S-NSSAI at all. Only the
// first is a refusal. Treating the second as one would let an NSACF that simply does not manage a
// slice block every session on it -- the same reasoning as the fail-open on an unreachable NSACF,
// applied to an answer rather than a failure to answer.
//
// Found by a test regression, not by review: the Sx-association gate creates sessions on
// sst=1 with no SD, which is a different S-NSSAI from the configured sst=1/sd=000001, so NSACF
// correctly answered SLICE_NOT_FOUND and a blanket refusal turned that into a 403.
bool acu_failure_is_a_real_refusal(const std::string& body) {
    try {
        const auto parsed = nlohmann::json::parse(body);
        if (!parsed.contains("acuFailureList") || !parsed.at("acuFailureList").is_array()) {
            return false;
        }
        for (const auto& item : parsed.at("acuFailureList")) {
            if (!item.contains("reason")) {
                continue;
            }
            const auto reason = item.at("reason").get<std::string>();
            if (reason.rfind("EXCEED_MAX", 0) == 0) {
                return true;
            }
        }
    } catch (const nlohmann::json::exception&) {
        // An unparseable failure list is not evidence of a refusal.
        return false;
    }
    return false;
}

// ADR-0278: returns false ONLY when NSACF really refused the slice. Every other outcome -- NSACF
// unreachable, no token, an unexpected status -- returns true, because a UE must not be denied
// service by an admission controller that failed to answer. A deliberate fail-open, named here
// rather than left implicit in the control flow: NSAC is an operator quota mechanism, not an
// authentication step, and TS 23.501 §5.15.11 gives it no authority over a registration it could
// not evaluate.
bool report_ue_to_nsacf(const PeerEndpoints& peers,
                        sbi_core::http2::Client& nsacf_client,
                        sbi_core::OAuth2Client& nsacf_oauth,
                        const std::string& supi,
                        bool increase) {
    if (peers.nsacf_base.empty()) {
        return true;
    }
    const auto token = nsacf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::error("amf-ngap: could not obtain an NSACF token for slice admission control: {}",
                      token.error());
        return true;
    }

    // The UE's own slice. kSst/kSd are this lab's single configured S-NSSAI -- the same pair the
    // Allowed NSSAI and the PDU session carry, so the admission control counts the slice the UE is
    // really on.
    nlohmann::json snssai{{"sst", kSst}};
    snssai["sd"] = fmt::format("{:06x}", kSd);

    nlohmann::json request;
    request["ueACRequestInfo"] = nlohmann::json::array({nlohmann::json{
        {"supi", supi},
        {"anType", "3GPP_ACCESS"},
        {"acuOperationList",
         nlohmann::json::array({nlohmann::json{{"updateFlag", increase ? "INCREASE" : "DECREASE"},
                                               {"snssai", snssai}}})}}});
    request["nfId"] = "00000000-0000-4000-8000-0000000000aa";

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "POST";
    http_req.url = peers.nsacf_base + "/nnsacf-nsac/v1/slices/ues";
    http_req.headers.emplace("content-type", "application/json");
    http_req.headers.emplace("authorization", "Bearer " + *token);
    http_req.body = request.dump();

    auto resp = nsacf_client.send(http_req);
    if (!resp.has_value()) {
        spdlog::warn("amf-ngap: NSACF NumOfUEsUpdate call failed for SUPI {}: {} -- admitting, see "
                     "this function's own fail-open note",
                     supi,
                     resp.error());
        return true;
    }
    if (resp->status == 204) {
        spdlog::info("amf-ngap: NSACF admitted SUPI {} onto slice sst={} ({})",
                     supi,
                     static_cast<int>(kSst),
                     increase ? "INCREASE" : "DECREASE");
        return true;
    }
    if (resp->status == 200) {
        // 200 is the YAML's "Partial successful ACU operation": the body carries the real
        // AcuFailureList. Only an EXCEED_MAX_* reason is a refusal -- see
        // acu_failure_is_a_real_refusal.
        if (acu_failure_is_a_real_refusal(resp->body)) {
            spdlog::warn("amf-ngap: NSACF REFUSED SUPI {} for its slice: {}", supi, resp->body);
            return false;
        }
        spdlog::info("amf-ngap: NSACF reported a non-quota ACU failure for SUPI {} ({}) -- "
                     "admitting, it holds no quota for this slice",
                     supi,
                     resp->body);
        return true;
    }
    spdlog::warn("amf-ngap: NSACF NumOfUEsUpdate returned unexpected status {} for SUPI {} -- "
                 "admitting, see this function's own fail-open note",
                 resp->status,
                 supi);
    return true;
}

void handle_uplink_nas_transport_smc_complete(const PeerEndpoints& peers,
                                              sbi_core::http2::Client& nsacf_client,
                                              sbi_core::OAuth2Client& nsacf_oauth,
                                              ngap_core::SctpSocket& assoc,
                                              UeSecurityContextStore& ue_security_contexts,
                                              AmfUeIdIndexStore& amf_ue_id_index,
                                              std::uint8_t amf_region_id,
                                              std::uint16_t amf_set_id,
                                              std::uint8_t amf_pointer,
                                              UeAuthState& auth_state,
                                              const InitiatingMessage_t& msg) {
    const auto nas_pdu_bytes_opt = extract_uplink_nas_pdu(msg);
    if (!nas_pdu_bytes_opt.has_value()) {
        return;
    }

    if (!auth_state.knas_int.has_value() || !auth_state.knas_enc.has_value()) {
        spdlog::warn("amf-ngap: received a post-SecurityModeCommand UplinkNASTransport with no "
                     "NAS security context (out-of-order message or lost association state), "
                     "ignoring");
        return;
    }

    const auto outcome = amf::nas::decode_security_mode_complete(
        *auth_state.knas_int, *auth_state.knas_enc, /*uplink_count=*/0, *nas_pdu_bytes_opt);
    if (!outcome.has_value()) {
        spdlog::warn("amf-ngap: UplinkNASTransport's NAS-PDU is not shaped like a "
                     "SecurityModeComplete, ignoring");
        return;
    }
    if (!outcome->mac_valid) {
        spdlog::warn("amf-ngap: SecurityModeComplete MAC verification FAILED for SUPI {} -- wrong "
                     "keys, a tampered/replayed message, or a NAS COUNT desync",
                     auth_state.supi);
        return;
    }

    spdlog::info("amf-ngap: SecurityModeComplete verified OK for SUPI {} -- NAS security context "
                 "active",
                 auth_state.supi);

    // ADR-0278: Network Slice Admission Control, BEFORE the UE is told it is registered.
    //
    // Placement is the whole point. ADR-0277 called NSACF after RegistrationAccept, which counts
    // the UE correctly but can only ever report a refusal -- by then the UE has been told it is
    // registered. TS 23.501 §5.15.11 makes NSAC a control on how many UEs may BE registered, so
    // the decision has to precede the Accept. The count is still taken here (the UE is admitted
    // the moment AMF decides to accept it), so a refusal consumes nothing.
    if (!report_ue_to_nsacf(peers, nsacf_client, nsacf_oauth, auth_state.supi, /*increase=*/true)) {
        // TS 24.501 §8.2.8 RegistrationReject with 5GMM cause 0x45
        // (INSUFFICIENT_RESOURCES_FOR_SLICE). Secured, because SecurityModeComplete just
        // succeeded and a NAS context exists -- see encode_registration_reject.
        constexpr std::uint8_t kMmCauseInsufficientResourcesForSlice = 0x45;
        const auto reject_nas =
            amf::nas::encode_registration_reject(*auth_state.knas_int,
                                                 *auth_state.knas_enc,
                                                 /*downlink_count=*/1,
                                                 kMmCauseInsufficientResourcesForSlice);
        send_downlink_nas_transport(assoc, auth_state.amf_ue_id, auth_state.ran_ue_id, reject_nas);
        spdlog::warn("amf-ngap: REGISTRATION REJECTED for SUPI {} -- NSACF refused its slice "
                     "(5GMM cause 0x45, insufficient resources for slice)",
                     auth_state.supi);
        // No security context is persisted and no PCF association is created: this UE is not
        // registered, so leaving either behind would make it look like it was.
        //
        // The phase is deliberately NOT rewound. This association's UeAuthState has no
        // "awaiting a fresh RegistrationRequest" phase to go back to (the machine starts at
        // AwaitingAuthenticationResponse), and inventing one would be a state-machine change
        // dressed as an error path. A retrying UE arrives as a new InitialUEMessage, which gets a
        // fresh UeAuthState of its own -- so the real retry path already works, and this
        // association is simply finished.
        return;
    }

    const auto tmsi = ue_security_contexts.allocate_tmsi();
    const auto reg_accept_nas = amf::nas::encode_registration_accept(*auth_state.knas_int,
                                                                     *auth_state.knas_enc,
                                                                     /*downlink_count=*/1,
                                                                     tmsi,
                                                                     amf_region_id,
                                                                     amf_set_id,
                                                                     amf_pointer);
    send_downlink_nas_transport(assoc, auth_state.amf_ue_id, auth_state.ran_ue_id, reg_accept_nas);
    spdlog::info("amf-ngap: sent DownlinkNASTransport with RegistrationAccept ({} bytes, "
                 "tmsi={:08x}), AMF-UE-NGAP-ID={}",
                 reg_accept_nas.size(),
                 tmsi,
                 auth_state.amf_ue_id);

    // LI IRI-POI hook (TS 33.127 6.2.2.4 "Registration", ADR-0377). The AMF has admitted the UE
    // and assigned its 5G-GUTI, so it is registered on the network side; the UE's
    // RegistrationComplete follows. Fired here, where SUPI + the just-allocated TMSI + this AMF's
    // GUTI parts are all in scope. No-op unless LI is enabled and this SUPI is a provisioned
    // target; delivery is best-effort (never breaks the registration). Disclosed: a stricter POI
    // would wait for RegistrationComplete and carry the UE's actual registration type.
    if (g_li_poi != nullptr && g_li_poi->is_target(auth_state.supi)) {
        GutiParts guti;
        guti.mcc = kMcc;
        guti.mnc = kMnc;
        guti.amf_region_id = amf_region_id;
        guti.amf_set_id = amf_set_id;
        guti.amf_pointer = amf_pointer;
        guti.five_g_tmsi = tmsi;
        g_li_poi->report_registration(auth_state.supi, guti);
    }

    // Real, persistent security context (gap-closure task #100/ADR-0075) -- uplink_count=1 and
    // downlink_count=2 are the NEXT expected values (SecurityModeComplete already consumed
    // uplink_count=0; RegistrationAccept just consumed downlink_count=1), matching
    // UeSecurityContextStore::next_uplink_count/next_downlink_count's own "allocate then use"
    // pre-increment convention -- the value stored here is what the NEXT real message on either
    // side will use.
    amf::UeSecurityContext ctx;
    ctx.supi = auth_state.supi;
    ctx.kamf = *auth_state.kamf;
    ctx.ngksi = 0; // this project's only security context per UE, ADR-0031 -- same fixed ngKSI=0
                   // SecurityModeCommand itself used.
    ctx.uplink_count = 1;
    ctx.downlink_count = 2;
    ctx.ue_security_capability = auth_state.ue_security_capability;
    ue_security_contexts.put(tmsi, ctx);

    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0090): real cross-association
    // index -- see amf_ue_id_index_store.hpp's own comment for why PathSwitchRequest (arriving on
    // a brand new association, from a different gNB) needs this to find this UE's persisted
    // security context by AMF-UE-NGAP-ID alone.
    amf_ue_id_index.put(auth_state.amf_ue_id, tmsi);

    auth_state.phase = UeAuthState::Phase::AwaitingRegistrationComplete;
}

// Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100/ADR-0075): real RegistrationComplete
// handling -- see UeAuthState::Phase's own comment for why this now genuinely fires (a real UE
// acknowledging the 5G-GUTI RegistrationAccept just assigned). uplink_count=1: the first secured
// uplink message after SecurityModeComplete's own uplink_count=0.
// ADR-0277: Network Slice Admission Control during Registration.
//
// TS 23.501 §5.15.11 makes NSAC the operator's control on how many UEs may be REGISTERED on a
// slice; TS 23.502 §4.2.2.2.2 puts the invocation in the Registration procedure, with AMF as the
// requesting NF. NSACF has existed in this project since ADR-0276 and nothing called it -- this is
// that call.
//
// Real, disclosed scope, stated here rather than left to be discovered: this AMF admits the UE
// and then reports it. It does not yet REFUSE a registration whose slice NSACF rejected -- doing
// that means failing the registration back to the UE with the right 5GMM cause and unwinding the
// PCF association already created, which is a behaviour change to the registration procedure
// itself rather than a call. So a rejection is logged loudly and the count is left accurate; the
// enforcement step is named as the next increment, not implied to exist.
void handle_uplink_nas_transport_registration_complete(const PeerEndpoints& peers,
                                                       sbi_core::http2::Client& pcf_client,
                                                       sbi_core::OAuth2Client& pcf_oauth,
                                                       UeContextStore& ue_contexts,
                                                       NgapUeRegistry& ue_ngap_registry,
                                                       ngap_core::SctpSocket& assoc,
                                                       UeAuthState& auth_state,
                                                       const InitiatingMessage_t& msg) {
    const auto nas_pdu_bytes_opt = extract_uplink_nas_pdu(msg);
    if (!nas_pdu_bytes_opt.has_value()) {
        return;
    }
    if (!auth_state.knas_int.has_value() || !auth_state.knas_enc.has_value()) {
        spdlog::warn("amf-ngap: received a post-RegistrationAccept UplinkNASTransport with no NAS "
                     "security context, ignoring");
        return;
    }
    const auto outcome = amf::nas::decode_registration_complete(
        *auth_state.knas_int, *auth_state.knas_enc, /*uplink_count=*/1, *nas_pdu_bytes_opt);
    if (!outcome.has_value()) {
        spdlog::warn("amf-ngap: UplinkNASTransport's NAS-PDU is not shaped like a "
                     "RegistrationComplete, ignoring");
        return;
    }
    if (!outcome->mac_valid) {
        spdlog::warn("amf-ngap: RegistrationComplete MAC verification FAILED for SUPI {} -- wrong "
                     "keys, a tampered/replayed message, or a NAS COUNT desync",
                     auth_state.supi);
        return;
    }

    spdlog::info("amf-ngap: RegistrationComplete verified OK for SUPI {}", auth_state.supi);
    auth_state.phase = UeAuthState::Phase::AwaitingPduSessionEstablishmentRequest;

    // Register this UE's live association so a later Namf_Communication N1N2MessageTransfer call
    // (arriving on the SBI HTTP/2 server's thread, from SMF once it has a real PDU Session
    // Establishment Accept to deliver) can reach it -- ADR-0038. downlink_count=2:
    // RegistrationAccept just used 1, so the next secured downlink message (the eventual Accept) is
    // genuinely 2.
    ue_ngap_registry.register_ue(
        auth_state.supi,
        NgapUeRegistry::Entry{&assoc,
                              static_cast<std::uint32_t>(auth_state.amf_ue_id),
                              static_cast<std::uint32_t>(auth_state.ran_ue_id),
                              *auth_state.knas_int,
                              *auth_state.knas_enc,
                              /*next_downlink_count=*/2});

    auto token = pcf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::error("amf-ngap: could not obtain PCF bearer token: {}", token.error());
        return;
    }

    sbi_gen::PolicyAssociationRequest_Npcf_AMPolicyControl preq{};
    preq.supi = auth_state.supi;
    // Mandatory per TS 29.507's schema even though nothing here implements a receiver yet -- see
    // the PeerEndpoints comment above.
    preq.notificationUri = peers.self_base + "/namf-callback/v1/am-policy-notify";
    // Mandatory (TS 29.571 §5.2.2 SupportedFeatures, a hex-encoded optional-feature bitmask) --
    // empty string means "none of PCF's optional features requested," the correct value given
    // this project doesn't implement any of them. Found via a real POST to PCF returning 400
    // "key 'suppFeat' not found" before this field was added -- not assumed correct from the
    // schema alone.
    preq.suppFeat = "";

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "POST";
    http_req.url = peers.pcf_base + "/npcf-am-policy-control/v1/policies";
    http_req.headers.emplace("content-type", "application/json");
    http_req.headers.emplace("authorization", "Bearer " + *token);
    http_req.body = nlohmann::json(preq).dump();

    auto resp = pcf_client.send(http_req);
    if (!resp.has_value()) {
        spdlog::error("amf-ngap: PCF CreateIndividualAMPolicyAssociation call failed: {}",
                      resp.error());
        return;
    }
    if (resp->status != 201) {
        spdlog::error("amf-ngap: PCF CreateIndividualAMPolicyAssociation returned unexpected "
                      "status {} for SUPI {}",
                      resp->status,
                      auth_state.supi);
        return;
    }

    nlohmann::json association_json;
    try {
        association_json = nlohmann::json::parse(resp->body);
    } catch (const nlohmann::json::exception& e) {
        spdlog::error("amf-ngap: PCF returned a malformed PolicyAssociation: {}", e.what());
        return;
    }

    ue_contexts.put(auth_state.supi, association_json);
    spdlog::info("amf-ngap: AM Policy Association established with PCF for SUPI {} -- UE "
                 "registration procedure fully complete",
                 auth_state.supi);
}

// PDU Session Establishment (TS 23.502 §4.3.2.2.1), AMF's side of it: decodes+verifies the
// UlNasTransport carrying the UE's PDU Session Establishment Request, then makes the real call
// this project's SMF turn stood its API up for but never had a live trigger for until now:
// Nsmf_PDUSession's CreateSMContext (TS 29.502), multipart/related per SMF's own handler
// (nfs/smf/src/main.cpp requires it, matching TS 29.502's schema).
//
// Deliberately does NOT decode the actual 5GSM PDU Session Establishment Request payload
// container itself (see amf::nas::decode_ul_nas_transport's own comment) -- it forwards the
// captured container bytes to SMF opaquely as n1SmMsg, and SMF is the one that decodes it. This
// function also does NOT send anything back to the UE directly: the real TS 23.502 mechanism for
// that is asynchronous (Namf_Communication N1N2MessageTransfer, called BY SMF once it has built a
// real PDU Session Establishment Accept, handled by the N1N2MessageTransfer route in main.cpp, not
// here) -- see ADR-0038, which closed the "no N1 SM Accept to forward" gap this comment used to
// describe.
void handle_uplink_nas_transport_pdu_session_establishment(const PeerEndpoints& peers,
                                                           sbi_core::http2::Client& smf_client,
                                                           sbi_core::OAuth2Client& smf_oauth,
                                                           const std::string& amf_instance_id,
                                                           UeAuthState& auth_state,
                                                           UeContextStore& ue_contexts,
                                                           NgapUeRegistry& ue_ngap_registry,
                                                           const InitiatingMessage_t& msg) {
    const auto nas_pdu_bytes_opt = extract_uplink_nas_pdu(msg);
    if (!nas_pdu_bytes_opt.has_value()) {
        return;
    }

    if (!auth_state.knas_int.has_value() || !auth_state.knas_enc.has_value()) {
        spdlog::warn("amf-ngap: received a post-registration UplinkNASTransport with no NAS "
                     "security context (out-of-order message or lost association state), ignoring");
        return;
    }

    // UPDATE (gap-closure task #100/ADR-0075): uplink_count=2, not 1 -- SecurityModeComplete was
    // uplink_count=0, and RegistrationComplete (now genuinely sent by a real UE, see
    // UeAuthState::Phase's own updated comment) consumed uplink_count=1, so this UlNasTransport is
    // genuinely the THIRD secured uplink message now, not the second.
    const auto outcome = amf::nas::decode_ul_nas_transport(*auth_state.knas_int,
                                                           *auth_state.knas_enc,
                                                           /*uplink_count=*/2,
                                                           *nas_pdu_bytes_opt);
    if (!outcome.has_value()) {
        spdlog::warn("amf-ngap: UplinkNASTransport's NAS-PDU is not shaped like a UlNasTransport "
                     "carrying N1 SM information, ignoring");
        return;
    }
    if (!outcome->mac_valid) {
        spdlog::warn("amf-ngap: UlNasTransport MAC verification FAILED for SUPI {} -- wrong keys, "
                     "a tampered/replayed message, or a NAS COUNT desync",
                     auth_state.supi);
        return;
    }
    auth_state.phase = UeAuthState::Phase::Done; // this project's only implemented SM procedure

    if (!outcome->dnn.has_value() || !outcome->snssai_sst.has_value()) {
        spdlog::error("amf-ngap: UlNasTransport for SUPI {} is missing DNN and/or S-NSSAI -- this "
                      "build requires both to call SMF (matches SMF's own CreateSMContext "
                      "requirement, see docs/DECISIONS.md ADR-0029), ignoring",
                      auth_state.supi);
        return;
    }

    spdlog::info("amf-ngap: PDU Session Establishment Request verified OK for SUPI {}, "
                 "pduSessionId={}, dnn={} -- requesting SM context from SMF",
                 auth_state.supi,
                 outcome->pdu_session_id,
                 *outcome->dnn);

    auto token = smf_oauth.get_bearer_token();
    if (!token.has_value()) {
        spdlog::error("amf-ngap: could not obtain SMF bearer token: {}", token.error());
        return;
    }

    sbi_gen::SmContextCreateData_Nsmf_PDUSession create_data{};
    create_data.servingNfId = amf_instance_id;
    create_data.servingNetwork.mcc = kMcc;
    create_data.servingNetwork.mnc = kMnc;
    create_data.anType.value = sbi_gen::AccessType::V3GPP_ACCESS;
    // Mandatory per TS 29.502's schema even though nothing here implements a receiver yet -- same
    // disclosed-deferred-callback shape as peers.self_base's other use (PCF's notificationUri).
    create_data.smContextStatusUri = peers.self_base + "/namf-callback/v1/sm-context-status";
    create_data.supi = auth_state.supi;
    create_data.pduSessionId = outcome->pdu_session_id;
    create_data.dnn = *outcome->dnn;
    sbi_gen::Snssai snssai{};
    snssai.sst = *outcome->snssai_sst;
    if (outcome->snssai_sd.has_value()) {
        char sd_hex[7];
        std::snprintf(sd_hex,
                      sizeof(sd_hex),
                      "%02x%02x%02x",
                      (*outcome->snssai_sd)[0],
                      (*outcome->snssai_sd)[1],
                      (*outcome->snssai_sd)[2]);
        snssai.sd = std::string(sd_hex);
    }
    create_data.sNssai = snssai;
    // Real N1 SM content (ADR-0038), not the opaque-and-dropped gap ADR-0036 disclosed: the
    // payload container bytes decode_ul_nas_transport captured verbatim, forwarded to SMF as the
    // real TS 29.502 mechanism intends -- AMF stays opaque to the content (contentId is an
    // arbitrary label, not itself meaningful), only SMF decodes it.
    sbi_gen::RefToBinaryData n1_sm_msg_ref{};
    n1_sm_msg_ref.contentId = "n1SmMsg";
    create_data.n1SmMsg = n1_sm_msg_ref;

    sbi_core::multipart::Part json_part;
    json_part.content_type = "application/json";
    json_part.body = nlohmann::json(create_data).dump();
    sbi_core::multipart::Part n1_sm_part;
    n1_sm_part.content_type = "application/vnd.3gpp.5gnas";
    n1_sm_part.content_id = "n1SmMsg";
    n1_sm_part.body.assign(outcome->payload_container.begin(), outcome->payload_container.end());
    const auto encoded = sbi_core::multipart::encode({json_part, n1_sm_part});

    sbi_core::http2::ClientRequest http_req;
    http_req.method = "POST";
    http_req.url = peers.smf_base + "/nsmf-pdusession/v1/sm-contexts";
    http_req.headers.emplace("content-type", encoded.content_type_header);
    http_req.headers.emplace("authorization", "Bearer " + *token);
    http_req.body = encoded.body;

    auto resp = smf_client.send(http_req);
    if (!resp.has_value()) {
        spdlog::error("amf-ngap: SMF CreateSMContext call failed: {}", resp.error());
        return;
    }
    if (resp->status != 201) {
        spdlog::error("amf-ngap: SMF CreateSMContext returned unexpected status {} for SUPI {}",
                      resp->status,
                      auth_state.supi);

        // ADR-0279: relay SMF's own NAS reject to the UE.
        //
        // Until now AMF discarded SMF's error body entirely, so a UE whose session SMF refused
        // waited for a message that never came. TS 29.502's SmContextCreateError carries the
        // reason the UE is owed in `n1SmMsg` -- AMF is the only party that can deliver it, since
        // SMF has no session to run an N1N2MessageTransfer against. AMF does not decode the
        // container (same opaque-payload discipline as the Accept path); it carries the bytes.
        const auto error_ct = resp->headers.find("content-type");
        if (error_ct == resp->headers.end() ||
            !sbi_core::multipart::is_multipart_related(error_ct->second)) {
            return;
        }
        auto error_parts = sbi_core::multipart::parse(error_ct->second, resp->body);
        if (!error_parts.has_value() || error_parts->size() < 2) {
            return;
        }
        std::string wanted = "n1SmMsg";
        try {
            const auto root = nlohmann::json::parse((*error_parts)[0].body);
            if (root.contains("n1SmMsg") && root.at("n1SmMsg").contains("contentId")) {
                wanted = root.at("n1SmMsg").at("contentId").get<std::string>();
            }
        } catch (const nlohmann::json::exception&) {
            // Fall through to the default contentId, same as the success path's own handling.
        }
        for (const auto& part : *error_parts) {
            if (!part.content_id.has_value() || *part.content_id != wanted) {
                continue;
            }
            const std::vector<std::uint8_t> n1_sm(part.body.begin(), part.body.end());
            // Through the registry, exactly as the ACCEPT is delivered (SMF's N1N2MessageTransfer
            // lands on AMF's SBI server, which calls this same method). This handler holds no
            // association of its own by design, and the registry owns the UE's downlink NAS COUNT
            // -- so a reject and an accept consume the same one count, which is correct: a UE
            // receives exactly one of them, never both.
            if (!ue_ngap_registry.send_dl_nas_transport(
                    auth_state.supi, static_cast<std::uint8_t>(outcome->pdu_session_id), n1_sm)) {
                spdlog::warn("amf-ngap: no live association for SUPI {} -- SMF's PDU session "
                             "reject could not be delivered",
                             auth_state.supi);
                break;
            }
            spdlog::warn("amf-ngap: relayed SMF's PDU session reject to SUPI {} (pduSessionId {})",
                         auth_state.supi,
                         outcome->pdu_session_id);
            break;
        }
        return;
    }

    // ADR-0249: capture the real SM context reference SMF just created. Until now AMF read the
    // 201 and threw the `Location` header away, which is the REAL reason "AMF doesn't call SMF
    // during handover" was an open gap -- it was not that the call was skipped, it is that AMF
    // structurally held no handle to make any subsequent Nsmf_PDUSession_UpdateSMContext call
    // with. TS 29.502 returns the created resource's URI in `Location`; the ref is its last path
    // segment. Stored per PDU session id under the UE's own SUPI-keyed context, alongside the
    // association data ue_contexts already holds.
    std::string sm_context_ref;
    for (const auto& [name, value] : resp->headers) {
        std::string lower_name;
        lower_name.reserve(name.size());
        for (const char c : name) {
            lower_name.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        if (lower_name == "location") {
            const auto slash = value.find_last_of('/');
            sm_context_ref = slash == std::string::npos ? value : value.substr(slash + 1);
            break;
        }
    }
    if (sm_context_ref.empty()) {
        // Real, disclosed: SMF's own 201 is required by TS 29.502 to carry Location. If it did
        // not, say so rather than silently continuing with no ref -- a later handover would then
        // fail with a confusing lookup miss instead of pointing at this.
        spdlog::warn("amf-ngap: SMF CreateSMContext 201 carried no Location header -- no SM "
                     "context ref stored for SUPI {}, pduSessionId={}; any later "
                     "UpdateSMContext (e.g. N2 handover) for this session cannot be made",
                     auth_state.supi,
                     outcome->pdu_session_id);
    } else {
        auto ue_ctx = ue_contexts.get(auth_state.supi);
        nlohmann::json ctx = ue_ctx.has_value() ? *ue_ctx : nlohmann::json::object();
        ctx["smContextRefs"][std::to_string(outcome->pdu_session_id)] = sm_context_ref;
        ue_contexts.put(auth_state.supi, ctx);
    }

    spdlog::info("amf-ngap: SM context established with SMF for SUPI {}, pduSessionId={} "
                 "(smContextRef={}) -- SMF will deliver the PDU Session Establishment Accept via "
                 "a separate N1N2MessageTransfer call (ADR-0038)",
                 auth_state.supi,
                 outcome->pdu_session_id,
                 sm_context_ref.empty() ? "<none>" : sm_context_ref);
}

void handle_association(const PeerEndpoints& peers,
                        ngap_core::SctpSocket assoc,
                        sbi_core::http2::Client& nsacf_client,
                        sbi_core::OAuth2Client& nsacf_oauth,
                        sbi_core::http2::Client& ausf_client,
                        sbi_core::OAuth2Client& ausf_oauth,
                        sbi_core::http2::Client& pcf_client,
                        sbi_core::OAuth2Client& pcf_oauth,
                        sbi_core::http2::Client& smf_client,
                        sbi_core::OAuth2Client& smf_oauth,
                        const std::string& amf_instance_id,
                        UeContextStore& ue_contexts,
                        NgapUeRegistry& ue_ngap_registry,
                        UeSecurityContextStore& ue_security_contexts,
                        AmfUeIdIndexStore& amf_ue_id_index,
                        amf::ngap::GnbAssociationRegistry& gnb_associations,
                        std::uint8_t amf_region_id,
                        std::uint16_t amf_set_id,
                        std::uint8_t amf_pointer) {
    spdlog::info("amf-ngap: gNB association established");
    UeAuthState auth_state{}; // this association's single UE, see UeAuthState's own comment
    // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0095): this association's own
    // real gNB identity, captured at NGSetupRequest -- registered into gnb_associations so a real
    // N2 handover elsewhere can route a HandoverRequest onto THIS association, unregistered when
    // it closes below.
    std::optional<std::vector<std::uint8_t>> this_gnb_id;
    while (true) {
        auto bytes = assoc.receive();
        if (bytes.empty()) {
            spdlog::info("amf-ngap: gNB association closed");
            if (!auth_state.supi.empty()) {
                ue_ngap_registry.unregister_ue(auth_state.supi);
            }
            if (this_gnb_id.has_value()) {
                gnb_associations.unregister_gnb(*this_gnb_id);
            }
            return;
        }

        NGAP_PDU_t* pdu = ::ngap::decode_pdu(bytes);
        if (pdu == nullptr) {
            std::string hex;
            for (auto b : bytes) {
                char buf[4];
                std::snprintf(buf, sizeof(buf), "%02x", b);
                hex += buf;
            }
            spdlog::warn(
                "amf-ngap: failed to decode NGAP PDU ({} bytes), ignoring: {}", bytes.size(), hex);
            continue;
        }

        if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
            pdu->choice.initiatingMessage->procedureCode == 21 /* id-NGSetup */) {
            this_gnb_id = handle_ng_setup_request(assoc, *pdu->choice.initiatingMessage);
            if (this_gnb_id.has_value()) {
                gnb_associations.register_gnb(*this_gnb_id, &assoc);
            }
        } else if ((pdu->present == NGAP_PDU_PR_successfulOutcome &&
                    pdu->choice.successfulOutcome->procedureCode ==
                        13 /* id-HandoverResourceAllocation */) ||
                   (pdu->present == NGAP_PDU_PR_unsuccessfulOutcome &&
                    pdu->choice.unsuccessfulOutcome->procedureCode ==
                        13 /* id-HandoverResourceAllocation */)) {
            // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0096): a real
            // HandoverRequestAcknowledge/HandoverFailure -- if handle_handover_required (running
            // on a DIFFERENT association's thread, the source) is blocked awaiting a reply for
            // THIS gNB, hand it over via the registry instead of a normal handler; otherwise this
            // is an unexpected/unsolicited reply, logged and dropped.
            const bool delivered = this_gnb_id.has_value() &&
                                   gnb_associations.deliver_reply_if_pending(*this_gnb_id, bytes);
            if (!delivered) {
                spdlog::warn("amf-ngap: received a HandoverRequestAcknowledge/HandoverFailure "
                             "with no outstanding relay for this gNB, ignoring");
            }
        } else if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
                   pdu->choice.initiatingMessage->procedureCode ==
                       12 /* id-HandoverPreparation */) {
            // Real HandoverRequired -- runs on the SOURCE association's own thread. Real, cold
            // lookup via amf_ue_id_index (same as PathSwitchRequest), NOT this association's own
            // auth_state -- see handle_handover_required's own header comment for why.
            handle_handover_required(assoc,
                                     peers,
                                     smf_client,
                                     smf_oauth,
                                     ue_contexts,
                                     ue_security_contexts,
                                     amf_ue_id_index,
                                     gnb_associations,
                                     amf_region_id,
                                     amf_set_id,
                                     amf_pointer,
                                     *pdu->choice.initiatingMessage);
        } else if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
                   pdu->choice.initiatingMessage->procedureCode == 10 /* id-HandoverCancel */) {
            // ADR-0261: the source gNB abandoning a handover it already prepared. Runs on the
            // SOURCE association's own thread, same cold-lookup discipline as
            // handle_handover_required.
            handle_handover_cancel(assoc,
                                   peers,
                                   smf_client,
                                   smf_oauth,
                                   ue_contexts,
                                   ue_security_contexts,
                                   amf_ue_id_index,
                                   gnb_associations,
                                   *pdu->choice.initiatingMessage);
        } else if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
                   pdu->choice.initiatingMessage->procedureCode ==
                       11 /* id-HandoverNotification */) {
            // Real HandoverNotify -- arrives on the TARGET association's own thread. See
            // handle_handover_notify's own header comment.
            handle_handover_notify(assoc,
                                   peers,
                                   smf_client,
                                   smf_oauth,
                                   ue_contexts,
                                   ue_ngap_registry,
                                   ue_security_contexts,
                                   amf_ue_id_index,
                                   *pdu->choice.initiatingMessage);
        } else if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
                   pdu->choice.initiatingMessage->procedureCode == 15 /* id-InitialUEMessage */) {
            handle_initial_ue_message(peers,
                                      assoc,
                                      ausf_client,
                                      ausf_oauth,
                                      ue_security_contexts,
                                      ue_ngap_registry,
                                      auth_state,
                                      *pdu->choice.initiatingMessage);
        } else if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
                   pdu->choice.initiatingMessage->procedureCode == 46 /* id-UplinkNASTransport */) {
            switch (auth_state.phase) {
                case UeAuthState::Phase::AwaitingAuthenticationResponse:
                    handle_uplink_nas_transport(peers,
                                                assoc,
                                                ausf_client,
                                                ausf_oauth,
                                                auth_state,
                                                *pdu->choice.initiatingMessage);
                    break;
                case UeAuthState::Phase::AwaitingSecurityModeComplete:
                    handle_uplink_nas_transport_smc_complete(peers,
                                                             nsacf_client,
                                                             nsacf_oauth,
                                                             assoc,
                                                             ue_security_contexts,
                                                             amf_ue_id_index,
                                                             amf_region_id,
                                                             amf_set_id,
                                                             amf_pointer,
                                                             auth_state,
                                                             *pdu->choice.initiatingMessage);
                    break;
                case UeAuthState::Phase::AwaitingRegistrationComplete:
                    handle_uplink_nas_transport_registration_complete(
                        peers,
                        pcf_client,
                        pcf_oauth,
                        ue_contexts,
                        ue_ngap_registry,
                        assoc,
                        auth_state,
                        *pdu->choice.initiatingMessage);
                    break;
                case UeAuthState::Phase::AwaitingPduSessionEstablishmentRequest:
                    handle_uplink_nas_transport_pdu_session_establishment(
                        peers,
                        smf_client,
                        smf_oauth,
                        amf_instance_id,
                        auth_state,
                        ue_contexts,
                        ue_ngap_registry,
                        *pdu->choice.initiatingMessage);
                    break;
                case UeAuthState::Phase::Done:
                    spdlog::warn(
                        "amf-ngap: received an UplinkNASTransport after this association's "
                        "one PDU session was already established for SUPI {}, ignoring (out "
                        "of scope: no second PDU session or other post-establishment NAS "
                        "procedure implemented yet)",
                        auth_state.supi);
                    break;
            }
        } else if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
                   pdu->choice.initiatingMessage->procedureCode ==
                       42 /* id-UEContextReleaseRequest */) {
            handle_ue_context_release_request(assoc, auth_state, *pdu->choice.initiatingMessage);
        } else if (pdu->present == NGAP_PDU_PR_successfulOutcome &&
                   pdu->choice.successfulOutcome->procedureCode == 41 /* id-UEContextRelease */) {
            handle_ue_context_release_complete(
                ue_ngap_registry, auth_state, *pdu->choice.successfulOutcome);
        } else if (pdu->present == NGAP_PDU_PR_initiatingMessage &&
                   pdu->choice.initiatingMessage->procedureCode == 25 /* id-PathSwitchRequest */) {
            // Arrives on a brand new association (the target gNB), not this association's own
            // auth_state -- see handle_path_switch_request's own header comment.
            handle_path_switch_request(assoc,
                                       ue_security_contexts,
                                       amf_ue_id_index,
                                       ue_ngap_registry,
                                       *pdu->choice.initiatingMessage);
        } else {
            spdlog::warn("amf-ngap: received NGAP PDU (present={}) with no handler yet, ignoring",
                         static_cast<int>(pdu->present));
        }

        ASN_STRUCT_FREE(asn_DEF_NGAP_PDU, pdu);
    }
}

} // namespace

void NgapUeRegistry::register_ue(const std::string& supi, Entry entry) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_[supi] = entry;
}

void NgapUeRegistry::unregister_ue(const std::string& supi) {
    std::lock_guard<std::mutex> lock(mutex_);
    entries_.erase(supi);
}

bool NgapUeRegistry::send_dl_nas_transport(const std::string& supi,
                                           std::uint8_t pdu_session_id,
                                           const std::vector<std::uint8_t>& n1_sm_container) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(supi);
    if (it == entries_.end() || it->second.socket == nullptr) {
        return false;
    }
    Entry& entry = it->second;
    const auto nas_bytes = amf::nas::encode_dl_nas_transport(
        entry.knas_int, entry.knas_enc, entry.next_downlink_count, pdu_session_id, n1_sm_container);
    send_downlink_nas_transport(*entry.socket, entry.amf_ue_id, entry.ran_ue_id, nas_bytes);
    entry.next_downlink_count += 1;
    return true;
}

bool NgapUeRegistry::send_raw(const std::string& supi, const std::vector<std::uint8_t>& pdu_bytes) {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = entries_.find(supi);
    if (it == entries_.end() || it->second.socket == nullptr) {
        return false;
    }
    it->second.socket->send(pdu_bytes);
    return true;
}

// Real, spawned once per accepted association -- constructs THIS association's own dedicated
// AUSF/PCF/SMF clients (sbi_core::http2::Client is synchronous/not thread-shared, same
// "separate thread gets its own separate client" discipline ADR-0006/ADR-0027 already
// established) and runs handle_association until the association closes. Gap-closure
// (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0095): moved out of run_ngap_lifecycle's own
// scope (previously constructed ONCE and shared by reference across every sequentially-handled
// association) specifically because real concurrency means two associations' threads could now
// otherwise race on the same non-thread-safe client -- a real correctness requirement, not
// stylistic.
void run_association_thread(ngap_core::SctpSocket assoc,
                            PeerEndpoints peers,
                            const std::string& amf_instance_id,
                            const std::string& nrf_base,
                            UeContextStore& ue_contexts,
                            NgapUeRegistry& ue_ngap_registry,
                            UeSecurityContextStore& ue_security_contexts,
                            AmfUeIdIndexStore& amf_ue_id_index,
                            amf::ngap::GnbAssociationRegistry& gnb_associations,
                            std::uint8_t amf_region_id,
                            std::uint16_t amf_set_id,
                            std::uint8_t amf_pointer) {
    sbi_core::http2::TlsConfig ausf_client_tls{
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client ausf_client(std::move(ausf_client_tls));
    sbi_core::OAuth2Client ausf_oauth(
        ausf_client, nrf_base + "/oauth2/token", amf_instance_id, "nausf-auth", "AUSF");

    sbi_core::http2::TlsConfig pcf_client_tls{
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client pcf_client(std::move(pcf_client_tls));
    sbi_core::OAuth2Client pcf_oauth(
        pcf_client, nrf_base + "/oauth2/token", amf_instance_id, "npcf-am-policy-control", "PCF");

    // ADR-0277: this association's own NSACF client, same per-association ownership as every other
    // client here (see this function's own comment) rather than a shared one.
    sbi_core::http2::TlsConfig nsacf_client_tls{
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client nsacf_client(std::move(nsacf_client_tls));
    sbi_core::OAuth2Client nsacf_oauth(
        nsacf_client, nrf_base + "/oauth2/token", amf_instance_id, "nnsacf-nsac", "NSACF");

    sbi_core::http2::TlsConfig smf_client_tls{
        .cert_path = CERTS_DIR "/amf/cert.pem",
        .key_path = CERTS_DIR "/amf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    sbi_core::http2::Client smf_client(std::move(smf_client_tls));
    sbi_core::OAuth2Client smf_oauth(
        smf_client, nrf_base + "/oauth2/token", amf_instance_id, "nsmf-pdusession", "SMF");

    handle_association(peers,
                       std::move(assoc),
                       nsacf_client,
                       nsacf_oauth,
                       ausf_client,
                       ausf_oauth,
                       pcf_client,
                       pcf_oauth,
                       smf_client,
                       smf_oauth,
                       amf_instance_id,
                       ue_contexts,
                       ue_ngap_registry,
                       ue_security_contexts,
                       amf_ue_id_index,
                       gnb_associations,
                       amf_region_id,
                       amf_set_id,
                       amf_pointer);
}

void run_ngap_lifecycle(const std::string& bind_address,
                        unsigned short bind_port,
                        const std::string& amf_instance_id,
                        const std::string& nrf_base,
                        const PeerEndpoints& peers,
                        UeContextStore& ue_contexts,
                        NgapUeRegistry& ue_ngap_registry,
                        UeSecurityContextStore& ue_security_contexts,
                        AmfUeIdIndexStore& amf_ue_id_index,
                        GnbAssociationRegistry& gnb_associations,
                        std::uint8_t amf_region_id,
                        std::uint16_t amf_set_id,
                        std::uint8_t amf_pointer,
                        LiPoi* li_poi) {
    // Publish the POI for the SMC-complete handler's Registration hook. Set before the accept
    // loop, so it is visible to every association thread. nullptr when LI is disabled.
    g_li_poi = li_poi;

    ngap_core::SctpSocket listener;
    listener.bind_and_listen(bind_address, bind_port);
    spdlog::info("amf-ngap: listening for NGAP/N2 (SCTP) on {}:{}", bind_address, bind_port);

    while (true) {
        ngap_core::SctpSocket assoc = listener.accept();
        // Gap-closure (docs/CAPABILITY_GAP_ANALYSIS.md task #100, ADR-0095): real concurrent
        // association handling -- one std::thread per accepted association (was strictly
        // sequential, ADR-0031: "a real AMF would handle multiple concurrent associations").
        // Detached: this lab has no coordinated shutdown path for in-flight associations (same
        // real, disclosed scope every other detached/fire-and-forget thread in this project
        // already carries), the process exiting is what ends them.
        std::thread(run_association_thread,
                    std::move(assoc),
                    // By value, not std::ref: this thread is detached and outlives the frame that
                    // owns `peers` in main()'s caller chain. The stores below are std::ref'd
                    // because main() owns them for the whole process lifetime; a 4-string copy
                    // per association costs nothing and removes the lifetime question entirely.
                    peers,
                    amf_instance_id,
                    nrf_base,
                    std::ref(ue_contexts),
                    std::ref(ue_ngap_registry),
                    std::ref(ue_security_contexts),
                    std::ref(amf_ue_id_index),
                    std::ref(gnb_associations),
                    amf_region_id,
                    amf_set_id,
                    amf_pointer)
            .detach();
    }
}

} // namespace amf::ngap
