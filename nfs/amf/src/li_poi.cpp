#include "li_poi.hpp"

#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"

#include <boost/asio/io_context.hpp>
#include <spdlog/spdlog.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstring>
#include <ctime>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "li_core/x1.hpp"
#include "li_core/x1_server.hpp"
#include "li_core/x2x3_client.hpp"
#include "li_core/x2x3_pdu.hpp"
#include "li_core/xiri.hpp"

namespace amf {

namespace {

constexpr const char* kX1Path = "/X1/NE";

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

// TS 103 221-2 clause 5.2.7 carries the XID as a 128-bit integer; the X1 XSD writes it as a UUID
// string. Parse the UUID's hex (ignoring dashes) into the 16 octets the PDU needs.
std::optional<std::array<std::uint8_t, 16>> uuid_to_bytes(const std::string& uuid) {
    std::array<std::uint8_t, 16> out{};
    std::size_t n = 0;
    int high = -1;
    for (const char c : uuid) {
        if (c == '-') {
            continue;
        }
        int v = 0;
        if (c >= '0' && c <= '9') {
            v = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            v = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            v = c - 'A' + 10;
        } else {
            return std::nullopt;
        }
        if (high < 0) {
            high = v;
        } else {
            if (n >= out.size()) {
                return std::nullopt;
            }
            out[n++] = static_cast<std::uint8_t>((high << 4) | v);
            high = -1;
        }
    }
    if (n != out.size() || high != -1) {
        return std::nullopt;
    }
    return out;
}

IdentifierAssociationGating gating_of(const li_core::x1::TaskDetails& details) {
    if (!details.identifier_association_events) {
        return IdentifierAssociationGating::Absent;
    }
    switch (*details.identifier_association_events) {
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

struct LiPoi::Impl {
    explicit Impl(Config cfg)
        : config(std::move(cfg)), x2_client(make_x2_config(config)),
          keepalive(make_keepalive_config(config)) {}

    static li_core::X2X3ClientConfig make_x2_config(const Config& c) {
        li_core::X2X3ClientConfig x2;
        x2.host = c.mdf2_host;
        x2.port = c.mdf2_port;
        x2.client_cert_path = c.cert_path;
        x2.client_key_path = c.key_path;
        x2.ca_path = c.ca_path;
        x2.sni = c.mdf2_sni;
        return x2;
    }

    static li_core::x1::KeepaliveMonitor::Config make_keepalive_config(const Config& c) {
        li_core::x1::KeepaliveMonitor::Config k;
        k.time_p1 = std::chrono::seconds(c.x1_keepalive_p1_seconds);
        k.time_p2 = std::chrono::seconds(c.x1_keepalive_p2_seconds);
        k.time_p3 = std::chrono::seconds(c.x1_keepalive_p3_seconds);
        k.allow_deactivate_all = c.x1_allow_deactivate_all;
        return k;
    }

    // --- target store (provisioned over LI_X1) -----------------------------------------------
    // xid -> the warrant's target identifiers. Process-lifetime, ADMF-provisioned; entirely
    // separate from the AMF's per-UE state. A single configured MDF2 is the delivery destination,
    // so the X1 Destination operations are accepted but not used for routing (disclosed).
    std::optional<li_core::x1::ErrorCode> activate(const li_core::x1::TaskDetails& details) {
        if (details.xid.empty() || details.targets.empty()) {
            return li_core::x1::ErrorCode::GenericError;
        }
        const std::lock_guard<std::mutex> lock(store_mutex);
        if (tasks.count(details.xid) != 0) {
            return li_core::x1::ErrorCode::XidAlreadyExists; // table 6.7-3: 2010
        }
        tasks.emplace(details.xid, Task{details.targets, gating_of(details)});
        return std::nullopt;
    }

    std::optional<li_core::x1::ErrorCode> modify(const li_core::x1::TaskDetails& details) {
        if (details.xid.empty() || details.targets.empty()) {
            return li_core::x1::ErrorCode::GenericError;
        }
        const std::lock_guard<std::mutex> lock(store_mutex);
        // A ModifyTask carries a full TaskDetails, so it replaces the gating too: adding,
        // changing or (by omitting the extension) removing IdentifierAssociationExtensions.
        tasks[details.xid] = Task{details.targets, gating_of(details)};
        return std::nullopt;
    }

    std::optional<li_core::x1::ErrorCode> deactivate(const std::string& xid) {
        const std::lock_guard<std::mutex> lock(store_mutex);
        tasks.erase(xid);
        sequence.erase(xid);
        return std::nullopt;
    }

    std::optional<li_core::x1::ErrorCode> deactivate_all() {
        const std::lock_guard<std::mutex> lock(store_mutex);
        tasks.clear();
        sequence.clear();
        return std::nullopt;
    }

    // The matched warrant for a SUPI: its XID and the target identifier that matched (for the
    // X2 Matched Target Identifier attribute). nullopt if the SUPI is not a target.
    struct Match {
        std::string xid;
        li_core::x1::TargetIdentifier target;
        IdentifierAssociationGating gating = IdentifierAssociationGating::Absent;
    };
    // EVERY task that targets this SUPI (one match per task): two warrants on the same UE may
    // carry different gating, and each is reported under its own XID -- a first-hit lookup over
    // an unordered_map would make which gating applies nondeterministic.
    std::vector<Match> matches(const std::string& supi) const {
        const std::string bare = bare_identity(supi);
        std::vector<Match> out;
        const std::lock_guard<std::mutex> lock(store_mutex);
        for (const auto& [xid, task] : tasks) {
            for (const auto& target : task.targets) {
                if (is_subscriber_kind(target.kind) && bare_identity(target.value) == bare) {
                    out.push_back(Match{xid, target, task.gating});
                    break;
                }
            }
        }
        return out;
    }

    // Build and send one xIRI for one matched task. Best-effort: failures are logged, never
    // propagated into the UE procedure.
    void emit(const Match& matched,
              const li_core::xiri::Event& event,
              li_core::PayloadDirection direction,
              AmfXiriRecord record) {
        const auto xid_bytes = uuid_to_bytes(matched.xid);
        if (!xid_bytes) {
            spdlog::warn("amf-li-poi: task XID {} is not a UUID -- cannot build the X2 PDU",
                         matched.xid);
            return;
        }
        const auto payload = li_core::xiri::encode_xiri_payload(event);
        if (!payload) {
            spdlog::error("amf-li-poi: could not encode {} xIRI for a target: {}",
                          record_name(record),
                          payload.error());
            return;
        }
        li_core::Pdu pdu;
        pdu.type = li_core::PduType::X2;
        pdu.payload_format = li_core::PayloadFormat::Tgpp33128Payload;
        pdu.payload_direction = direction;
        pdu.xid = *xid_bytes;
        const auto now = static_cast<std::uint32_t>(std::time(nullptr));
        pdu.attributes.push_back(li_core::attr_sequence_number(next_sequence(matched.xid)));
        pdu.attributes.push_back(li_core::attr_network_function_id(config.network_function_id));
        pdu.attributes.push_back(
            li_core::attr_interception_point_id(config.interception_point_id));
        pdu.attributes.push_back(li_core::attr_timestamp(now, 0));
        pdu.attributes.push_back(li_core::attr_matched_target_identifier(
            "<" + matched.target.element + ">" + matched.target.value + "</" +
            matched.target.element + ">"));
        pdu.payload = *payload;

        if (const auto sent = x2_client.send(pdu); !sent) {
            // Best-effort: an intercept delivery failure is logged, never allowed to break the
            // UE's procedure on the network side.
            spdlog::error("amf-li-poi: LI_X2 delivery of an {} xIRI to the MDF2 failed: {}",
                          record_name(record),
                          sent.error());
            return;
        }
        spdlog::info(
            "amf-li-poi: delivered {} xIRI for target XID {}", record_name(record), matched.xid);
    }

    std::uint32_t next_sequence(const std::string& xid) {
        const std::lock_guard<std::mutex> lock(store_mutex);
        return sequence[xid]++;
    }

    li_core::x1::TaskStoreCallbacks make_callbacks() {
        li_core::x1::TaskStoreCallbacks cb;
        cb.ne_identifier = config.ne_identifier;
        cb.keepalive_supported = true;
        cb.activate_task = [this](const li_core::x1::TaskDetails& d) { return activate(d); };
        cb.modify_task = [this](const li_core::x1::TaskDetails& d) { return modify(d); };
        cb.deactivate_task = [this](const std::string& xid) { return deactivate(xid); };
        cb.deactivate_all_tasks = [this] { return deactivate_all(); };
        // A single configured MDF2 is the delivery destination; Destination provisioning is
        // accepted (a conformant NE answers it) but not used for routing in this slice.
        cb.create_destination = [](const li_core::x1::DestinationDetails&) {
            return std::optional<li_core::x1::ErrorCode>{};
        };
        cb.remove_destination = [](const std::string&) {
            return std::optional<li_core::x1::ErrorCode>{};
        };
        cb.remove_all_destinations = [] { return std::optional<li_core::x1::ErrorCode>{}; };
        return cb;
    }

    Config config;
    li_core::X2X3Client x2_client;

    struct Task {
        std::vector<li_core::x1::TargetIdentifier> targets;
        IdentifierAssociationGating gating = IdentifierAssociationGating::Absent;
    };
    mutable std::mutex store_mutex;
    std::unordered_map<std::string, Task> tasks;
    std::unordered_map<std::string, std::uint32_t> sequence;

    // LI_X1 listener: its own io_context on its own thread (the AMF's main io_context and NGAP
    // thread are separate).
    boost::asio::io_context x1_ioc;
    std::unique_ptr<sbi_core::http2::Server> x1_server;
    std::thread x1_thread;

    li_core::x1::KeepaliveMonitor keepalive;
    std::mutex keepalive_mutex;
    std::thread keepalive_thread;
    std::atomic<bool> running{false};
};

LiPoi::LiPoi(Config config) : impl_(std::make_unique<Impl>(std::move(config))) {}

LiPoi::~LiPoi() {
    stop();
}

void LiPoi::start() {
    const sbi_core::http2::TlsConfig tls{
        .cert_path = impl_->config.cert_path,
        .key_path = impl_->config.key_path,
        .ca_path = impl_->config.ca_path,
    };
    impl_->x1_server = std::make_unique<sbi_core::http2::Server>(
        impl_->x1_ioc, impl_->config.x1_bind_address, impl_->config.x1_port, tls);
    impl_->x1_server->add_route(
        "POST",
        kX1Path,
        [this, callbacks = impl_->make_callbacks()](const sbi_core::http2::Request& request) {
            {
                const std::lock_guard<std::mutex> lock(impl_->keepalive_mutex);
                impl_->keepalive.on_x1_request(std::chrono::steady_clock::now());
            }
            sbi_core::http2::Response response;
            response.status = 200; // X1-level errors ride in the body, not the HTTP status
            response.headers.emplace("content-type", "application/xml");
            response.body = li_core::x1::handle_request(request.body, callbacks);
            return response;
        });
    impl_->x1_server->start();
    sbi_core::stop_on_shutdown_signal(impl_->x1_ioc);

    impl_->running.store(true);
    impl_->x1_thread = std::thread([this] { impl_->x1_ioc.run(); });

    impl_->keepalive_thread = std::thread([this] {
        while (impl_->running.load()) {
            for (int i = 0; i < 10 && impl_->running.load(); ++i) {
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            if (!impl_->running.load()) {
                break;
            }
            li_core::x1::KeepaliveMonitor::Action action{};
            {
                const std::lock_guard<std::mutex> lock(impl_->keepalive_mutex);
                action = impl_->keepalive.tick(std::chrono::steady_clock::now());
            }
            switch (action) {
                case li_core::x1::KeepaliveMonitor::Action::SendFaultReport:
                    spdlog::error("amf-li-poi: no X1 request within TIME_P2 -- ReportNEIssue "
                                  "FaultReport is due to the ADMF (needs an X1 client)");
                    break;
                case li_core::x1::KeepaliveMonitor::Action::SendFaultCleared:
                    spdlog::info("amf-li-poi: X1 contact restored");
                    break;
                case li_core::x1::KeepaliveMonitor::Action::DeactivateAllTasks:
                    spdlog::error(
                        "amf-li-poi: TIME_P3 expired with no ADMF contact -- deactivating tasks");
                    impl_->deactivate_all();
                    break;
                case li_core::x1::KeepaliveMonitor::Action::None:
                    break;
            }
        }
    });

    spdlog::info("amf-li-poi: LI_X1 on https://{}:{}{} (TLS 1.3 + mTLS), LI_X2 to MDF2 {}:{}",
                 impl_->config.x1_bind_address,
                 impl_->config.x1_port,
                 kX1Path,
                 impl_->config.mdf2_host,
                 impl_->config.mdf2_port);
}

void LiPoi::stop() {
    if (!impl_->running.exchange(false)) {
        return;
    }
    impl_->x1_ioc.stop();
    if (impl_->x1_thread.joinable()) {
        impl_->x1_thread.join();
    }
    if (impl_->keepalive_thread.joinable()) {
        impl_->keepalive_thread.join();
    }
    impl_->x2_client.disconnect();
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

} // namespace amf
