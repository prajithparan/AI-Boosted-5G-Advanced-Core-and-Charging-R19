// MDF2 -- the Mediation and Delivery Function for IRI of TS 33.127 clause 5.3.4 (ADR-0377).
//
// This is the process the three li_core libraries of increment 3 were built for. It terminates
// three interfaces and owns no others:
//
//   LI_X1  (ETSI TS 103 221-1, XML over HTTP/2 + mTLS, clause 7.2.2.3)  -- the ADMF provisions
//          warrants here: ActivateTask/ModifyTask/DeactivateTask/DeactivateAllTasks,
//          CreateDestination/RemoveDestination/RemoveAllDestinations, Keepalive, Ping.
//   LI_X2  (ETSI TS 103 221-2 PDUs over mTLS, TS 33.128 clause 5.3)     -- IRI-POIs stream xIRIs
//          in; each PDU carries the XID the POI was provisioned with, which is how a record finds
//          its warrant.
//   LI_HI2 (ETSI TS 102 232-1 PS-PDUs over TLS, TS 33.128 clause 5.5)   -- the mediated IRI
//          records go out to the LEMF, one Delivery Function per provisioned Destination.
//
// Deliberately NOT an SBI NF: it does not register with the NRF and exposes no 3GPP service API.
// TS 33.127 keeps the LI architecture off the service-based plane -- the NRF's LI support is the
// SIRF (clause 5.3.6), which tells the LIPF what exists; it does not make the MDF discoverable.
//
// State lives in Valkey (task_store.hpp), not in this process, so a second replica sees the same
// warrants (ADR-0359).

#include "sbi_core/http2_server.hpp"
#include "sbi_core/io_context_pool.hpp"
#include "sbi_core/logging.hpp"
#include "sbi_core/metrics.hpp"
#include "sbi_core/otel.hpp"

#include <boost/asio/io_context.hpp>
#include <nlohmann/json.hpp>
#include <spdlog/spdlog.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <sw/redis++/redis++.h>
#include <thread>
#include <vector>

#include "li_core/hi2.hpp"
#include "li_core/hi2_client.hpp"
#include "li_core/x1.hpp"
#include "li_core/x1_server.hpp"
#include "li_core/x2x3_pdu.hpp"
#include "li_core/x2x3_server.hpp"
#include "nf_config/nf_config.hpp"
#include "task_store.hpp"

namespace {

// TS 103 221-1 clause 7.2.2.2: the ADMF posts X1 requests to this path on the NE.
constexpr const char* kX1Path = "/X1/NE";

// TS 103 221-2 clause 5.2.7 carries the XID as a 128-bit integer; the X1 XSD writes it as a UUID
// string. One rendering, used in both directions, so a task provisioned as
// "a0a1a2a3-a4a5-a6a7-a8a9-aaabacadaeaf" is found by the PDU whose XID octets are those bytes.
std::string xid_to_uuid(const std::array<std::uint8_t, 16>& xid) {
    static const char* hex = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (std::size_t i = 0; i < xid.size(); ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            out.push_back('-');
        }
        out.push_back(hex[xid[i] >> 4]);
        out.push_back(hex[xid[i] & 0x0FU]);
    }
    return out;
}

} // namespace

int main() {
    sbi_core::init_logging("li-mdf");
    const auto config = nf_config::load("li-mdf", CONFIG_DIR);

    const auto x1_port = nf_config::require<std::uint16_t>(config, "x1_port", "LI_MDF_X1_PORT");
    const auto x2x3_port =
        nf_config::require<std::uint16_t>(config, "x2x3_port", "LI_MDF_X2X3_PORT");
    const auto metrics_bind_address = nf_config::require<std::string>(
        config, "metrics_bind_address", "LI_MDF_METRICS_BIND_ADDRESS");
    const auto redis_url = nf_config::require<std::string>(config, "redis_url", "LI_MDF_REDIS_URL");
    const auto ne_identifier =
        nf_config::require<std::string>(config, "ne_identifier", "LI_MDF_NE_IDENTIFIER");

    const auto& network = config.at("network_identifier");
    const auto operator_identifier = network.at("operator_identifier").get<std::string>();
    const auto network_element_identifier =
        network.at("network_element_identifier").get<std::string>();
    const auto delivery_country_code = config.at("delivery_country_code").get<std::string>();
    const auto authorization_country_code =
        config.at("authorization_country_code").get<std::string>();
    const auto interception_point_id = config.at("interception_point_id").get<std::string>();

    const auto& x2x3_config = config.at("x2x3");
    const auto& hi2_config = config.at("hi2");
    const auto& keepalive_config = config.at("x1_keepalive");

    sbi_core::init_metrics(metrics_bind_address);
    spdlog::info("li-mdf: starting, neIdentifier={}", ne_identifier);

    sw::redis::Redis redis(redis_url);
    redis.ping(); // fail fast if the warrant store is not there -- an MDF2 without it is blind
    li_mdf::TaskStore store(redis);

    auto meter = sbi_core::get_meter("li-mdf");
    auto x2_received = meter->CreateUInt64Counter("li_mdf_x2_pdus_received_total",
                                                  "LI_X2 PDUs received from IRI-POIs");
    auto x2_unmatched = meter->CreateUInt64Counter(
        "li_mdf_x2_unmatched_total", "LI_X2 PDUs whose XID matches no provisioned task");
    auto mediation_failed = meter->CreateUInt64Counter(
        "li_mdf_mediation_failed_total", "xIRIs that could not be mediated into an IRI record");
    auto hi2_delivered =
        meter->CreateUInt64Counter("li_mdf_hi2_records_handed_to_delivery_total",
                                   "Mediated IRI records handed to a Delivery Function");
    auto hi2_undeliverable = meter->CreateUInt64Counter(
        "li_mdf_hi2_undeliverable_total",
        "Mediated IRI records with no usable Destination to deliver them to");

    // One TS 102 232-1 Delivery Function per provisioned Destination, created on first use and
    // kept for the life of the process: the clause-6.3 DF owns a persistent connection and a
    // cyclic buffer, so tearing one down per record would defeat both.
    std::mutex delivery_mutex;
    std::map<std::string, std::unique_ptr<li_core::Hi2Client>> deliveries;

    const auto keepalive_header = [&] {
        li_core::hi2::PsHeader header;
        // Clause 6.3.4: on a keep-alive only the timestamp and version must be "set
        // appropriately"; the rest may be any value. These are the MDF's own identifiers, so a
        // LEMF sees a coherent header rather than filler.
        header.liid = ne_identifier;
        header.communication_identifier.operator_identifier = operator_identifier;
        header.communication_identifier.network_element_identifier = network_element_identifier;
        return header;
    }();

    const auto delivery_for = [&](const li_mdf::Destination& destination) -> li_core::Hi2Client* {
        const std::lock_guard<std::mutex> lock(delivery_mutex);
        auto it = deliveries.find(destination.did);
        if (it != deliveries.end()) {
            return it->second.get();
        }
        li_core::Hi2ClientConfig cfg;
        cfg.host = destination.host;
        cfg.port = destination.port;
        cfg.client_cert_path = CERTS_DIR "/li-mdf/cert.pem";
        cfg.client_key_path = CERTS_DIR "/li-mdf/key.pem";
        cfg.ca_path = CERTS_DIR "/ca/ca.crt";
        cfg.sni = hi2_config.value("sni", std::string{});
        cfg.reconnect_interval =
            std::chrono::seconds(hi2_config.at("reconnect_interval_seconds").get<int>());
        cfg.keepalive_time1 =
            std::chrono::seconds(hi2_config.at("keepalive_time1_seconds").get<int>());
        cfg.keepalive_time3 =
            std::chrono::seconds(hi2_config.at("keepalive_time3_seconds").get<int>());
        cfg.buffer_capacity_bytes = hi2_config.at("buffer_capacity_bytes").get<std::size_t>();

        const std::string did = destination.did;
        auto client = std::make_unique<li_core::Hi2Client>(
            cfg, keepalive_header, [did](li_core::Hi2Event event, std::string_view detail) {
                // Clause 6.3.2/6.3.3 want these "reported to the Handover Manager"; this project
                // has none, so they are logged and counted (ADR-0376).
                switch (event) {
                    case li_core::Hi2Event::Connected:
                        spdlog::info("li-mdf: HI2 to destination {} connected ({})", did, detail);
                        break;
                    case li_core::Hi2Event::ConnectFailed:
                        spdlog::warn(
                            "li-mdf: HI2 to destination {} could not connect: {}", did, detail);
                        break;
                    case li_core::Hi2Event::Disconnected:
                        spdlog::warn("li-mdf: HI2 to destination {} dropped: {}", did, detail);
                        break;
                    case li_core::Hi2Event::BufferFull:
                        spdlog::error("li-mdf: HI2 to destination {}: {}", did, detail);
                        break;
                    case li_core::Hi2Event::KeepaliveTimeout:
                        spdlog::warn("li-mdf: HI2 to destination {}: {}", did, detail);
                        break;
                    case li_core::Hi2Event::Resynchronised:
                        spdlog::info("li-mdf: HI2 to destination {}: {}", did, detail);
                        break;
                }
            });
        client->start();
        auto* raw = client.get();
        deliveries.emplace(destination.did, std::move(client));
        return raw;
    };

    // ---- LI_X2: receive, mediate, deliver -------------------------------------------------
    li_core::X2X3ServerConfig x2x3_server_config;
    x2x3_server_config.bind_address = x2x3_config.at("bind_address").get<std::string>();
    x2x3_server_config.port = x2x3_port;
    x2x3_server_config.server_cert_path = CERTS_DIR "/li-mdf/cert.pem";
    x2x3_server_config.server_key_path = CERTS_DIR "/li-mdf/key.pem";
    x2x3_server_config.ca_path = CERTS_DIR "/ca/ca.crt";
    x2x3_server_config.max_pdu_bytes = x2x3_config.at("max_pdu_bytes").get<std::uint32_t>();

    li_core::X2X3Server x2x3_server(
        x2x3_server_config, [&](const li_core::Pdu& pdu, std::string_view peer) {
            x2_received->Add(1);
            if (pdu.type != li_core::PduType::X2) {
                // An MDF2 mediates IRI. X3 content of communication belongs to the MDF3, which is a
                // later increment; refusing it is honest, dropping it silently would not be.
                spdlog::warn("li-mdf: {} sent an X3 PDU; this is an MDF2 (IRI only)", peer);
                return;
            }
            const std::string xid = xid_to_uuid(pdu.xid);
            const auto task = store.task(xid);
            if (!task) {
                x2_unmatched->Add(1);
                spdlog::warn(
                    "li-mdf: LI_X2 PDU from {} carries XID {}, which matches no provisioned "
                    "task -- dropped",
                    peer,
                    xid);
                return;
            }

            // One record per LIID (TS 103 221-1 5.1.2: an XID may map to several), each delivered
            // to that route's own destinations.
            bool delivered = false;
            for (const auto& route : task->routes) {
                if (route.delivery == li_core::x1::MediationDeliveryType::Hi3Only) {
                    continue; // that LIID takes content of communication only; MDF3's job
                }
                li_core::hi2::MediationContext context;
                context.liid = route.liid;
                context.communication_identifier.operator_identifier = operator_identifier;
                context.communication_identifier.network_element_identifier =
                    network_element_identifier;
                context.communication_identifier.delivery_country_code = delivery_country_code;
                context.authorization_country_code = authorization_country_code;
                context.interception_point_id = interception_point_id;
                context.sequence_number = store.next_sequence(route.liid);
                // TS 33.128 clause 5.5.5: identifiers from the X1 provisioning message are
                // "lEAProvided". The ones in the PDU's own Matched/Other attributes are added by
                // mediate_x2_pdu.
                context.target_identifiers = li_mdf::lea_provided(*task);
                // IRI-Type is per-event (table 5.5.2-1, "as specified in the relevant clause").
                // Until the POI increments carry the event-to-IRI-Type mapping, every record is
                // iRI-Report(4) -- the type for an event that is not a session begin/continue/end.
                // Disclosed, ADR-0377.
                context.iri_type = li_core::hi2::IriType::Report;

                const auto ps_pdu = li_core::hi2::mediate_x2_pdu(pdu, context);
                if (!ps_pdu) {
                    mediation_failed->Add(1);
                    spdlog::error("li-mdf: xIRI for XID {} (LIID {}) could not be mediated: {}",
                                  xid,
                                  route.liid,
                                  ps_pdu.error());
                    continue;
                }

                for (const auto& did : route.dids) {
                    const auto destination = store.destination(did);
                    if (!destination) {
                        spdlog::warn(
                            "li-mdf: task {} names destination {}, which is not provisioned",
                            xid,
                            did);
                        continue;
                    }
                    if (destination->delivery == li_core::x1::DeliveryType::X3Only) {
                        continue; // this Destination takes CC only
                    }
                    if (auto* client = delivery_for(*destination)) {
                        if (const auto sent = client->send(*ps_pdu); !sent) {
                            spdlog::error(
                                "li-mdf: destination {} refused the record: {}", did, sent.error());
                            continue;
                        }
                        delivered = true;
                    }
                }
            }
            if (delivered) {
                hi2_delivered->Add(1);
            } else {
                hi2_undeliverable->Add(1);
                spdlog::error("li-mdf: mediated record for XID {} has no usable destination", xid);
            }
        });

    if (const auto started = x2x3_server.start(); !started) {
        spdlog::critical(
            "li-mdf: cannot listen for LI_X2 on port {}: {}", x2x3_port, started.error());
        return 1;
    }

    // ---- LI_X1: provisioning ---------------------------------------------------------------
    li_core::x1::TaskStoreCallbacks callbacks;
    callbacks.ne_identifier = ne_identifier;
    callbacks.keepalive_supported = true;
    callbacks.activate_task = [&](const li_core::x1::TaskDetails& details) {
        const auto error = store.activate(details);
        if (!error) {
            spdlog::info("li-mdf: task {} activated, {} target identifier(s), {} destination(s)",
                         details.xid,
                         details.targets.size(),
                         details.dids.size());
        }
        return error;
    };
    callbacks.modify_task = [&](const li_core::x1::TaskDetails& details) {
        return store.modify(details);
    };
    callbacks.deactivate_task = [&](const std::string& xid) { return store.deactivate(xid); };
    callbacks.deactivate_all_tasks = [&] { return store.deactivate_all(); };
    callbacks.create_destination = [&](const li_core::x1::DestinationDetails& details) {
        return store.create_destination(details);
    };
    callbacks.remove_destination = [&](const std::string& did) {
        return store.remove_destination(did);
    };
    callbacks.remove_all_destinations = [&] { return store.remove_all_destinations(); };

    li_core::x1::KeepaliveMonitor::Config keepalive;
    keepalive.time_p2 = std::chrono::seconds(keepalive_config.at("time_p2_seconds").get<int>());
    keepalive.time_p1 = std::chrono::seconds(keepalive_config.at("time_p1_seconds").get<int>());
    keepalive.time_p3 = std::chrono::seconds(keepalive_config.at("time_p3_seconds").get<int>());
    keepalive.allow_deactivate_all = keepalive_config.at("allow_deactivate_all").get<bool>();
    li_core::x1::KeepaliveMonitor monitor(keepalive);
    std::mutex monitor_mutex;

    sbi_core::http2::TlsConfig x1_tls{
        .cert_path = CERTS_DIR "/li-mdf/cert.pem",
        .key_path = CERTS_DIR "/li-mdf/key.pem",
        .ca_path = CERTS_DIR "/ca/ca.crt",
    };
    boost::asio::io_context ioc;
    sbi_core::http2::Server x1_server(ioc, "0.0.0.0", x1_port, x1_tls);
    x1_server.add_route("POST", kX1Path, [&](const sbi_core::http2::Request& request) {
        {
            const std::lock_guard<std::mutex> lock(monitor_mutex);
            monitor.on_x1_request(std::chrono::steady_clock::now());
        }
        sbi_core::http2::Response response;
        response.status = 200; // 7.2.2.2: X1-level errors ride in the body, not the HTTP status
        response.headers.emplace("content-type", "application/xml");
        response.body = li_core::x1::handle_request(request.body, callbacks);
        return response;
    });

    // Clause 6.6.2's timers run on their own thread: the NE asserts the ADMF is alive, and a long
    // silence is a fault it must report.
    std::atomic<bool> running{true};
    std::thread keepalive_thread([&] {
        while (running.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(10));
            li_core::x1::KeepaliveMonitor::Action action{};
            {
                const std::lock_guard<std::mutex> lock(monitor_mutex);
                action = monitor.tick(std::chrono::steady_clock::now());
            }
            switch (action) {
                case li_core::x1::KeepaliveMonitor::Action::SendFaultReport:
                    // ReportNEIssue back to the ADMF needs an X1 *client*, which lands with the
                    // ADMF increment; until then the fault is logged, not silently dropped.
                    spdlog::error("li-mdf: no X1 request within TIME_P2 -- ReportNEIssue "
                                  "FaultReport (code {}) is due to the ADMF",
                                  li_core::x1::KeepaliveMonitor::kKeepalivesNotReceived);
                    break;
                case li_core::x1::KeepaliveMonitor::Action::SendFaultCleared:
                    spdlog::info("li-mdf: X1 contact restored -- ReportNEIssue FaultCleared is "
                                 "due to the ADMF");
                    break;
                case li_core::x1::KeepaliveMonitor::Action::DeactivateAllTasks:
                    spdlog::error("li-mdf: TIME_P3 expired with no ADMF contact -- deactivating "
                                  "all tasks (code {})",
                                  li_core::x1::KeepaliveMonitor::kDatabaseCleared);
                    store.deactivate_all();
                    break;
                case li_core::x1::KeepaliveMonitor::Action::None:
                    break;
            }
        }
    });

    sbi_core::on_shutdown_signal([&] {
        running.store(false);
        x2x3_server.stop();
    });

    x1_server.start();
    spdlog::info("li-mdf: LI_X1 on https://0.0.0.0:{}{} (TLS 1.3 + mTLS)", x1_port, kX1Path);
    spdlog::info("li-mdf: LI_X2 on {}:{} (TLS 1.3 + mTLS)",
                 x2x3_server_config.bind_address,
                 x2x3_server.bound_port());
    spdlog::info("li-mdf: Prometheus metrics at http://{}/metrics", metrics_bind_address);
    sbi_core::run_multi_threaded(ioc);

    running.store(false);
    if (keepalive_thread.joinable()) {
        keepalive_thread.join();
    }
    x2x3_server.stop();
    return 0;
}
