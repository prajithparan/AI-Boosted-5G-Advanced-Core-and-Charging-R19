#include "li_poi.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <array>
#include <map>
#include <mutex>

namespace smf {

namespace xiri = li_core::xiri;
namespace x1 = li_core::x1;

namespace {

// SBI identity -> the bare value X1 target identifiers and xIRIs use. Applied to BOTH a warrant's
// target value and a session's identity before comparing, so "imsi-2040..." and "2040..." match.
std::string bare_identity(const std::string& s) {
    for (const char* prefix : {"imsi-", "nai-", "supi-", "imei-", "imeisv-", "msisdn-", "extid-"}) {
        const std::size_t len = std::char_traits<char>::length(prefix);
        if (s.size() >= len && s.compare(0, len, prefix) == 0) {
            return s.substr(len);
        }
    }
    return s;
}

bool starts_with(const std::string& s, const char* prefix) {
    return s.rfind(prefix, 0) == 0;
}

// TS 29.571 Pei "imei-<15 digits>" carries the check digit; the xIRI's IMEI (TS 33.128,
// NumericString SIZE(14)) and X1's `imei` do not. IMEISV is 16 digits in both.
std::string pei_for_matching(const std::string& pei) {
    std::string bare = bare_identity(pei);
    if (starts_with(pei, "imei-") && bare.size() == 15) {
        bare.pop_back();
    }
    return bare;
}

// TS 33.127 6.2.3.1.2: the identifier formats the SMF's POI/TF must support.
const std::vector<x1::TargetIdentifierKind>& supported_kinds() {
    using K = x1::TargetIdentifierKind;
    static const std::vector<K> kinds{
        K::SupiImsi, K::SupiNai, K::PeiImei, K::PeiImeisv, K::GpsiMsisdn, K::GpsiNai};
    return kinds;
}
const std::vector<x1::TargetIdentifierKind>& supi_kinds() {
    using K = x1::TargetIdentifierKind;
    static const std::vector<K> kinds{K::SupiImsi, K::SupiNai};
    return kinds;
}
const std::vector<x1::TargetIdentifierKind>& pei_kinds() {
    using K = x1::TargetIdentifierKind;
    static const std::vector<K> kinds{K::PeiImei, K::PeiImeisv};
    return kinds;
}
const std::vector<x1::TargetIdentifierKind>& gpsi_kinds() {
    using K = x1::TargetIdentifierKind;
    static const std::vector<K> kinds{K::GpsiMsisdn, K::GpsiNai};
    return kinds;
}

xiri::Supi to_xiri_supi(const std::string& supi) {
    const std::string bare = bare_identity(supi);
    return starts_with(supi, "nai-") ? xiri::Supi{xiri::Nai{bare}} : xiri::Supi{xiri::Imsi{bare}};
}

std::optional<xiri::Pei> to_xiri_pei(const std::optional<std::string>& pei) {
    if (!pei) {
        return std::nullopt;
    }
    const std::string v = pei_for_matching(*pei);
    if (v.size() == 16) {
        return xiri::Pei{xiri::Imeisv{v}};
    }
    if (v.size() == 14) {
        return xiri::Pei{xiri::Imei{v}};
    }
    return std::nullopt; // not an IMEI / IMEISV (e.g. a MAC or EUI-64 PEI): not modelled
}

std::optional<xiri::Gpsi> to_xiri_gpsi(const std::optional<std::string>& gpsi) {
    if (!gpsi) {
        return std::nullopt;
    }
    const std::string bare = bare_identity(*gpsi);
    if (starts_with(*gpsi, "msisdn-")) {
        return xiri::Gpsi{xiri::Msisdn{bare}};
    }
    return xiri::Gpsi{xiri::Nai{bare}};
}

xiri::SmIdentities to_identities(const LiSession& s) {
    xiri::SmIdentities ids;
    if (!s.supi.empty()) {
        ids.supi = to_xiri_supi(s.supi);
    }
    ids.pei = to_xiri_pei(s.pei);
    ids.gpsi = to_xiri_gpsi(s.gpsi);
    return ids;
}

} // namespace

struct SmfLiPoi::Impl {
    explicit Impl(li_poi::Config cfg) : runtime(std::move(cfg), make_hooks()) {}

    li_poi::Hooks make_hooks() {
        li_poi::Hooks h;
        h.log_name = "smf-li-poi";
        h.supported_kinds = supported_kinds();
        h.normalise = [](const std::string& s) { return bare_identity(s); };
        h.on_targets_added = [this](const std::string& xid,
                                    const std::vector<x1::TargetIdentifier>& added) {
            start_of_interception(xid, added);
        };
        return h;
    }

    // Every task that targets any identity of the session; one Match per task (a session whose SUPI
    // and GPSI are both targeted by the same warrant is reported once).
    std::vector<li_poi::Match> matches(const LiSession& s) const {
        std::vector<li_poi::Match> out;
        const auto add = [&](std::vector<li_poi::Match> found) {
            for (auto& m : found) {
                const bool dup = std::any_of(
                    out.begin(), out.end(), [&](const auto& o) { return o.xid == m.xid; });
                if (!dup) {
                    out.push_back(std::move(m));
                }
            }
        };
        if (!s.supi.empty()) {
            add(runtime.matches(s.supi, supi_kinds()));
        }
        if (s.pei) {
            add(runtime.matches(pei_for_matching(*s.pei), pei_kinds()));
        }
        if (s.gpsi) {
            add(runtime.matches(*s.gpsi, gpsi_kinds()));
        }
        return out;
    }

    void emit(const li_poi::Match& match,
              const xiri::Event& event,
              li_core::PayloadDirection direction,
              const char* record) {
        const auto payload = xiri::encode_xiri_payload(event);
        if (!payload) {
            spdlog::error("smf-li-poi: could not encode {} xIRI: {}", record, payload.error());
            return;
        }
        runtime.emit(match, *payload, direction, record);
    }

    static std::string key(const std::string& supi, std::uint8_t pdu_session_id) {
        return bare_identity(supi) + "/" + std::to_string(pdu_session_id);
    }

    // TS 33.128 6.2.3.2.5: a warrant newly activated on a UE that already has PDU sessions: one
    // record per session ("with a different value of correlation information"), under THIS task's
    // XID only.
    void start_of_interception(const std::string& xid,
                               const std::vector<x1::TargetIdentifier>& added) {
        std::vector<LiSession> sessions;
        {
            const std::lock_guard<std::mutex> lock(registry_mutex);
            for (const auto& [k, s] : established) {
                sessions.push_back(s);
            }
        }
        for (const auto& s : sessions) {
            for (const auto& m : matches(s)) {
                if (m.xid != xid) {
                    continue;
                }
                // only a target this ModifyTask/ActivateTask ADDED starts an interception
                const bool newly = std::any_of(added.begin(), added.end(), [&](const auto& t) {
                    return t.element == m.target.element && t.value == m.target.value;
                });
                if (!newly || !s.gtp_tunnel) {
                    continue;
                }
                xiri::SmfStartOfInterceptionWithEstablishedPduSession soi;
                soi.ids = to_identities(s);
                soi.pdu_session_id = s.pdu_session_id;
                soi.gtp_tunnel = *s.gtp_tunnel;
                soi.pdu_session_type = s.pdu_session_type;
                soi.snssai = s.snssai;
                soi.ue_endpoints = s.ue_endpoints;
                soi.location = s.location;
                soi.dnn = s.dnn;
                soi.request_type = xiri::SmRequestType::ExistingPduSession;
                soi.access_type = s.access_type;
                // 6.2.3.2.5: "shall set the Payload Direction field ... to not applicable
                // (Direction Value 5)".
                emit(m,
                     soi,
                     li_core::PayloadDirection::NotApplicable,
                     "SMFStartOfInterceptionWithEstablishedPDUSession");
            }
        }
    }

    li_poi::PoiRuntime runtime;
    std::mutex registry_mutex;
    std::map<std::string, LiSession> established; // "<bare supi>/<pdu session id>"
};

SmfLiPoi::SmfLiPoi(li_poi::Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}
SmfLiPoi::~SmfLiPoi() = default;

void SmfLiPoi::start() {
    impl_->runtime.start();
}
void SmfLiPoi::stop() {
    impl_->runtime.stop();
}

bool SmfLiPoi::is_target(const LiSession& session) const {
    return !impl_->matches(session).empty();
}

void SmfLiPoi::report_establishment(const LiSession& session) {
    if (!session.gtp_tunnel) {
        spdlog::warn(
            "smf-li-poi: PDU session {} of {} has no UPF tunnel -- no SMFPDUSessionEstablishment "
            "(gTPTunnelID is mandatory and is never invented)",
            session.pdu_session_id,
            session.supi);
        return;
    }
    {
        const std::lock_guard<std::mutex> lock(impl_->registry_mutex);
        impl_->established[Impl::key(session.supi, session.pdu_session_id)] = session;
    }
    for (const auto& m : impl_->matches(session)) {
        xiri::SmfPduSessionEstablishment e;
        e.ids = to_identities(session);
        e.pdu_session_id = session.pdu_session_id;
        e.gtp_tunnel = *session.gtp_tunnel;
        e.pdu_session_type = session.pdu_session_type;
        e.snssai = session.snssai;
        e.ue_endpoints = session.ue_endpoints;
        e.location = session.location;
        e.dnn = session.dnn;
        e.request_type = session.request_type;
        e.access_type = session.access_type;
        // A UE-initiated procedure: table 5.3.2-1 sets the direction from the initiator.
        impl_->emit(m, e, li_core::PayloadDirection::FromTarget, "SMFPDUSessionEstablishment");
    }
}

void SmfLiPoi::report_modification(const LiSession& session) {
    for (const auto& m : impl_->matches(session)) {
        xiri::SmfPduSessionModification e;
        e.ids = to_identities(session);
        e.snssai = session.snssai;
        e.location = session.location;
        e.request_type = xiri::SmRequestType::ModificationRequest;
        e.access_type = session.access_type;
        e.pdu_session_id = session.pdu_session_id;
        // Network-detected (handover completion): neither to nor from the target (value 5), the
        // same interpretation ADR-0440 records for the AMF's network-detected records.
        impl_->emit(m, e, li_core::PayloadDirection::NotApplicable, "SMFPDUSessionModification");
    }
}

void SmfLiPoi::report_release(const LiSession& session) {
    for (const auto& m : impl_->matches(session)) {
        xiri::SmfPduSessionRelease e;
        e.supi = to_xiri_supi(session.supi);
        e.pei = to_xiri_pei(session.pei);
        e.gpsi = to_xiri_gpsi(session.gpsi);
        e.pdu_session_id = session.pdu_session_id;
        e.location = session.location;
        impl_->emit(m, e, li_core::PayloadDirection::NotApplicable, "SMFPDUSessionRelease");
    }
    const std::lock_guard<std::mutex> lock(impl_->registry_mutex);
    impl_->established.erase(Impl::key(session.supi, session.pdu_session_id));
}

void SmfLiPoi::report_unsuccessful(const LiUnsuccessful& failure) {
    const LiSession& s = failure.session;
    for (const auto& m : impl_->matches(s)) {
        xiri::SmfUnsuccessfulProcedure e;
        e.failed_procedure = failure.procedure;
        e.failure_cause = failure.five_gsm_cause;
        e.initiator = failure.initiator;
        e.ids = to_identities(s);
        if (failure.has_pdu_session_id) {
            e.pdu_session_id = s.pdu_session_id;
        }
        if (!s.dnn.empty()) {
            e.dnn = s.dnn;
        }
        e.request_type = s.request_type;
        e.access_type = s.access_type;
        e.location = s.location;
        const auto direction = failure.initiator == xiri::SmInitiator::Ue
                                   ? li_core::PayloadDirection::FromTarget
                                   : (failure.initiator == xiri::SmInitiator::Network
                                          ? li_core::PayloadDirection::ToTarget
                                          : li_core::PayloadDirection::Unknown);
        impl_->emit(m, e, direction, "SMFUnsuccessfulProcedure");
    }
}

namespace {

std::optional<std::array<std::uint8_t, 3>> sd_from_hex(const std::string& hex) {
    if (hex.size() != 6) {
        return std::nullopt;
    }
    std::array<std::uint8_t, 3> out{};
    for (std::size_t i = 0; i < 3; ++i) {
        out[i] = static_cast<std::uint8_t>(std::stoi(hex.substr(i * 2, 2), nullptr, 16));
    }
    return out;
}

xiri::SmRequestType request_type_from(const std::string& s) {
    if (s == "EXISTING_PDU_SESSION") {
        return xiri::SmRequestType::ExistingPduSession;
    }
    if (s == "INITIAL_EMERGENCY_REQUEST") {
        return xiri::SmRequestType::InitialEmergencyRequest;
    }
    if (s == "EXISTING_EMERGENCY_PDU_SESSION") {
        return xiri::SmRequestType::ExistingEmergencyPduSession;
    }
    if (s == "MODIFICATION_REQUEST") {
        return xiri::SmRequestType::ModificationRequest;
    }
    if (s == "MA_PDU_REQUEST") {
        return xiri::SmRequestType::MaPduRequest;
    }
    // TS 29.502 makes requestType OPTIONAL in SmContextCreateData and the AMF of this build does
    // not send it; a CreateSMContext without one is a new session, so INITIAL_REQUEST (disclosed
    // default).
    return xiri::SmRequestType::InitialRequest;
}

} // namespace

LiSession li_session_from_create(const nlohmann::json& b) {
    LiSession s;
    s.supi = b.value("supi", std::string{});
    if (b.contains("pei") && b.at("pei").is_string()) {
        s.pei = b.at("pei").get<std::string>();
    }
    if (b.contains("gpsi") && b.at("gpsi").is_string()) {
        s.gpsi = b.at("gpsi").get<std::string>();
    }
    s.pdu_session_id = static_cast<std::uint8_t>(b.value("pduSessionId", 0));
    s.dnn = b.value("dnn", std::string{});
    if (b.contains("sNssai") && b.at("sNssai").is_object()) {
        xiri::Snssai sn;
        sn.sst = static_cast<std::uint8_t>(b.at("sNssai").value("sst", 0));
        if (b.at("sNssai").contains("sd") && b.at("sNssai").at("sd").is_string()) {
            sn.sd = sd_from_hex(b.at("sNssai").at("sd").get<std::string>());
        }
        s.snssai = sn;
    }
    s.request_type = request_type_from(b.value("requestType", std::string{}));
    const std::string an = b.value("anType", std::string{});
    s.access_type = an == "NON_3GPP_ACCESS" ? xiri::AccessType::NonThreeGppAccess
                                            : xiri::AccessType::ThreeGppAccess;
    return s;
}

void li_set_tunnel(LiSession& session, const nlohmann::json& ctx) {
    if (!ctx.contains("ulTeid")) {
        return;
    }
    xiri::Fteid f;
    f.teid = ctx.at("ulTeid").get<std::uint32_t>();
    if (ctx.contains("ulIpv4") && ctx.at("ulIpv4").is_array() && ctx.at("ulIpv4").size() == 4) {
        std::array<std::uint8_t, 4> ip{};
        for (std::size_t i = 0; i < 4; ++i) {
            ip[i] = ctx.at("ulIpv4").at(i).get<std::uint8_t>();
        }
        f.ipv4 = ip;
    }
    session.gtp_tunnel = f;
}

nlohmann::json li_context_record(const LiSession& s) {
    nlohmann::json j{{"pduSessionId", s.pdu_session_id},
                     {"dnn", s.dnn},
                     {"requestType", static_cast<int>(s.request_type)},
                     {"accessType", s.access_type ? static_cast<int>(*s.access_type) : 1}};
    if (s.pei) {
        j["pei"] = *s.pei;
    }
    if (s.gpsi) {
        j["gpsi"] = *s.gpsi;
    }
    if (s.snssai) {
        j["sst"] = s.snssai->sst;
        if (s.snssai->sd) {
            j["sd"] = std::vector<std::uint8_t>(s.snssai->sd->begin(), s.snssai->sd->end());
        }
    }
    return j;
}

LiSession li_session_from_context(const nlohmann::json& ctx) {
    LiSession s;
    s.supi = ctx.value("supi", std::string{});
    if (ctx.contains("li") && ctx.at("li").is_object()) {
        const auto& li = ctx.at("li");
        s.pdu_session_id = static_cast<std::uint8_t>(li.value("pduSessionId", 0));
        s.dnn = li.value("dnn", std::string{});
        s.request_type = static_cast<xiri::SmRequestType>(li.value("requestType", 1));
        s.access_type = static_cast<xiri::AccessType>(li.value("accessType", 1));
        if (li.contains("pei")) {
            s.pei = li.at("pei").get<std::string>();
        }
        if (li.contains("gpsi")) {
            s.gpsi = li.at("gpsi").get<std::string>();
        }
        if (li.contains("sst")) {
            xiri::Snssai sn;
            sn.sst = li.at("sst").get<std::uint8_t>();
            if (li.contains("sd") && li.at("sd").is_array() && li.at("sd").size() == 3) {
                std::array<std::uint8_t, 3> sd{};
                for (std::size_t i = 0; i < 3; ++i) {
                    sd[i] = li.at("sd").at(i).get<std::uint8_t>();
                }
                sn.sd = sd;
            }
            s.snssai = sn;
        }
    }
    li_set_tunnel(s, ctx);
    return s;
}

} // namespace smf
