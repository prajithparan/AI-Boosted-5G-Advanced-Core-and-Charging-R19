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

} // namespace

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
        tasks.emplace(details.xid, details.targets);
        return std::nullopt;
    }

    std::optional<li_core::x1::ErrorCode> modify(const li_core::x1::TaskDetails& details) {
        if (details.xid.empty() || details.targets.empty()) {
            return li_core::x1::ErrorCode::GenericError;
        }
        const std::lock_guard<std::mutex> lock(store_mutex);
        tasks[details.xid] = details.targets;
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
    };
    std::optional<Match> match(const std::string& supi) const {
        const std::string bare = bare_identity(supi);
        const std::lock_guard<std::mutex> lock(store_mutex);
        for (const auto& [xid, targets] : tasks) {
            for (const auto& target : targets) {
                if (is_subscriber_kind(target.kind) && bare_identity(target.value) == bare) {
                    return Match{xid, target};
                }
            }
        }
        return std::nullopt;
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

    mutable std::mutex store_mutex;
    std::unordered_map<std::string, std::vector<li_core::x1::TargetIdentifier>> tasks;
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
    return impl_->match(supi).has_value();
}

void LiPoi::report_registration(const std::string& supi, const GutiParts& guti) {
    const auto matched = impl_->match(supi);
    if (!matched) {
        return; // not a target
    }
    const auto xid_bytes = uuid_to_bytes(matched->xid);
    if (!xid_bytes) {
        spdlog::warn("amf-li-poi: task XID {} is not a UUID -- cannot build the X2 PDU",
                     matched->xid);
        return;
    }

    const std::string bare = bare_identity(supi);
    li_core::xiri::AmfRegistration reg;
    // TS 33.128 6.2.2.2.2 marks registrationType/Result M. The exact type (initial/mobility/...)
    // comes from the UE's RegistrationRequest; wiring that through the AMF's Stage-2 parse is a
    // refinement, so this slice reports the lab's case: a successful initial 3GPP-access
    // registration. Disclosed (ADR-0377 / docs/TRACEABILITY.md).
    reg.registration_type = li_core::xiri::AmfRegistrationType::Initial;
    reg.registration_result = li_core::xiri::AmfRegistrationResult::ThreeGppAccess;
    if (matched->target.kind == li_core::x1::TargetIdentifierKind::SupiNai ||
        matched->target.kind == li_core::x1::TargetIdentifierKind::Nai) {
        reg.supi = li_core::xiri::Nai{bare};
    } else {
        reg.supi = li_core::xiri::Imsi{bare};
    }
    reg.guti.mcc = guti.mcc;
    reg.guti.mnc = guti.mnc;
    reg.guti.amf_region_id = guti.amf_region_id;
    reg.guti.amf_set_id = guti.amf_set_id;
    reg.guti.amf_pointer = guti.amf_pointer;
    reg.guti.five_g_tmsi = guti.five_g_tmsi;

    const auto payload = li_core::xiri::encode_xiri_payload(reg);
    if (!payload) {
        spdlog::error("amf-li-poi: could not encode AMFRegistration xIRI for a target: {}",
                      payload.error());
        return;
    }

    li_core::Pdu pdu;
    pdu.type = li_core::PduType::X2;
    pdu.payload_format = li_core::PayloadFormat::Tgpp33128Payload;
    pdu.payload_direction = li_core::PayloadDirection::FromTarget;
    pdu.xid = *xid_bytes;
    const auto now = static_cast<std::uint32_t>(std::time(nullptr));
    pdu.attributes.push_back(li_core::attr_sequence_number(impl_->next_sequence(matched->xid)));
    pdu.attributes.push_back(li_core::attr_network_function_id(impl_->config.network_function_id));
    pdu.attributes.push_back(
        li_core::attr_interception_point_id(impl_->config.interception_point_id));
    pdu.attributes.push_back(li_core::attr_timestamp(now, 0));
    pdu.attributes.push_back(li_core::attr_matched_target_identifier(
        "<" + matched->target.element + ">" + matched->target.value + "</" +
        matched->target.element + ">"));
    pdu.payload = *payload;

    if (const auto sent = impl_->x2_client.send(pdu); !sent) {
        // Best-effort: an intercept delivery failure is logged and counted, never allowed to
        // break the UE's registration on the network side.
        spdlog::error(
            "amf-li-poi: LI_X2 delivery of an AMFRegistration xIRI to the MDF2 failed: {}",
            sent.error());
        return;
    }
    spdlog::info("amf-li-poi: delivered AMFRegistration xIRI for target XID {}", matched->xid);
}

} // namespace amf
