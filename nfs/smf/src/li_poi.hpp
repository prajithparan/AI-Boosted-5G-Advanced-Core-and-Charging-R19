#pragma once

#include <nlohmann/json.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "li_core/xiri.hpp"
#include "li_poi/poi_runtime.hpp"

// The SMF's IRI-POI (TS 33.127 clause 6.2.3, TS 33.128 clause 6.2.3, ADR-0463): the generic POI
// machinery is libs/li-poi; this keeps what is SMF-specific -- which identities match (SUPI, PEI
// and GPSI independently, 6.2.3.2), which xIRI records exist, and the registry of established PDU
// sessions that "start of interception with an established PDU session" (6.2.3.3) needs.
//
// Every report_* is a cheap no-op unless a provisioned warrant targets one of the session's
// identities, and delivery is best-effort: a failure is logged, never propagated into the PDU
// session procedure that triggered it.
namespace smf {

// What the SMF knows about one PDU session, in the SBI's own forms ("imsi-...", "imeisv-...",
// "msisdn-...") -- the POI converts them to the xIRI's.
struct LiSession {
    std::string supi;
    std::optional<std::string> pei;
    std::optional<std::string> gpsi;
    std::uint8_t pdu_session_id = 0;
    // The UPF N3 uplink tunnel. gTPTunnelID is MANDATORY in the establishment xIRI, so a session
    // with no real tunnel (no UPF reached) is NOT reported as established rather than given a
    // made-up one.
    std::optional<li_core::xiri::Fteid> gtp_tunnel;
    li_core::xiri::PduSessionType pdu_session_type = li_core::xiri::PduSessionType::IPv4;
    std::optional<li_core::xiri::Snssai> snssai;
    std::vector<li_core::xiri::UeEndpoint> ue_endpoints;
    std::optional<li_core::xiri::Location> location;
    std::string dnn;
    li_core::xiri::SmRequestType request_type = li_core::xiri::SmRequestType::InitialRequest;
    std::optional<li_core::xiri::AccessType> access_type;
};

// A procedure the SMF refused (TS 33.128 6.2.3.2.6).
struct LiUnsuccessful {
    li_core::xiri::SmFailedProcedure procedure =
        li_core::xiri::SmFailedProcedure::PduSessionEstablishment;
    std::uint8_t five_gsm_cause = 31; // TS 24.501 9.11.4.2: #31 "request rejected, unspecified"
    li_core::xiri::SmInitiator initiator = li_core::xiri::SmInitiator::Ue;
    LiSession session; // whatever of it is known (supi / pei / gpsi / pdu id / dnn at least)
    bool has_pdu_session_id = false;
};

// The SBI forms the SMF already parses -> a LiSession. `create_body` is the first part of the
// CreateSMContext request (SmContextCreateData). The UPF tunnel comes from the SM context the N4
// establishment populated (`ulTeid` / `ulIpv4`), not from the request.
LiSession li_session_from_create(const nlohmann::json& create_body);
void li_set_tunnel(LiSession& session, const nlohmann::json& sm_context);
// What the SMF keeps in its SM context (under "li") so release / modification can rebuild the
// session.
nlohmann::json li_context_record(const LiSession& session);
LiSession li_session_from_context(const nlohmann::json& sm_context);

class SmfLiPoi {
public:
    explicit SmfLiPoi(li_poi::Config config);
    ~SmfLiPoi();
    SmfLiPoi(const SmfLiPoi&) = delete;
    SmfLiPoi& operator=(const SmfLiPoi&) = delete;

    void start();
    void stop();

    // True if an active warrant targets any of the session's identities.
    [[nodiscard]] bool is_target(const LiSession& session) const;

    // 6.2.3.3: the SMF sent PDU SESSION ESTABLISHMENT ACCEPT and the session is active. Also
    // records the session as established (BEFORE matching, so a warrant activated concurrently is
    // caught by at least one of this call and the targets-added worker -- an over-report, never a
    // miss).
    void report_establishment(const LiSession& session);
    // The session was modified (handover completion, path switch).
    void report_modification(const LiSession& session);
    // The session was released. Forgets it afterwards.
    void report_release(const LiSession& session);
    // A procedure for this UE was rejected.
    void report_unsuccessful(const LiUnsuccessful& failure);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace smf
