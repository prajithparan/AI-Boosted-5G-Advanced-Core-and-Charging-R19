#include "li_poi.hpp"

#include <spdlog/spdlog.h>

#include <algorithm>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include "li_core/x1.hpp"
#include "li_core/x2x3_pdu.hpp"
#include "li_core/xiri.hpp"
#include "li_poi/poi_runtime.hpp"

namespace amf {

namespace {

// A target identifier that names a subscriber (as opposed to equipment or an address): the kinds
// the AMF can match a registering UE's SUPI against.
bool is_subscriber_kind(li_core::x1::TargetIdentifierKind kind) {
    using K = li_core::x1::TargetIdentifierKind;
    return kind == K::SupiImsi || kind == K::SupiNai || kind == K::Imsi || kind == K::Nai;
}

// The AMF carries a SUPI in the SBI "imsi-<digits>" form; X1 targets and the xIRI want the bare
// identity. Strip the leading scheme so both sides compare on the same string.
std::string bare_identity(const std::string& s) {
    for (const char* prefix : {"imsi-", "nai-", "supi-"}) {
        const std::size_t len = std::strlen(prefix);
        if (s.size() >= len && s.compare(0, len, prefix) == 0) {
            return s.substr(len);
        }
    }
    return s;
}

IdentifierAssociationGating
gating_of(const std::optional<li_core::x1::IdentifierAssociationEventsGenerated>& events) {
    if (!events) {
        return IdentifierAssociationGating::Absent;
    }
    switch (*events) {
        case li_core::x1::IdentifierAssociationEventsGenerated::IdentifierAssociation:
            return IdentifierAssociationGating::IdentifierAssociation;
        case li_core::x1::IdentifierAssociationEventsGenerated::All:
            return IdentifierAssociationGating::All;
    }
    return IdentifierAssociationGating::Absent;
}

const char* record_name(AmfXiriRecord record) {
    switch (record) {
        case AmfXiriRecord::Registration:
            return "AMFRegistration";
        case AmfXiriRecord::LocationUpdate:
            return "AMFLocationUpdate";
        case AmfXiriRecord::IdentifierAssociation:
            return "AMFIdentifierAssociation";
        case AmfXiriRecord::IdentifierDeassociation:
            return "AMFIdentifierDeassociation";
        case AmfXiriRecord::Deregistration:
            return "AMFDeregistration";
        case AmfXiriRecord::StartOfInterception:
            return "AMFStartOfInterceptionWithRegisteredUE";
    }
    return "AMF xIRI";
}

li_core::xiri::FiveGGuti to_xiri_guti(const GutiParts& guti) {
    li_core::xiri::FiveGGuti g;
    g.mcc = guti.mcc;
    g.mnc = guti.mnc;
    g.amf_region_id = guti.amf_region_id;
    g.amf_set_id = guti.amf_set_id;
    g.amf_pointer = guti.amf_pointer;
    g.five_g_tmsi = guti.five_g_tmsi;
    return g;
}

// Location.locationInfo.userLocation -- the form TS 33.128 tables 6.2.2.2.4-1 (form 1, NGAP
// source) and 6.2.2.2.7-1 prescribe.
li_core::xiri::Location to_xiri_location(const li_core::xiri::UserLocation& user_location) {
    li_core::xiri::Location loc;
    loc.user_location = user_location;
    return loc;
}

// "YYYYMMDDHHMMSSZ" -- a GeneralizedTime in UTC (TS 33.128 table 6.2.2.2.5-1 timeOfRegistration:
// "qualified with time zone information, i.e. as UTC or offset from UTC, not as local time").
std::string utc_generalized_time_now() {
    const std::time_t now = std::time(nullptr);
    std::tm tm{};
    gmtime_r(&now, &tm);
    char buf[32];
    std::strftime(buf, sizeof(buf), "%Y%m%d%H%M%SZ", &tm);
    return buf;
}

// The xIRI's SUPI: TS 33.128 NOTE 1 of tables 6.2.2.2.7-1/-2 -- "SUPI shall always be provided".
// This POI matches only SUPI/IMSI/NAI target kinds, so the matched identity IS the SUPI.
li_core::xiri::Supi xiri_supi(const li_core::x1::TargetIdentifier& target,
                              const std::string& bare) {
    if (target.kind == li_core::x1::TargetIdentifierKind::SupiNai ||
        target.kind == li_core::x1::TargetIdentifierKind::Nai) {
        return li_core::xiri::Nai{bare};
    }
    return li_core::xiri::Imsi{bare};
}

} // namespace

bool xiri_record_enabled(IdentifierAssociationGating gating, AmfXiriRecord record) {
    const bool identifier_record = record == AmfXiriRecord::IdentifierAssociation ||
                                   record == AmfXiriRecord::IdentifierDeassociation;
    switch (gating) {
        case IdentifierAssociationGating::Absent:
            return !identifier_record;
        case IdentifierAssociationGating::IdentifierAssociation:
            return identifier_record || record == AmfXiriRecord::LocationUpdate;
        case IdentifierAssociationGating::All:
            return true;
    }
    return false;
}

namespace {

// The identity kinds the AMF can match a registering UE's SUPI against.
const std::vector<li_core::x1::TargetIdentifierKind>& subscriber_kinds() {
    using K = li_core::x1::TargetIdentifierKind;
    static const std::vector<K> kinds{K::SupiImsi, K::SupiNai, K::Imsi, K::Nai};
    return kinds;
}

li_poi::Config runtime_config(const LiPoi::Config& c) {
    li_poi::Config r;
    r.x1_bind_address = c.x1_bind_address;
    r.x1_port = c.x1_port;
    r.ne_identifier = c.ne_identifier;
    r.network_function_id = c.network_function_id;
    r.interception_point_id = c.interception_point_id;
    r.mdf2_host = c.mdf2_host;
    r.mdf2_port = c.mdf2_port;
    r.mdf2_sni = c.mdf2_sni;
    r.cert_path = c.cert_path;
    r.key_path = c.key_path;
    r.ca_path = c.ca_path;
    r.x1_keepalive_p1_seconds = c.x1_keepalive_p1_seconds;
    r.x1_keepalive_p2_seconds = c.x1_keepalive_p2_seconds;
    r.x1_keepalive_p3_seconds = c.x1_keepalive_p3_seconds;
    r.x1_allow_deactivate_all = c.x1_allow_deactivate_all;
    return r;
}

} // namespace

struct LiPoi::Impl {
    // The generic POI machinery (ADR-0463) lives in libs/li-poi; this keeps what is AMF-specific.
    explicit Impl(Config cfg)
        : config(std::move(cfg)), runtime(runtime_config(config), make_hooks()) {}

    li_poi::Hooks make_hooks() {
        li_poi::Hooks h;
        h.log_name = "amf-li-poi";
        h.supported_kinds = subscriber_kinds();
        h.normalise = [](const std::string& s) { return bare_identity(s); };
        h.on_targets_added = [this](const std::string& xid,
                                    const std::vector<li_core::x1::TargetIdentifier>& added) {
            process(xid, added);
        };
        return h;
    }

    // The matched warrants for a SUPI, with the X1 gating mapped onto the AMF's record gating.
    struct Match {
        std::string xid;
        li_core::x1::TargetIdentifier target;
        IdentifierAssociationGating gating = IdentifierAssociationGating::Absent;
    };
    std::vector<Match> matches(const std::string& supi) const {
        std::vector<Match> out;
        for (const auto& m : runtime.matches(supi, subscriber_kinds())) {
            out.push_back(Match{m.xid, m.target, gating_of(m.identifier_association)});
        }
        return out;
    }

    // Build and send one xIRI for one matched task. Best-effort: failures are logged, never
    // propagated into the UE procedure.
    void emit(const Match& matched,
              const li_core::xiri::Event& event,
              li_core::PayloadDirection direction,
              AmfXiriRecord record) {
        const auto payload = li_core::xiri::encode_xiri_payload(event);
        if (!payload) {
            spdlog::error("amf-li-poi: could not encode {} xIRI for a target: {}",
                          record_name(record),
                          payload.error());
            return;
        }
        runtime.emit(li_poi::Match{matched.xid, matched.target, std::nullopt},
                     *payload,
                     direction,
                     record_name(record));
    }

    // --- registered-UE state + AMFStartOfInterceptionWithRegisteredUE (ADR-0461) ---------------
    struct RegisteredUe {
        GutiParts guti;
        std::optional<li_core::xiri::Location> location;
        std::string time_of_registration; // UTC GeneralizedTime, REGISTRATION ACCEPT sent
    };

    // One job: for every newly targeted subscriber identity that is 5GMM-REGISTERED right now,
    // emit one AMFStartOfInterceptionWithRegisteredUE under THIS task's XID only. Runs on the
    // runtime's worker thread, off the X1 request path.
    void process(const std::string& xid, const std::vector<li_core::x1::TargetIdentifier>& added) {
        for (const auto& target : added) {
            if (!std::any_of(subscriber_kinds().begin(), subscriber_kinds().end(), [&](auto k) {
                    return k == target.kind;
                })) {
                continue;
            }
            const std::string bare = bare_identity(target.value);
            RegisteredUe ue;
            {
                const std::lock_guard<std::mutex> lock(registry_mutex);
                const auto it = registered.find(bare);
                if (it == registered.end()) {
                    continue; // not registered: nothing to start with
                }
                ue = it->second;
            }
            const auto task = runtime.task(xid);
            if (!task) {
                return; // deactivated before the worker got here
            }
            const auto gating = gating_of(task->identifier_association);
            if (!xiri_record_enabled(gating, AmfXiriRecord::StartOfInterception)) {
                continue; // IdentifierAssociation-only warrant: "No other record types"
            }
            li_core::xiri::AmfStartOfInterceptionWithRegisteredUE soi;
            // 3GPP access only: this AMF has no N3IWF/TNGF path (disclosed).
            soi.registration_result = li_core::xiri::AmfRegistrationResult::ThreeGppAccess;
            soi.supi = xiri_supi(target, bare);
            soi.guti = to_xiri_guti(ue.guti);
            soi.location = ue.location;
            soi.time_of_registration = ue.time_of_registration;
            emit(Match{xid, target, gating},
                 soi,
                 li_core::PayloadDirection::NotApplicable, // 6.2.2.2.5: Direction Value 5
                 AmfXiriRecord::StartOfInterception);
        }
    }

    Config config;
    li_poi::PoiRuntime runtime;
    std::mutex registry_mutex;
    std::unordered_map<std::string, RegisteredUe> registered; // keyed by bare SUPI digits
};

LiPoi::LiPoi(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

LiPoi::~LiPoi() {
    stop();
}

void LiPoi::start() {
    impl_->runtime.start();
}

void LiPoi::stop() {
    impl_->runtime.stop();
}

void LiPoi::note_registered(const std::string& supi,
                            const GutiParts& guti,
                            const std::optional<li_core::xiri::UserLocation>& location) {
    Impl::RegisteredUe ue;
    ue.guti = guti;
    if (location) {
        ue.location = to_xiri_location(*location);
    }
    ue.time_of_registration = utc_generalized_time_now();
    const std::lock_guard<std::mutex> lock(impl_->registry_mutex);
    impl_->registered[bare_identity(supi)] = std::move(ue);
}

void LiPoi::note_location(const std::string& supi, const li_core::xiri::UserLocation& location) {
    const std::lock_guard<std::mutex> lock(impl_->registry_mutex);
    const auto it = impl_->registered.find(bare_identity(supi));
    if (it != impl_->registered.end()) {
        it->second.location = to_xiri_location(location);
    }
}

void LiPoi::note_deregistered(const std::string& supi) {
    const std::lock_guard<std::mutex> lock(impl_->registry_mutex);
    impl_->registered.erase(bare_identity(supi));
}

bool LiPoi::is_target(const std::string& supi) const {
    return !impl_->matches(supi).empty();
}

void LiPoi::report_registration(const std::string& supi, const GutiParts& guti) {
    const std::string bare = bare_identity(supi);
    for (const auto& matched : impl_->matches(supi)) {
        if (!xiri_record_enabled(matched.gating, AmfXiriRecord::Registration)) {
            continue; // IdentifierAssociation-only target: 6.2.2.2.1, "No other record types"
        }
        li_core::xiri::AmfRegistration reg;
        // TS 33.128 6.2.2.2.2 marks registrationType/Result M. The exact type
        // (initial/mobility/...) comes from the UE's RegistrationRequest; wiring that through the
        // AMF's Stage-2 parse is a refinement, so this reports the lab's case: a successful
        // initial 3GPP-access registration. Disclosed (ADR-0378 / docs/TRACEABILITY.md).
        reg.registration_type = li_core::xiri::AmfRegistrationType::Initial;
        reg.registration_result = li_core::xiri::AmfRegistrationResult::ThreeGppAccess;
        reg.supi = xiri_supi(matched.target, bare);
        reg.guti = to_xiri_guti(guti);
        // UE-initiated procedure: table 5.3.2-1 sets the direction from the initiator.
        impl_->emit(
            matched, reg, li_core::PayloadDirection::FromTarget, AmfXiriRecord::Registration);
    }
}

void LiPoi::report_identifier_association(const std::string& supi,
                                          const GutiParts& guti,
                                          const li_core::xiri::UserLocation& location) {
    const std::string bare = bare_identity(supi);
    for (const auto& matched : impl_->matches(supi)) {
        if (!xiri_record_enabled(matched.gating, AmfXiriRecord::IdentifierAssociation)) {
            continue; // table 6.2.2.1.1-1: extension absent -> never generated
        }
        li_core::xiri::AmfIdentifierAssociation assoc;
        assoc.supi = xiri_supi(matched.target, bare);
        assoc.guti = to_xiri_guti(guti);
        assoc.location = to_xiri_location(location);
        // 6.2.2.2.7: "shall set the Payload Direction field ... to not applicable (Direction
        // Value 5)".
        impl_->emit(matched,
                    assoc,
                    li_core::PayloadDirection::NotApplicable,
                    AmfXiriRecord::IdentifierAssociation);
    }
}

void LiPoi::report_location_update(const std::string& supi,
                                   const li_core::xiri::UserLocation& location) {
    const std::string bare = bare_identity(supi);
    for (const auto& matched : impl_->matches(supi)) {
        if (!xiri_record_enabled(matched.gating, AmfXiriRecord::LocationUpdate)) {
            continue;
        }
        li_core::xiri::AmfLocationUpdate update;
        update.supi = xiri_supi(matched.target, bare);
        update.location = to_xiri_location(location);
        // Clause 6.2.2.2.4 names no direction. Table 5.3.2-1 sets it from the procedure's
        // initiator; the N2 handover procedures that trigger this record are initiated by the
        // NG-RAN, not by the target UE and not as a message to it, so neither 2 (to target) nor
        // 3 (from target) describes it. Value 5 (not applicable) is used -- an interpretation,
        // disclosed in ADR-0440, consistent with 6.2.2.2.7's value for the other network-detected
        // AMF records.
        impl_->emit(matched,
                    update,
                    li_core::PayloadDirection::NotApplicable,
                    AmfXiriRecord::LocationUpdate);
    }
}

void LiPoi::report_deregistration(const std::string& supi,
                                  bool switch_off,
                                  const std::optional<GutiParts>& guti,
                                  const std::optional<li_core::xiri::UserLocation>& location) {
    const std::string bare = bare_identity(supi);
    for (const auto& matched : impl_->matches(supi)) {
        if (!xiri_record_enabled(matched.gating, AmfXiriRecord::Deregistration)) {
            continue; // IdentifierAssociation-only target: 6.2.2.2.1, "No other record types"
        }
        li_core::xiri::AmfDeregistration dereg;
        // UE-initiated only in this build (ADR-0393): this AMF implements no network-initiated
        // deregistration procedure, so table 5.3.2-1's direction is always the UE side.
        dereg.deregistration_direction = li_core::xiri::AmfDirection::UeInitiated;
        dereg.access_type = li_core::xiri::AccessType::ThreeGppAccess;
        dereg.supi = xiri_supi(matched.target, bare);
        if (guti) {
            dereg.guti = to_xiri_guti(*guti);
        }
        if (location) {
            dereg.location = to_xiri_location(*location);
        }
        dereg.switch_off_indicator =
            switch_off ? li_core::xiri::AmfDeregistration::SwitchOff::SwitchOff
                       : li_core::xiri::AmfDeregistration::SwitchOff::NormalDetach;
        // Clause 6.2.2.2.3 names no direction; table 5.3.2-1 sets it from the initiator, which is
        // the target UE for every trigger this AMF implements -- same convention as
        // report_registration's own UE-initiated case.
        impl_->emit(
            matched, dereg, li_core::PayloadDirection::FromTarget, AmfXiriRecord::Deregistration);
    }
}

void LiPoi::report_identifier_deassociation(
    const std::string& supi,
    const GutiParts& guti,
    const std::optional<li_core::xiri::UserLocation>& location) {
    const std::string bare = bare_identity(supi);
    for (const auto& matched : impl_->matches(supi)) {
        if (!xiri_record_enabled(matched.gating, AmfXiriRecord::IdentifierDeassociation)) {
            continue; // table 6.2.2.1.1-1: extension absent -> never generated
        }
        li_core::xiri::AmfIdentifierDeassociation deassoc;
        deassoc.supi = xiri_supi(matched.target, bare);
        deassoc.guti = to_xiri_guti(guti);
        if (location) {
            deassoc.location = to_xiri_location(*location);
        }
        // Clause 6.2.2.2.7 (the deassociation half): "shall set the Payload Direction field ...
        // to not applicable (Direction Value 5)" -- same value the association half already uses.
        impl_->emit(matched,
                    deassoc,
                    li_core::PayloadDirection::NotApplicable,
                    AmfXiriRecord::IdentifierDeassociation);
    }
}

} // namespace amf
